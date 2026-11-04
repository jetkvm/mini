/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: MIT
 */

#include "dma_cache_fastpath.h"

#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"

#define DMA_CACHE_FASTPATH_MAX_REGIONS 8U
#define DMA_CACHE_FASTPATH_SLOT_BITS   8U
#define DMA_CACHE_FASTPATH_SLOT_MASK   ((1U << DMA_CACHE_FASTPATH_SLOT_BITS) - 1U)

/* The wrapper is reached from task and ISR paths on both HP cores. It must
 * never spin behind a preempted owner: a contended lookup simply falls back
 * to the stock cache operation for that call. Registry mutations remain
 * serialized by the same lock outside this high-rate path. */

typedef enum {
    REGION_FREE = 0,
    REGION_PREPARING,
    REGION_DEVICE_EXCLUSIVE,
    REGION_CPU_OR_FALLBACK,
} region_state_t;

typedef struct {
    void *address;
    size_t size;
    uint32_t generation;
    region_state_t state;
    uint64_t skipped_c2m_calls;
    uint64_t skipped_c2m_bytes;
    uint64_t skipped_m2c_calls;
    uint64_t skipped_m2c_bytes;
    uint64_t passthrough_calls;
    uint64_t unexpected_overlap_calls;
} fastpath_region_t;

static DRAM_ATTR fastpath_region_t s_regions[DMA_CACHE_FASTPATH_MAX_REGIONS];
static DRAM_ATTR dma_cache_fastpath_global_stats_t s_global_stats;
static DRAM_ATTR portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR bool s_enabled = true;
static uint32_t s_next_generation;

/* Supplied by GNU ld's --wrap=esp_cache_msync. */
esp_err_t __real_esp_cache_msync(void *addr, size_t size, int flags);

static dma_cache_fastpath_handle_t make_handle(size_t slot, uint32_t generation)
{
    return (generation << DMA_CACHE_FASTPATH_SLOT_BITS) | (uint32_t)(slot + 1U);
}

static bool decode_handle(dma_cache_fastpath_handle_t handle, size_t *out_slot,
                          uint32_t *out_generation)
{
    uint32_t encoded_slot = handle & DMA_CACHE_FASTPATH_SLOT_MASK;
    uint32_t generation = handle >> DMA_CACHE_FASTPATH_SLOT_BITS;
    if (encoded_slot == 0 || encoded_slot > DMA_CACHE_FASTPATH_MAX_REGIONS ||
        generation == 0) {
        return false;
    }
    *out_slot = encoded_slot - 1U;
    *out_generation = generation;
    return true;
}

static bool IRAM_ATTR ranges_overlap(const void *lhs_address, size_t lhs_size,
                                     const void *rhs_address, size_t rhs_size)
{
    uintptr_t lhs = (uintptr_t)lhs_address;
    uintptr_t rhs = (uintptr_t)rhs_address;
    if (lhs_size == 0 || rhs_size == 0 ||
        lhs_size > UINTPTR_MAX - lhs || rhs_size > UINTPTR_MAX - rhs) {
        return false;
    }
    return lhs < rhs + rhs_size && rhs < lhs + lhs_size;
}

static bool IRAM_ATTR request_flags_supported(int flags, bool *out_m2c)
{
    const int known_flags = ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                            ESP_CACHE_MSYNC_FLAG_UNALIGNED |
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                            ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                            ESP_CACHE_MSYNC_FLAG_TYPE_INST;
    if ((flags & ~known_flags) != 0 ||
        (flags & ESP_CACHE_MSYNC_FLAG_TYPE_INST) != 0) {
        return false;
    }

    int direction = flags & (ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                             ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    if (direction == (ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                      ESP_CACHE_MSYNC_FLAG_DIR_M2C)) {
        return false;
    }
    *out_m2c = direction == ESP_CACHE_MSYNC_FLAG_DIR_M2C;
    /* Preserve stock esp_cache_msync() argument semantics even on an exact
     * registered range: IDF rejects UNALIGNED for the M2C direction. */
    if (*out_m2c && (flags & ESP_CACHE_MSYNC_FLAG_UNALIGNED) != 0) {
        return false;
    }
    return true;
}

esp_err_t IRAM_ATTR __wrap_esp_cache_msync(void *addr, size_t size, int flags)
{
    bool skip = false;
    bool m2c = false;
    bool supported = request_flags_supported(flags, &m2c);

    if (portTRY_ENTER_CRITICAL_SAFE(&s_lock, portMUX_TRY_LOCK) != pdPASS) {
        return __real_esp_cache_msync(addr, size, flags);
    }
    if (s_enabled && size != 0) {
        for (size_t i = 0; i < DMA_CACHE_FASTPATH_MAX_REGIONS; i++) {
            fastpath_region_t *region = &s_regions[i];
            if (region->state == REGION_FREE ||
                region->state == REGION_PREPARING) {
                continue;
            }

            bool exact = addr == region->address && size == region->size;
            if (exact && supported &&
                region->state == REGION_DEVICE_EXCLUSIVE) {
                if (m2c) {
                    region->skipped_m2c_calls++;
                    region->skipped_m2c_bytes += size;
                    s_global_stats.skipped_m2c_calls++;
                    s_global_stats.skipped_m2c_bytes += size;
                } else {
                    region->skipped_c2m_calls++;
                    region->skipped_c2m_bytes += size;
                    s_global_stats.skipped_c2m_calls++;
                    s_global_stats.skipped_c2m_bytes += size;
                }
                skip = true;
                break;
            }

            if (ranges_overlap(addr, size, region->address, region->size)) {
                region->passthrough_calls++;
                s_global_stats.passthrough_calls++;
                if (region->state == REGION_DEVICE_EXCLUSIVE) {
                    /* A non-whitelisted access violates the exact whole-frame
                     * pattern. Fail closed: all later calls use real IDF cache
                     * maintenance until the owner explicitly rearms. */
                    region->state = REGION_CPU_OR_FALLBACK;
                    region->unexpected_overlap_calls++;
                    s_global_stats.unexpected_overlap_calls++;
                }
                break;
            }
        }
    }
    portEXIT_CRITICAL_SAFE(&s_lock);

    if (skip) {
        return ESP_OK;
    }
    return __real_esp_cache_msync(addr, size, flags);
}

esp_err_t dma_cache_fastpath_register(void *address, size_t size,
                                      dma_cache_fastpath_handle_t *out_handle)
{
    if (!address || size == 0 ||
        size > UINTPTR_MAX - (uintptr_t)address || !out_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = DMA_CACHE_FASTPATH_INVALID_HANDLE;

    size_t line_size = esp_cache_get_line_size_by_addr(address);
    if (line_size == 0 || ((uintptr_t)address % line_size) != 0 ||
        (size % line_size) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t slot = DMA_CACHE_FASTPATH_MAX_REGIONS;
    uint32_t generation = 0;
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < DMA_CACHE_FASTPATH_MAX_REGIONS; i++) {
        if (s_regions[i].state != REGION_FREE &&
            ranges_overlap(address, size, s_regions[i].address,
                           s_regions[i].size)) {
            portEXIT_CRITICAL(&s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (slot == DMA_CACHE_FASTPATH_MAX_REGIONS &&
            s_regions[i].state == REGION_FREE) {
            slot = i;
        }
    }
    if (slot == DMA_CACHE_FASTPATH_MAX_REGIONS) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }

    generation = ++s_next_generation;
    if (generation == 0 || generation > (UINT32_MAX >> DMA_CACHE_FASTPATH_SLOT_BITS)) {
        s_next_generation = 1;
        generation = 1;
    }
    s_regions[slot] = (fastpath_region_t) {
        .address = address,
        .size = size,
        .generation = generation,
        .state = REGION_PREPARING,
    };
    portEXIT_CRITICAL(&s_lock);

    esp_err_t ret = __real_esp_cache_msync(
        address, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                       ESP_CACHE_MSYNC_FLAG_INVALIDATE);

    portENTER_CRITICAL(&s_lock);
    if (ret == ESP_OK) {
        s_regions[slot].state = REGION_DEVICE_EXCLUSIVE;
        *out_handle = make_handle(slot, generation);
    } else {
        memset(&s_regions[slot], 0, sizeof(s_regions[slot]));
    }
    portEXIT_CRITICAL(&s_lock);
    return ret;
}

static esp_err_t get_region_for_transition(dma_cache_fastpath_handle_t handle,
                                           size_t *out_slot, void **out_address,
                                           size_t *out_size)
{
    size_t slot;
    uint32_t generation;
    if (!decode_handle(handle, &slot, &generation)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    fastpath_region_t *region = &s_regions[slot];
    if (region->state == REGION_FREE || region->state == REGION_PREPARING ||
        region->generation != generation) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *out_slot = slot;
    *out_address = region->address;
    *out_size = region->size;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t dma_cache_fastpath_cpu_acquire(dma_cache_fastpath_handle_t handle)
{
    size_t slot;
    void *address;
    size_t size;
    esp_err_t ret = get_region_for_transition(handle, &slot, &address, &size);
    if (ret != ESP_OK) {
        return ret;
    }

    portENTER_CRITICAL(&s_lock);
    s_regions[slot].state = REGION_CPU_OR_FALLBACK;
    portEXIT_CRITICAL(&s_lock);
    return __real_esp_cache_msync(address, size,
                                  ESP_CACHE_MSYNC_FLAG_DIR_M2C);
}

esp_err_t dma_cache_fastpath_device_rearm(dma_cache_fastpath_handle_t handle)
{
    size_t slot;
    void *address;
    size_t size;
    esp_err_t ret = get_region_for_transition(handle, &slot, &address, &size);
    if (ret != ESP_OK) {
        return ret;
    }

    portENTER_CRITICAL(&s_lock);
    s_regions[slot].state = REGION_CPU_OR_FALLBACK;
    portEXIT_CRITICAL(&s_lock);

    ret = __real_esp_cache_msync(address, size,
                                 ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                 ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    if (ret == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_regions[slot].state = REGION_DEVICE_EXCLUSIVE;
        portEXIT_CRITICAL(&s_lock);
    }
    return ret;
}

esp_err_t dma_cache_fastpath_unregister(dma_cache_fastpath_handle_t handle)
{
    size_t slot;
    uint32_t generation;
    if (!decode_handle(handle, &slot, &generation)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = dma_cache_fastpath_cpu_acquire(handle);
    if (ret != ESP_OK) {
        return ret;
    }

    portENTER_CRITICAL(&s_lock);
    if (s_regions[slot].generation == generation) {
        memset(&s_regions[slot], 0, sizeof(s_regions[slot]));
    } else {
        ret = ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_lock);
    return ret;
}

esp_err_t dma_cache_fastpath_get_stats(dma_cache_fastpath_handle_t handle,
                                       dma_cache_fastpath_stats_t *out_stats)
{
    if (!out_stats) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t slot;
    uint32_t generation;
    if (!decode_handle(handle, &slot, &generation)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    const fastpath_region_t *region = &s_regions[slot];
    if (region->state == REGION_FREE || region->state == REGION_PREPARING ||
        region->generation != generation) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *out_stats = (dma_cache_fastpath_stats_t) {
        .address = region->address,
        .size = region->size,
        .state = region->state == REGION_DEVICE_EXCLUSIVE ?
                 DMA_CACHE_FASTPATH_STATE_DEVICE_EXCLUSIVE :
                 DMA_CACHE_FASTPATH_STATE_CPU_OR_FALLBACK,
        .skipped_c2m_calls = region->skipped_c2m_calls,
        .skipped_c2m_bytes = region->skipped_c2m_bytes,
        .skipped_m2c_calls = region->skipped_m2c_calls,
        .skipped_m2c_bytes = region->skipped_m2c_bytes,
        .passthrough_calls = region->passthrough_calls,
        .unexpected_overlap_calls = region->unexpected_overlap_calls,
    };
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void dma_cache_fastpath_get_global_stats(dma_cache_fastpath_global_stats_t *out_stats)
{
    if (!out_stats) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out_stats = s_global_stats;
    portEXIT_CRITICAL(&s_lock);
}

void dma_cache_fastpath_set_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_lock);
    s_enabled = enabled;
    if (!enabled) {
        for (size_t i = 0; i < DMA_CACHE_FASTPATH_MAX_REGIONS; i++) {
            if (s_regions[i].state == REGION_DEVICE_EXCLUSIVE) {
                s_regions[i].state = REGION_CPU_OR_FALLBACK;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

bool dma_cache_fastpath_is_enabled(void)
{
    portENTER_CRITICAL(&s_lock);
    bool enabled = s_enabled;
    portEXIT_CRITICAL(&s_lock);
    return enabled;
}

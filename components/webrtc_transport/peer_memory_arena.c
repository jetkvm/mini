/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "peer_memory_arena.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "multi_heap.h"

static const char *TAG = "peer_memory";

static atomic_uintptr_t s_heap_pointer;
static atomic_uintptr_t s_arena_base;
static atomic_size_t s_arena_capacity;
static atomic_bool s_accepting_allocations;
static atomic_uint s_active_operations;
static atomic_uint s_allocation_failures;
static atomic_uint s_begin_failures;
static atomic_uint s_leak_events;
static atomic_bool s_teardown_failure_reported;
static portMUX_TYPE s_heap_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_detail_cache_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_detail_cache_generation;
static uint32_t s_detail_cache_largest;
static uint32_t s_detail_cache_allocated;
static uint64_t s_detail_cache_timestamp_us;

static void invalidate_detail_cache(void)
{
    portENTER_CRITICAL(&s_detail_cache_lock);
    ++s_detail_cache_generation;
    s_detail_cache_largest = 0;
    s_detail_cache_allocated = 0;
    s_detail_cache_timestamp_us = 0;
    portEXIT_CRITICAL(&s_detail_cache_lock);
}

static uint32_t clamp_u32(size_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static multi_heap_handle_t arena_heap(void)
{
    return (multi_heap_handle_t)atomic_load_explicit(
        &s_heap_pointer, memory_order_acquire);
}

static bool pointer_is_in_arena(const void *pointer)
{
    const uintptr_t base = atomic_load_explicit(
        &s_arena_base, memory_order_acquire);
    const size_t capacity = atomic_load_explicit(
        &s_arena_capacity, memory_order_acquire);
    const uintptr_t value = (uintptr_t)pointer;
    return base != 0U && value >= base && value - base < capacity;
}

static multi_heap_handle_t begin_arena_operation(bool allocation)
{
    multi_heap_handle_t heap = arena_heap();
    if (heap == NULL ||
        (allocation && !atomic_load_explicit(
                           &s_accepting_allocations,
                           memory_order_acquire))) {
        return NULL;
    }
    atomic_fetch_add_explicit(&s_active_operations, 1U,
                              memory_order_acq_rel);
    if (heap != arena_heap() ||
        (allocation && !atomic_load_explicit(
                           &s_accepting_allocations,
                           memory_order_acquire))) {
        atomic_fetch_sub_explicit(&s_active_operations, 1U,
                                  memory_order_acq_rel);
        return NULL;
    }
    return heap;
}

static void end_arena_operation(void)
{
    atomic_fetch_sub_explicit(&s_active_operations, 1U,
                              memory_order_release);
}

/* A retained arena is retried by the peer task. Count and print the ownership
 * failure once for this arena generation instead of turning that retry into a
 * 10-ms log/statistics storm. The fail-closed backing allocation remains
 * intact until every owner has released its block. */
static bool claim_teardown_failure_report(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &s_teardown_failure_reported, &expected, true,
            memory_order_acq_rel, memory_order_acquire)) {
        return false;
    }
    atomic_fetch_add_explicit(&s_leak_events, 1U, memory_order_relaxed);
    return true;
}

esp_err_t webrtc_peer_memory_arena_begin(size_t capacity_bytes)
{
    if (capacity_bytes == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load_explicit(&s_accepting_allocations,
                             memory_order_acquire)) {
        /* Never tear down an arena which still belongs to a live peer.  The
         * retry below is only for a prior close which stopped admission but
         * observed vendor blocks that were released slightly later. */
        return ESP_ERR_INVALID_STATE;
    }
    if (arena_heap() != NULL ||
        atomic_load_explicit(&s_arena_base, memory_order_acquire) != 0U) {
        /* A previous close may have observed a vendor block which was freed
         * slightly later. Retry the fail-closed teardown before rejecting a
         * new peer; live blocks remain protected and still reject begin. */
        esp_err_t cleanup = webrtc_peer_memory_arena_end();
        if (cleanup != ESP_OK) {
            /* The previous arena still owns physical PSRAM. Treat it as a
             * memory-admission failure so the transport uses its 30-second
             * memory backoff instead of rebuilding every scheduler tick. */
            return ESP_ERR_NO_MEM;
        }
    }

    void *base = heap_caps_aligned_alloc(
        64U, capacity_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (base == NULL) {
        atomic_fetch_add_explicit(&s_begin_failures, 1U,
                                  memory_order_relaxed);
        return ESP_ERR_NO_MEM;
    }
    multi_heap_handle_t heap = multi_heap_register(base, capacity_bytes);
    if (heap == NULL) {
        heap_caps_free(base);
        atomic_fetch_add_explicit(&s_begin_failures, 1U,
                                  memory_order_relaxed);
        return ESP_ERR_NO_MEM;
    }
    multi_heap_set_lock(heap, &s_heap_lock);
    invalidate_detail_cache();
    atomic_store_explicit(&s_teardown_failure_reported, false,
                          memory_order_release);
    atomic_store_explicit(&s_arena_capacity, capacity_bytes,
                          memory_order_release);
    atomic_store_explicit(&s_arena_base, (uintptr_t)base,
                          memory_order_release);
    atomic_store_explicit(&s_heap_pointer, (uintptr_t)heap,
                          memory_order_release);
    /* Prime the expensive details before the new peer starts allocating or
     * sending video. Live diagnostics only read this timestamped cache. */
    webrtc_peer_memory_arena_snapshot_t initial_snapshot;
    webrtc_peer_memory_arena_get_snapshot(&initial_snapshot);
    atomic_store_explicit(&s_accepting_allocations, true,
                          memory_order_release);
    ESP_LOGI(TAG, "peer arena ready capacity=%u", (unsigned)capacity_bytes);
    return ESP_OK;
}

esp_err_t webrtc_peer_memory_arena_end(void)
{
    multi_heap_handle_t heap = arena_heap();
    void *base = (void *)atomic_load_explicit(
        &s_arena_base, memory_order_acquire);
    if (heap == NULL || base == NULL) {
        return ESP_OK;
    }

    atomic_store_explicit(&s_accepting_allocations, false,
                          memory_order_release);
    for (unsigned attempt = 0U;
         atomic_load_explicit(&s_active_operations,
                              memory_order_acquire) != 0U &&
         attempt < 100U;
         ++attempt) {
        vTaskDelay(1);
    }
    if (atomic_load_explicit(&s_active_operations,
                             memory_order_acquire) != 0U) {
        if (claim_teardown_failure_report()) {
            ESP_LOGE(TAG, "peer arena still has active allocator operations");
        }
        return ESP_ERR_TIMEOUT;
    }

    multi_heap_info_t info = {0};
    multi_heap_get_info(heap, &info);
    if (info.allocated_blocks != 0U) {
        if (claim_teardown_failure_report()) {
            ESP_LOGE(TAG,
                     "peer arena retained blocks=%u bytes=%u; preserving backing memory",
                     (unsigned)info.allocated_blocks,
                     (unsigned)info.total_allocated_bytes);
        }
        return ESP_ERR_INVALID_STATE;
    }

    atomic_store_explicit(&s_heap_pointer, 0U, memory_order_release);
    atomic_store_explicit(&s_arena_base, 0U, memory_order_release);
    atomic_store_explicit(&s_arena_capacity, 0U, memory_order_release);
    invalidate_detail_cache();

    heap_caps_free(base);
    ESP_LOGI(TAG, "peer arena released");
    return ESP_OK;
}

static void get_snapshot(webrtc_peer_memory_arena_snapshot_t *snapshot,
                         bool detailed)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    multi_heap_handle_t heap = begin_arena_operation(false);
    if (heap != NULL) {
        snapshot->active = true;
        snapshot->capacity_bytes = clamp_u32(
            atomic_load_explicit(&s_arena_capacity,
                                 memory_order_acquire));
        portENTER_CRITICAL(&s_detail_cache_lock);
        uint32_t cache_generation = s_detail_cache_generation;
        snapshot->largest_free_block = s_detail_cache_largest;
        snapshot->allocated_blocks = s_detail_cache_allocated;
        snapshot->detailed_sample_timestamp_us = s_detail_cache_timestamp_us;
        portEXIT_CRITICAL(&s_detail_cache_lock);
        if (detailed) {
            multi_heap_info_t info = {0};
            multi_heap_get_info(heap, &info);
            snapshot->free_bytes = clamp_u32(info.total_free_bytes);
            snapshot->minimum_free_bytes = clamp_u32(info.minimum_free_bytes);
            snapshot->largest_free_block = clamp_u32(info.largest_free_block);
            snapshot->allocated_blocks = clamp_u32(info.allocated_blocks);
            snapshot->detailed_sample_timestamp_us =
                (uint64_t)esp_timer_get_time();
            /* A sample from a replaced arena must never repopulate the new
             * generation's cache, even if its backing address is reused. */
            portENTER_CRITICAL(&s_detail_cache_lock);
            if (cache_generation == s_detail_cache_generation &&
                heap == arena_heap()) {
                s_detail_cache_largest = snapshot->largest_free_block;
                s_detail_cache_allocated = snapshot->allocated_blocks;
                s_detail_cache_timestamp_us =
                    snapshot->detailed_sample_timestamp_us;
            }
            portEXIT_CRITICAL(&s_detail_cache_lock);
        } else {
            snapshot->free_bytes = clamp_u32(multi_heap_free_size(heap));
            snapshot->minimum_free_bytes = clamp_u32(
                multi_heap_minimum_free_size(heap));
        }
        end_arena_operation();
    }
    snapshot->allocation_failures = atomic_load_explicit(
        &s_allocation_failures, memory_order_relaxed);
    snapshot->begin_failures = atomic_load_explicit(
        &s_begin_failures, memory_order_relaxed);
    snapshot->leak_events = atomic_load_explicit(
        &s_leak_events, memory_order_relaxed);
}

void webrtc_peer_memory_arena_get_snapshot(
    webrtc_peer_memory_arena_snapshot_t *snapshot)
{
    get_snapshot(snapshot, true);
}

void webrtc_peer_memory_arena_get_snapshot_cached(
    webrtc_peer_memory_arena_snapshot_t *snapshot)
{
    get_snapshot(snapshot, false);
}

/* Outside an arena, which production never begins, media_lib memory is
 * general PSRAM. malloc() would put each block up to
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL bytes in internal RAM, and the data
 * channels' send and receive caches hold hundreds of such blocks (frame
 * records and small messages, kept until acknowledged): with OS Services
 * traffic they held 15-35 KB of internal RAM, which TLS admissions need.
 * media_lib_free() releases these blocks with libc free(), which accepts a
 * pointer from any heap. */
#define PEER_FALLBACK_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* Strong definitions replace the weak media_lib allocator in
 * third_party/esp_peer/src/media_lib_weak.c. esp_peer's media_lib calls use
 * this private heap; the RTP rolling buffer's xRingbufferCreateWithCaps()
 * allocation does not go through media_lib and keeps its own placement. */
void *media_lib_malloc(size_t size)
{
    if (size == 0U) {
        return NULL;
    }
    multi_heap_handle_t heap = begin_arena_operation(true);
    if (heap == NULL) {
        if (atomic_load_explicit(&s_arena_base,
                                 memory_order_acquire) != 0U) {
            /* Teardown has closed admission but intentionally retained the
             * backing block. Never let a late vendor allocation escape into
             * the global video heap during that window. */
            atomic_fetch_add_explicit(&s_allocation_failures, 1U,
                                      memory_order_relaxed);
            return NULL;
        }
        return heap_caps_malloc(size, PEER_FALLBACK_CAPS);
    }
    void *memory = multi_heap_malloc(heap, size);
    end_arena_operation();
    if (memory == NULL) {
        atomic_fetch_add_explicit(&s_allocation_failures, 1U,
                                  memory_order_relaxed);
    }
    return memory;
}

void *media_lib_calloc(size_t count, size_t size)
{
    if (count != 0U && size > SIZE_MAX / count) {
        atomic_fetch_add_explicit(&s_allocation_failures, 1U,
                                  memory_order_relaxed);
        return NULL;
    }
    const size_t bytes = count * size;
    void *memory = media_lib_malloc(bytes);
    if (memory != NULL) {
        memset(memory, 0, bytes);
    }
    return memory;
}

void media_lib_free(void *pointer)
{
    if (pointer == NULL) {
        return;
    }
    if (!pointer_is_in_arena(pointer)) {
        free(pointer);
        return;
    }
    multi_heap_handle_t heap = begin_arena_operation(false);
    if (heap == NULL) {
        /* Fail closed.  A pointer in the retained arena must never be handed
         * to libc even if teardown detected an asynchronous vendor leak. */
        atomic_fetch_add_explicit(&s_leak_events, 1U,
                                  memory_order_relaxed);
        return;
    }
    multi_heap_free(heap, pointer);
    end_arena_operation();
}

void *media_lib_realloc(void *pointer, size_t size)
{
    if (pointer == NULL) {
        return media_lib_malloc(size);
    }
    if (size == 0U) {
        media_lib_free(pointer);
        return NULL;
    }
    if (!pointer_is_in_arena(pointer)) {
        return heap_caps_realloc(pointer, size, PEER_FALLBACK_CAPS);
    }
    multi_heap_handle_t heap = begin_arena_operation(true);
    if (heap == NULL) {
        atomic_fetch_add_explicit(&s_allocation_failures, 1U,
                                  memory_order_relaxed);
        return NULL;
    }
    void *memory = multi_heap_realloc(heap, pointer, size);
    end_arena_operation();
    if (memory == NULL) {
        atomic_fetch_add_explicit(&s_allocation_failures, 1U,
                                  memory_order_relaxed);
    }
    return memory;
}

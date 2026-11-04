/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_memory_policy.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "multi_heap.h"

typedef struct {
    bool active;
    bool tls;
    uint32_t generation;
    const char *owner;
    jetkvm_memory_policy_request_t request;
    /* TLS only: the owner's record, the free bytes at the admission, the
     * least free bytes that watch_held_tls() has seen since, and everything
     * that was reserved during the hold: what was held at the admission,
     * this reservation, and each one admitted since. */
    jetkvm_memory_policy_tls_admission_t *record;
    uint32_t admitted_internal_free;
    uint32_t admitted_dma_free;
    uint32_t internal_low;
    uint32_t dma_low;
    uint32_t internal_reserved_during;
    uint32_t dma_reserved_during;
    bool overrun;
    bool under_floor;
} reservation_slot_t;

typedef struct {
    bool active;
    uint32_t generation;
    const char *owner;
    size_t working_set_bytes;
} video_rebuild_state_t;

static const char *TAG = "memory_policy";
static atomic_bool s_initialized;
static atomic_uint s_control_allocation_failures;
static atomic_uint s_lvgl_allocation_failures;
static atomic_uint s_json_allocation_failures;
static atomic_uint s_admission_denials;
static atomic_uint s_video_rebuild_begin_denials;
static atomic_uint s_video_rebuild_failures;
static atomic_uint s_video_rebuild_blocked_materializations;
static uint8_t *s_policy_arenas;
static multi_heap_handle_t s_control_heap;
static multi_heap_handle_t s_lvgl_heap;
static size_t s_lvgl_initial_free;
static portMUX_TYPE s_control_heap_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_lvgl_heap_lock = portMUX_INITIALIZER_UNLOCKED;
/* Only the expensive heap-walk details are reused by live telemetry. */
static portMUX_TYPE s_snapshot_details_lock = portMUX_INITIALIZER_UNLOCKED;
static jetkvm_memory_policy_snapshot_t s_snapshot_details;
static StaticSemaphore_t s_reservation_mutex_storage;
static SemaphoreHandle_t s_reservation_mutex;
static reservation_slot_t
    s_reservations[JETKVM_MEMORY_RESERVATION_SLOTS];
static uint32_t s_next_reservation_generation = 1U;
static video_rebuild_state_t s_video_rebuild;
static uint32_t s_next_video_rebuild_generation = 1U;
/* Telemetry only; kept in PSRAM so it costs no internal RAM. */
static jetkvm_memory_policy_tls_admission_t *s_tls_admissions;
/* While a TLS reservation is held, this timer reads the free internal and
 * DMA-capable RAM every JETKVM_MEMORY_TLS_WATCH_MS (watch_sample()). */
static esp_timer_handle_t s_watch_timer;
static bool s_watch_running;

static uint32_t clamp_u32(size_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

void *jetkvm_memory_policy_control_alloc(size_t size)
{
    if (size == 0U || s_control_heap == NULL) {
        return NULL;
    }
    void *memory = multi_heap_malloc(s_control_heap, size);
    if (memory == NULL) {
        atomic_fetch_add_explicit(&s_control_allocation_failures, 1U,
                                  memory_order_relaxed);
    }
    return memory;
}

void *jetkvm_memory_policy_control_realloc(void *memory, size_t size)
{
    if (memory == NULL) {
        return jetkvm_memory_policy_control_alloc(size);
    }
    if (size == 0U) {
        jetkvm_memory_policy_control_free(memory);
        return NULL;
    }
    if (s_control_heap == NULL) {
        return NULL;
    }
    void *resized = multi_heap_realloc(s_control_heap, memory, size);
    if (resized == NULL) {
        atomic_fetch_add_explicit(&s_control_allocation_failures, 1U,
                                  memory_order_relaxed);
    }
    return resized;
}

void jetkvm_memory_policy_control_free(void *memory)
{
    if (memory != NULL && s_control_heap != NULL) {
        multi_heap_free(s_control_heap, memory);
    }
}

void *jetkvm_memory_policy_lvgl_alloc(size_t size)
{
    if (size == 0U || s_lvgl_heap == NULL) {
        return NULL;
    }
    void *memory = multi_heap_malloc(s_lvgl_heap, size);
    if (memory == NULL) {
        atomic_fetch_add_explicit(&s_lvgl_allocation_failures, 1U,
                                  memory_order_relaxed);
    }
    return memory;
}

void *jetkvm_memory_policy_lvgl_realloc(void *memory, size_t size)
{
    if (memory == NULL) {
        return jetkvm_memory_policy_lvgl_alloc(size);
    }
    if (size == 0U) {
        jetkvm_memory_policy_lvgl_free(memory);
        return NULL;
    }
    if (s_lvgl_heap == NULL) {
        return NULL;
    }
    void *resized = multi_heap_realloc(s_lvgl_heap, memory, size);
    if (resized == NULL) {
        atomic_fetch_add_explicit(&s_lvgl_allocation_failures, 1U,
                                  memory_order_relaxed);
    }
    return resized;
}

void jetkvm_memory_policy_lvgl_free(void *memory)
{
    if (memory != NULL && s_lvgl_heap != NULL) {
        multi_heap_free(s_lvgl_heap, memory);
    }
}

bool jetkvm_memory_policy_lvgl_check(void)
{
    return s_lvgl_heap != NULL && multi_heap_check(s_lvgl_heap, false);
}

static void *json_control_alloc(size_t size)
{
    void *memory = jetkvm_memory_policy_control_alloc(size);
    if (memory == NULL) {
        atomic_fetch_add_explicit(&s_json_allocation_failures, 1U,
                                  memory_order_relaxed);
    }
    return memory;
}

static void json_control_free(void *memory)
{
    jetkvm_memory_policy_control_free(memory);
}

static bool has_projected_floor(size_t free_bytes, size_t reserved_bytes,
                                size_t requested_bytes, size_t floor_bytes)
{
    return reserved_bytes <= free_bytes &&
           requested_bytes <= free_bytes - reserved_bytes &&
           free_bytes - reserved_bytes - requested_bytes >= floor_bytes;
}

/* The has_projected_floor() margin as a signed value. */
static int32_t projected_headroom(size_t free_bytes, size_t reserved_bytes,
                                  size_t requested_bytes, size_t floor_bytes)
{
    const int64_t headroom = (int64_t)free_bytes - (int64_t)reserved_bytes -
                             (int64_t)requested_bytes - (int64_t)floor_bytes;
    return headroom < INT32_MIN ? INT32_MIN :
           headroom > INT32_MAX ? INT32_MAX : (int32_t)headroom;
}

/* Caller holds s_reservation_mutex. Owners are string literals; the table
 * keeps the first JETKVM_MEMORY_TLS_ADMISSION_OWNERS names it sees. Returns
 * the owner's record, or NULL for an owner that the table has no room for. */
static jetkvm_memory_policy_tls_admission_t *record_tls_admission(
    const char *owner, bool accepted, int32_t internal_headroom,
    int32_t dma_headroom, uint32_t internal_largest, uint32_t dma_largest)
{
    for (size_t i = 0U; s_tls_admissions != NULL &&
                        i < JETKVM_MEMORY_TLS_ADMISSION_OWNERS; ++i) {
        jetkvm_memory_policy_tls_admission_t *record = &s_tls_admissions[i];
        if (record->owner == NULL) {
            *record = (jetkvm_memory_policy_tls_admission_t) {
                .owner = owner ? owner : "unknown",
                .min_internal_headroom = INT32_MAX,
                .min_dma_headroom = INT32_MAX,
                .min_internal_largest = UINT32_MAX,
                .min_dma_largest = UINT32_MAX,
                .held_internal_min = UINT32_MAX,
                .held_dma_min = UINT32_MAX,
            };
        } else if (strcmp(record->owner, owner ? owner : "unknown") != 0) {
            continue;
        }
        if (accepted) {
            record->admitted++;
        } else {
            record->denied++;
        }
        record->last_internal_headroom = internal_headroom;
        record->last_dma_headroom = dma_headroom;
        if (internal_headroom < record->min_internal_headroom) {
            record->min_internal_headroom = internal_headroom;
        }
        if (dma_headroom < record->min_dma_headroom) {
            record->min_dma_headroom = dma_headroom;
        }
        if (internal_largest < record->min_internal_largest) {
            record->min_internal_largest = internal_largest;
        }
        if (dma_largest < record->min_dma_largest) {
            record->min_dma_largest = dma_largest;
        }
        return record;
    }
    return NULL;
}

/* Caller holds s_reservation_mutex and has just read the free bytes under
 * it, each a sum over the heap's regions at one moment. Notes them for
 * every TLS reservation that is held now. */
static void watch_held_tls(uint32_t internal_free, uint32_t dma_free)
{
    for (size_t i = 0U; i < JETKVM_MEMORY_RESERVATION_SLOTS; ++i) {
        reservation_slot_t *slot = &s_reservations[i];
        if (!slot->active || !slot->tls) {
            continue;
        }
        if (internal_free < slot->internal_low) {
            slot->internal_low = internal_free;
        }
        if (dma_free < slot->dma_low) {
            slot->dma_low = dma_free;
        }
        jetkvm_memory_policy_tls_admission_t *record = slot->record;
        if (record == NULL) {
            continue;
        }
        if (slot->internal_low < record->held_internal_min) {
            record->held_internal_min = slot->internal_low;
        }
        if (slot->dma_low < record->held_dma_min) {
            record->held_dma_min = slot->dma_low;
        }
        const uint32_t internal_fall =
            slot->admitted_internal_free - slot->internal_low;
        const uint32_t dma_fall = slot->admitted_dma_free - slot->dma_low;
        if (internal_fall > record->held_internal_fall_max) {
            record->held_internal_fall_max = internal_fall;
        }
        if (dma_fall > record->held_dma_fall_max) {
            record->held_dma_fall_max = dma_fall;
        }
        if (!slot->overrun &&
            (internal_fall > slot->internal_reserved_during +
                                 (JETKVM_MEMORY_INTERNAL_ADMISSION_FLOOR_BYTES -
                                  JETKVM_MEMORY_INTERNAL_HARD_FLOOR_BYTES) ||
             dma_fall > slot->dma_reserved_during +
                            (JETKVM_MEMORY_DMA_ADMISSION_FLOOR_BYTES -
                             JETKVM_MEMORY_DMA_HARD_FLOOR_BYTES))) {
            slot->overrun = true;
            record->held_overruns++;
        }
        if (!slot->under_floor &&
            (slot->internal_low <
                 JETKVM_MEMORY_INTERNAL_ADMISSION_FLOOR_BYTES ||
             slot->dma_low < JETKVM_MEMORY_DMA_ADMISSION_FLOOR_BYTES)) {
            slot->under_floor = true;
            record->held_under_floor++;
        }
    }
}

/* The timer's callback: one reading for the reservations that are held at
 * this moment. It reads under the mutex, so a reading is never noted for a
 * reservation that was admitted after it or released before it. It does not
 * wait: whoever holds the mutex reads the free bytes itself. A callback
 * that runs after the last release finds no reservation. */
static void watch_sample(void *unused)
{
    (void)unused;
    if (xSemaphoreTake(s_reservation_mutex, 0) != pdTRUE) {
        return;
    }
    watch_held_tls(
        (uint32_t)heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (uint32_t)heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    xSemaphoreGive(s_reservation_mutex);
}

/* Caller holds s_reservation_mutex. The timer runs while a TLS reservation
 * is held. Without a timer the holds are seen at the policy's own looks. */
static void watch_run_locked(bool run)
{
    if (s_watch_timer == NULL || run == s_watch_running) {
        return;
    }
    if (run) {
        s_watch_running =
            esp_timer_start_periodic(
                s_watch_timer,
                (uint64_t)JETKVM_MEMORY_TLS_WATCH_MS * 1000U) == ESP_OK;
    } else {
        (void)esp_timer_stop(s_watch_timer);
        s_watch_running = false;
    }
}

static bool request_shape_valid(
    const jetkvm_memory_policy_request_t *request)
{
    return request != NULL && request->dma_bytes <= request->internal_bytes &&
           request->internal_largest_block <= request->internal_bytes &&
           request->dma_largest_block <= request->dma_bytes &&
           request->psram_largest_block <= request->psram_bytes;
}

esp_err_t jetkvm_memory_policy_init(void)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(
            &s_initialized, &expected, true, memory_order_acq_rel,
            memory_order_acquire)) {
        return ESP_OK;
    }
    const size_t psram_total = heap_caps_get_total_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (psram_total < JETKVM_MEMORY_POLICY_ARENAS_TOTAL_BYTES +
                          JETKVM_MEMORY_PSRAM_ADMISSION_FLOOR_BYTES) {
        atomic_store_explicit(&s_initialized, false, memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    s_reservation_mutex = xSemaphoreCreateMutexStatic(
        &s_reservation_mutex_storage);
    if (s_reservation_mutex == NULL) {
        atomic_store_explicit(&s_initialized, false, memory_order_release);
        return ESP_ERR_NO_MEM;
    }

    s_policy_arenas = heap_caps_aligned_alloc(
        64U, JETKVM_MEMORY_POLICY_ARENAS_TOTAL_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_policy_arenas == NULL) {
        atomic_store_explicit(&s_initialized, false, memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    s_control_heap = multi_heap_register(
        s_policy_arenas, JETKVM_MEMORY_CONTROL_ARENA_BYTES);
    s_lvgl_heap = multi_heap_register(
        s_policy_arenas + JETKVM_MEMORY_CONTROL_ARENA_BYTES,
        JETKVM_MEMORY_LVGL_ARENA_BYTES);
    if (s_control_heap == NULL || s_lvgl_heap == NULL) {
        heap_caps_free(s_policy_arenas);
        s_policy_arenas = NULL;
        s_control_heap = NULL;
        s_lvgl_heap = NULL;
        atomic_store_explicit(&s_initialized, false, memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    multi_heap_set_lock(s_control_heap, &s_control_heap_lock);
    multi_heap_set_lock(s_lvgl_heap, &s_lvgl_heap_lock);
    /* Without it only the TLS admission telemetry is missing. */
    s_tls_admissions = heap_caps_calloc(
        JETKVM_MEMORY_TLS_ADMISSION_OWNERS, sizeof(*s_tls_admissions),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* Without it a hold is seen at the policy's own looks only. */
    const esp_timer_create_args_t watch_timer = {
        .callback = watch_sample,
        .name = "memory_watch",
    };
    if (esp_timer_create(&watch_timer, &s_watch_timer) != ESP_OK) {
        s_watch_timer = NULL;
    }
    multi_heap_info_t lvgl_initial = {0};
    multi_heap_get_info(s_lvgl_heap, &lvgl_initial);
    s_lvgl_initial_free = lvgl_initial.total_free_bytes;

    cJSON_Hooks hooks = {
        .malloc_fn = json_control_alloc,
        .free_fn = json_control_free,
    };
    cJSON_InitHooks(&hooks);
    ESP_LOGI(TAG,
             "policy ready: internal hard/admit=%u/%u DMA=%u/%u "
             "PSRAM=%u/%u policy-arenas=%u "
             "control/LVGL=%u/%u storage-ingress-total=%u",
             JETKVM_MEMORY_INTERNAL_HARD_FLOOR_BYTES,
             JETKVM_MEMORY_INTERNAL_ADMISSION_FLOOR_BYTES,
             JETKVM_MEMORY_DMA_HARD_FLOOR_BYTES,
             JETKVM_MEMORY_DMA_ADMISSION_FLOOR_BYTES,
             JETKVM_MEMORY_PSRAM_HARD_FLOOR_BYTES,
             JETKVM_MEMORY_PSRAM_ADMISSION_FLOOR_BYTES,
             JETKVM_MEMORY_POLICY_ARENAS_TOTAL_BYTES,
             JETKVM_MEMORY_CONTROL_ARENA_BYTES,
             JETKVM_MEMORY_LVGL_ARENA_BYTES,
             JETKVM_MEMORY_STORAGE_INGRESS_TOTAL_MAX_BYTES);
    return ESP_OK;
}

static void memory_policy_snapshot(
    jetkvm_memory_policy_snapshot_t *snapshot, bool detailed)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (!detailed) {
        portENTER_CRITICAL(&s_snapshot_details_lock);
        snapshot->internal_largest = s_snapshot_details.internal_largest;
        snapshot->dma_largest = s_snapshot_details.dma_largest;
        snapshot->psram_largest = s_snapshot_details.psram_largest;
        snapshot->control_arena_largest = s_snapshot_details.control_arena_largest;
        snapshot->lvgl_arena_largest = s_snapshot_details.lvgl_arena_largest;
        snapshot->lvgl_allocated_blocks = s_snapshot_details.lvgl_allocated_blocks;
        snapshot->lvgl_free_blocks = s_snapshot_details.lvgl_free_blocks;
        snapshot->detailed_sample_timestamp_us = s_snapshot_details.detailed_sample_timestamp_us;
        portEXIT_CRITICAL(&s_snapshot_details_lock);
    }
    snapshot->internal_free = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snapshot->internal_minimum = (uint32_t)heap_caps_get_minimum_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (detailed) {
        snapshot->internal_largest = (uint32_t)heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    snapshot->dma_free = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    snapshot->dma_minimum = (uint32_t)heap_caps_get_minimum_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (detailed) {
        snapshot->dma_largest = (uint32_t)heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    }
    snapshot->psram_free = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    snapshot->psram_minimum = (uint32_t)heap_caps_get_minimum_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (detailed) {
        snapshot->psram_largest = (uint32_t)heap_caps_get_largest_free_block(
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_control_heap != NULL) {
        multi_heap_info_t control = {0};
        if (detailed) {
            multi_heap_get_info(s_control_heap, &control);
        } else {
            control.total_free_bytes = multi_heap_free_size(s_control_heap);
            control.minimum_free_bytes = multi_heap_minimum_free_size(s_control_heap);
            control.largest_free_block = snapshot->control_arena_largest;
        }
        snapshot->control_arena_free = clamp_u32(control.total_free_bytes);
        snapshot->control_arena_minimum =
            clamp_u32(control.minimum_free_bytes);
        snapshot->control_arena_largest =
            clamp_u32(control.largest_free_block);
    }
    if (s_lvgl_heap != NULL) {
        multi_heap_info_t lvgl = {0};
        if (detailed) {
            multi_heap_get_info(s_lvgl_heap, &lvgl);
        } else {
            lvgl.total_free_bytes = multi_heap_free_size(s_lvgl_heap);
            lvgl.minimum_free_bytes = multi_heap_minimum_free_size(s_lvgl_heap);
            lvgl.largest_free_block = snapshot->lvgl_arena_largest;
            lvgl.allocated_blocks = snapshot->lvgl_allocated_blocks;
            lvgl.free_blocks = snapshot->lvgl_free_blocks;
        }
        const size_t used = s_lvgl_initial_free >= lvgl.total_free_bytes ?
            s_lvgl_initial_free - lvgl.total_free_bytes : 0U;
        const size_t peak = s_lvgl_initial_free >= lvgl.minimum_free_bytes ?
            s_lvgl_initial_free - lvgl.minimum_free_bytes : 0U;
        snapshot->lvgl_arena_capacity = clamp_u32(s_lvgl_initial_free);
        snapshot->lvgl_arena_free = clamp_u32(lvgl.total_free_bytes);
        snapshot->lvgl_arena_minimum =
            clamp_u32(lvgl.minimum_free_bytes);
        snapshot->lvgl_arena_largest =
            clamp_u32(lvgl.largest_free_block);
        snapshot->lvgl_arena_used = clamp_u32(used);
        snapshot->lvgl_arena_peak_used = clamp_u32(peak);
        snapshot->lvgl_allocated_blocks =
            clamp_u32(lvgl.allocated_blocks);
        snapshot->lvgl_free_blocks = clamp_u32(lvgl.free_blocks);
    }
    if (detailed) {
        snapshot->detailed_sample_timestamp_us = esp_timer_get_time();
        portENTER_CRITICAL(&s_snapshot_details_lock);
        s_snapshot_details = *snapshot;
        portEXIT_CRITICAL(&s_snapshot_details_lock);
    }
    snapshot->control_allocation_failures = atomic_load_explicit(
        &s_control_allocation_failures, memory_order_relaxed);
    snapshot->lvgl_allocation_failures = atomic_load_explicit(
        &s_lvgl_allocation_failures, memory_order_relaxed);
    snapshot->json_allocation_failures = atomic_load_explicit(
        &s_json_allocation_failures, memory_order_relaxed);
    snapshot->admission_denials = atomic_load_explicit(
        &s_admission_denials, memory_order_relaxed);
    snapshot->video_rebuild_begin_denials = atomic_load_explicit(
        &s_video_rebuild_begin_denials, memory_order_relaxed);
    snapshot->video_rebuild_failures = atomic_load_explicit(
        &s_video_rebuild_failures, memory_order_relaxed);
    snapshot->video_rebuild_blocked_materializations =
        atomic_load_explicit(&s_video_rebuild_blocked_materializations,
                             memory_order_relaxed);

    if (s_reservation_mutex != NULL &&
        xSemaphoreTake(s_reservation_mutex, portMAX_DELAY) == pdTRUE) {
        /* Read again under the mutex: a reservation that was admitted
         * after the values above were read has not seen them. */
        watch_held_tls(
            (uint32_t)heap_caps_get_free_size(
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (uint32_t)heap_caps_get_free_size(
                MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        snapshot->video_rebuild_active = s_video_rebuild.active;
        snapshot->video_rebuild_working_set_bytes =
            clamp_u32(s_video_rebuild.working_set_bytes);
        for (size_t i = 0U; i < JETKVM_MEMORY_RESERVATION_SLOTS; ++i) {
            if (!s_reservations[i].active) {
                continue;
            }
            snapshot->reservation_internal_bytes +=
                clamp_u32(s_reservations[i].request.internal_bytes);
            snapshot->reservation_dma_bytes +=
                clamp_u32(s_reservations[i].request.dma_bytes);
            snapshot->reservation_psram_bytes +=
                clamp_u32(s_reservations[i].request.psram_bytes);
            snapshot->reservation_storage_ingress_bytes +=
                clamp_u32(
                    s_reservations[i].request.storage_ingress_bytes);
            snapshot->active_reservations++;
            if (s_reservations[i].request.storage_ingress_bytes != 0U) {
                snapshot->active_storage_ingress_reservations++;
            }
            if (s_reservations[i].tls) {
                snapshot->tls_reservation_internal_bytes +=
                    clamp_u32(s_reservations[i].request.internal_bytes);
                snapshot->tls_reservation_dma_bytes +=
                    clamp_u32(s_reservations[i].request.dma_bytes);
                snapshot->active_tls_reservations++;
            }
        }
        xSemaphoreGive(s_reservation_mutex);
    }
}

void jetkvm_memory_policy_get_snapshot(
    jetkvm_memory_policy_snapshot_t *snapshot)
{
    memory_policy_snapshot(snapshot, true);
}

void jetkvm_memory_policy_get_snapshot_cached(
    jetkvm_memory_policy_snapshot_t *snapshot)
{
    memory_policy_snapshot(snapshot, false);
}

size_t jetkvm_memory_policy_get_tls_admissions(
    jetkvm_memory_policy_tls_admission_t *admissions, size_t capacity)
{
    size_t count = 0U;
    if (admissions == NULL || s_reservation_mutex == NULL ||
        xSemaphoreTake(s_reservation_mutex, portMAX_DELAY) != pdTRUE) {
        return 0U;
    }
    while (s_tls_admissions != NULL && count < capacity &&
           count < JETKVM_MEMORY_TLS_ADMISSION_OWNERS &&
           s_tls_admissions[count].owner != NULL) {
        admissions[count] = s_tls_admissions[count];
        for (size_t i = 0U; i < JETKVM_MEMORY_RESERVATION_SLOTS; ++i) {
            if (s_reservations[i].active &&
                s_reservations[i].record == &s_tls_admissions[count]) {
                admissions[count].reserved_internal +=
                    clamp_u32(s_reservations[i].request.internal_bytes);
                admissions[count].reserved_dma +=
                    clamp_u32(s_reservations[i].request.dma_bytes);
            }
        }
        count++;
    }
    xSemaphoreGive(s_reservation_mutex);
    return count;
}


/* The PSRAM floor after pending tickets, and the largest block for both. */
static bool psram_fits(
    const jetkvm_memory_policy_snapshot_t *memory,
    const jetkvm_memory_policy_request_t *request, size_t reserved_psram,
    size_t reserved_psram_largest)
{
    const size_t largest =
        request->psram_largest_block > reserved_psram_largest ?
            request->psram_largest_block : reserved_psram_largest;
    return has_projected_floor(memory->psram_free, reserved_psram,
                               request->psram_bytes,
                               JETKVM_MEMORY_PSRAM_ADMISSION_FLOOR_BYTES) &&
           largest <= memory->psram_largest;
}

/* The video buffers are allocated once at boot (jetkvm_video_reserve_buffers())
 * and a mode change does not request them again, so a request is judged by
 * the floors and by its own largest allocation only. */
static bool request_fits(
    const jetkvm_memory_policy_snapshot_t *memory,
    const jetkvm_memory_policy_request_t *request,
    size_t reserved_internal, size_t reserved_dma, size_t reserved_psram,
    size_t reserved_storage_ingress, size_t reserved_internal_largest,
    size_t reserved_dma_largest, size_t reserved_psram_largest)
{
    const bool ingress_fits =
        reserved_storage_ingress <=
            JETKVM_MEMORY_STORAGE_INGRESS_TOTAL_MAX_BYTES &&
        request->storage_ingress_bytes <=
            JETKVM_MEMORY_STORAGE_INGRESS_TOTAL_MAX_BYTES -
                reserved_storage_ingress;
    return ingress_fits && has_projected_floor(
               memory->internal_free, reserved_internal,
               request->internal_bytes,
               JETKVM_MEMORY_INTERNAL_ADMISSION_FLOOR_BYTES) &&
           has_projected_floor(
               memory->dma_free, reserved_dma, request->dma_bytes,
               JETKVM_MEMORY_DMA_ADMISSION_FLOOR_BYTES) &&
           psram_fits(memory, request, reserved_psram, reserved_psram_largest) &&
           request->internal_largest_block <= memory->internal_largest &&
           reserved_internal_largest <= memory->internal_largest &&
           request->dma_largest_block <= memory->dma_largest &&
           reserved_dma_largest <= memory->dma_largest;
}

/* Whether an active video rebuild keeps the request out. A rebuild keeps
 * out every request with PSRAM while it allocates its PSRAM working set.
 * With the video pool its working set is 0, except in the debug captures
 * (the BGR888 diagnostic's output, the CSI driver's backup buffer), and a
 * request that takes PSRAM only cannot take anything that such a rebuild
 * allocates, so it is judged against memory as at any other time. */
static bool video_rebuild_blocks(
    const jetkvm_memory_policy_request_t *request, bool psram_only)
{
    return s_video_rebuild.active && request->psram_bytes != 0U &&
           (!psram_only || s_video_rebuild.working_set_bytes != 0U);
}

/* With wait_for_tickets, a request that only other owners' tickets keep
 * out is deferred as one a video rebuild blocks is: *deferred, and no
 * denial recorded. */
static bool reserve_with_domain(
    const char *owner, const jetkvm_memory_policy_request_t *request,
    bool tls, bool psram_only, jetkvm_memory_policy_reservation_t *reservation,
    bool *deferred, bool wait_for_tickets)
{
    if (deferred != NULL) {
        *deferred = false;
    }
    if (reservation == NULL || reservation->active ||
        !request_shape_valid(request)) {
        return false;
    }
    if (s_reservation_mutex == NULL ||
        xSemaphoreTake(s_reservation_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    /* A peer can retry once the video transaction commits. Check under the
     * same arbiter as rebuild_begin, before any heap walk or reservation. */
    if (deferred != NULL && video_rebuild_blocks(request, psram_only)) {
        *deferred = true;
        xSemaphoreGive(s_reservation_mutex);
        return false;
    }
    /* Snapshot after acquiring the arbiter. If an earlier owner has already
     * materialized and released its ticket, its allocation is now visible;
     * if it has not, its live ticket is included below. This closes the
     * snapshot -> ticket TOCTOU window without holding a spinlock while the
     * heap walks its free lists. */
    jetkvm_memory_policy_snapshot_t memory = {
        .internal_free = (uint32_t)heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .internal_largest = (uint32_t)heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        .dma_free = (uint32_t)heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
        .dma_largest = (uint32_t)heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
        .psram_free = (uint32_t)heap_caps_get_free_size(
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .psram_largest = (uint32_t)heap_caps_get_largest_free_block(
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    };
    watch_held_tls(memory.internal_free, memory.dma_free);

    size_t reserved_internal = 0U;
    size_t reserved_dma = 0U;
    size_t reserved_psram = 0U;
    size_t reserved_storage_ingress = 0U;
    size_t reserved_internal_largest = 0U;
    size_t reserved_dma_largest = 0U;
    size_t reserved_psram_largest = 0U;
    size_t free_slot = JETKVM_MEMORY_RESERVATION_SLOTS;
    bool accepted = false;
    const bool blocked_by_video_rebuild =
        video_rebuild_blocks(request, psram_only);

    for (size_t i = 0U; i < JETKVM_MEMORY_RESERVATION_SLOTS; ++i) {
        const reservation_slot_t *slot = &s_reservations[i];
        if (!slot->active) {
            if (free_slot == JETKVM_MEMORY_RESERVATION_SLOTS) {
                free_slot = i;
            }
            continue;
        }
        reserved_internal += slot->request.internal_bytes;
        reserved_dma += slot->request.dma_bytes;
        reserved_psram += slot->request.psram_bytes;
        reserved_storage_ingress +=
            slot->request.storage_ingress_bytes;
        if (slot->request.internal_largest_block >
            reserved_internal_largest) {
            reserved_internal_largest =
                slot->request.internal_largest_block;
        }
        if (slot->request.dma_largest_block > reserved_dma_largest) {
            reserved_dma_largest = slot->request.dma_largest_block;
        }
        if (slot->request.psram_largest_block > reserved_psram_largest) {
            reserved_psram_largest = slot->request.psram_largest_block;
        }
    }
    const bool fits = psram_only ?
        psram_fits(&memory, request, reserved_psram, reserved_psram_largest) :
        request_fits(&memory, request, reserved_internal, reserved_dma,
                     reserved_psram, reserved_storage_ingress,
                     reserved_internal_largest,
                     reserved_dma_largest, reserved_psram_largest);
    accepted = !blocked_by_video_rebuild &&
        free_slot < JETKVM_MEMORY_RESERVATION_SLOTS && fits;
    if (!accepted && wait_for_tickets && !blocked_by_video_rebuild &&
        free_slot < JETKVM_MEMORY_RESERVATION_SLOTS &&
        request_fits(&memory, request, 0U, 0U, 0U, 0U, 0U, 0U, 0U)) {
        *deferred = true;
        xSemaphoreGive(s_reservation_mutex);
        return false;
    }
    jetkvm_memory_policy_tls_admission_t *record = !tls ? NULL :
        record_tls_admission(
            owner, accepted,
            projected_headroom(memory.internal_free, reserved_internal,
                               request->internal_bytes,
                               JETKVM_MEMORY_INTERNAL_ADMISSION_FLOOR_BYTES),
            projected_headroom(memory.dma_free, reserved_dma,
                               request->dma_bytes,
                               JETKVM_MEMORY_DMA_ADMISSION_FLOOR_BYTES),
            memory.internal_largest, memory.dma_largest);
    if (accepted) {
        /* What this owner may allocate counts for every hold that it is
         * admitted in. */
        for (size_t i = 0U; i < JETKVM_MEMORY_RESERVATION_SLOTS; ++i) {
            if (s_reservations[i].active && s_reservations[i].tls) {
                s_reservations[i].internal_reserved_during +=
                    (uint32_t)request->internal_bytes;
                s_reservations[i].dma_reserved_during +=
                    (uint32_t)request->dma_bytes;
            }
        }
        reservation_slot_t *slot = &s_reservations[free_slot];
        uint32_t generation = s_next_reservation_generation++;
        if (generation == 0U) {
            generation = s_next_reservation_generation++;
        }
        *slot = (reservation_slot_t) {
            .active = true,
            .tls = tls,
            .generation = generation,
            .owner = owner,
            .request = *request,
            .record = record,
            .admitted_internal_free = memory.internal_free,
            .admitted_dma_free = memory.dma_free,
            .internal_low = memory.internal_free,
            .dma_low = memory.dma_free,
            .internal_reserved_during =
                (uint32_t)(reserved_internal + request->internal_bytes),
            .dma_reserved_during =
                (uint32_t)(reserved_dma + request->dma_bytes),
        };
        *reservation = (jetkvm_memory_policy_reservation_t) {
            .generation = generation,
            .slot = (uint8_t)free_slot,
            .active = true,
        };
        if (tls) {
            /* The admission's free bytes are the hold's first value. */
            watch_held_tls(memory.internal_free, memory.dma_free);
            watch_run_locked(true);
        }
    }
    xSemaphoreGive(s_reservation_mutex);

    if (!accepted) {
        atomic_fetch_add_explicit(&s_admission_denials, 1U,
                                  memory_order_relaxed);
        if (blocked_by_video_rebuild) {
            atomic_fetch_add_explicit(
                &s_video_rebuild_blocked_materializations, 1U,
                memory_order_relaxed);
        }
        ESP_LOGW(TAG,
                 "reservation denied owner=%s req int=%u dma=%u psram=%u "
                 "ingress=%u "
                 "free int=%" PRIu32 " dma=%" PRIu32 " psram=%" PRIu32
                 " largest=%" PRIu32 " pending=%u/%u/%u ingress=%u "
                 "video-rebuild=%d",
                 owner ? owner : "unknown",
                 (unsigned)request->internal_bytes,
                 (unsigned)request->dma_bytes,
                 (unsigned)request->psram_bytes,
                 (unsigned)request->storage_ingress_bytes,
                 memory.internal_free, memory.dma_free, memory.psram_free,
                 memory.psram_largest, (unsigned)reserved_internal,
                 (unsigned)reserved_dma, (unsigned)reserved_psram,
                 (unsigned)reserved_storage_ingress,
                 blocked_by_video_rebuild);
    }
    return accepted;
}

bool jetkvm_memory_policy_reserve(
    const char *owner, const jetkvm_memory_policy_request_t *request,
    jetkvm_memory_policy_reservation_t *reservation)
{
    return reserve_with_domain(owner, request, false, false, reservation,
                               NULL, false);
}

bool jetkvm_memory_policy_reserve_psram(
    const char *owner, size_t bytes,
    jetkvm_memory_policy_reservation_t *reservation)
{
    const jetkvm_memory_policy_request_t request = {
        .psram_bytes = bytes,
        .psram_largest_block = bytes,
    };
    return reserve_with_domain(owner, &request, false, true, reservation,
                               NULL, false);
}

bool jetkvm_memory_policy_wait_reserve_psram(
    const char *owner, size_t bytes,
    jetkvm_memory_policy_reservation_t *reservation)
{
    const jetkvm_memory_policy_request_t request = {
        .psram_bytes = bytes,
        .psram_largest_block = bytes,
    };
    for (uint32_t waited_ms = 0U; waited_ms < JETKVM_MEMORY_PSRAM_WAIT_MS;
         waited_ms += JETKVM_MEMORY_PSRAM_RETRY_MS) {
        /* Deferred only while a video rebuild is active. */
        bool deferred = false;
        const bool accepted = reserve_with_domain(
            owner, &request, false, true, reservation, &deferred, false);
        if (accepted || !deferred) {
            return accepted;
        }
        vTaskDelay(pdMS_TO_TICKS(JETKVM_MEMORY_PSRAM_RETRY_MS));
    }
    return jetkvm_memory_policy_reserve_psram(owner, bytes, reservation);
}

esp_err_t jetkvm_memory_policy_try_reserve_peer(
    const char *owner, const jetkvm_memory_policy_request_t *request,
    jetkvm_memory_policy_reservation_t *reservation)
{
    bool deferred = false;
    bool accepted = reserve_with_domain(owner, request, false, false,
                                       reservation, &deferred, false);
    return accepted ? ESP_OK : deferred ? ESP_ERR_NOT_FINISHED : ESP_ERR_NO_MEM;
}

/* A TLS reservation has the same bytes in internal RAM and in its
 * DMA-capable part: bytes, which must be free as one block, and
 * link_bytes for the Wi-Fi link's blocks of 1,604 B each. */
static bool reserve_tls_with_domain(
    const char *owner, size_t bytes, size_t link_bytes,
    jetkvm_memory_policy_reservation_t *reservation, bool *deferred)
{
    const jetkvm_memory_policy_request_t request = {
        .internal_bytes = bytes + link_bytes,
        .internal_largest_block = bytes,
        .dma_bytes = bytes + link_bytes,
        .dma_largest_block = bytes,
    };
    return reserve_with_domain(owner, &request, true, false, reservation,
                               deferred, deferred != NULL);
}

/* Tries every JETKVM_MEMORY_TLS_RETRY_MS for up to JETKVM_MEMORY_TLS_WAIT_MS
 * while only other owners' reservations keep the request out, then once
 * more without a wait, which records the outcome. */
static bool wait_reserve_tls_with_domain(
    const char *owner, size_t bytes, size_t link_bytes,
    jetkvm_memory_policy_reservation_t *reservation)
{
    for (uint32_t waited_ms = 0U; waited_ms < JETKVM_MEMORY_TLS_WAIT_MS;
         waited_ms += JETKVM_MEMORY_TLS_RETRY_MS) {
        bool deferred = false;
        if (reserve_tls_with_domain(owner, bytes, link_bytes, reservation,
                                    &deferred) ||
            !deferred) {
            return reservation->active;
        }
        vTaskDelay(pdMS_TO_TICKS(JETKVM_MEMORY_TLS_RETRY_MS));
    }
    return reserve_tls_with_domain(owner, bytes, link_bytes, reservation,
                                   NULL);
}

bool jetkvm_memory_policy_reserve_tls(
    const char *owner, jetkvm_memory_policy_reservation_t *reservation)
{
    return reserve_tls_with_domain(
        owner, JETKVM_MEMORY_TLS_CONNECTION_RESERVE_BYTES,
        JETKVM_MEMORY_TLS_LINK_RESERVE_BYTES, reservation, NULL);
}

bool jetkvm_memory_policy_wait_reserve_tls(
    const char *owner, jetkvm_memory_policy_reservation_t *reservation)
{
    return wait_reserve_tls_with_domain(
        owner, JETKVM_MEMORY_TLS_CONNECTION_RESERVE_BYTES,
        JETKVM_MEMORY_TLS_LINK_RESERVE_BYTES, reservation);
}

bool jetkvm_memory_policy_wait_reserve_download(
    const char *owner, jetkvm_memory_policy_reservation_t *reservation)
{
    /* The queue's blocks include the 3 of a connection. */
    return wait_reserve_tls_with_domain(
        owner, JETKVM_MEMORY_TLS_CONNECTION_RESERVE_BYTES,
        JETKVM_MEMORY_DOWNLOAD_LINK_RESERVE_BYTES, reservation);
}

bool jetkvm_memory_policy_reserve_tls_session(
    const char *owner, bool ice_servers,
    jetkvm_memory_policy_reservation_t *reservation)
{
    return reserve_tls_with_domain(
        owner,
        ice_servers ? JETKVM_MEMORY_TLS_SESSION_ICE_RESERVE_BYTES :
                      JETKVM_MEMORY_TLS_SESSION_RESERVE_BYTES,
        JETKVM_MEMORY_TLS_LINK_RESERVE_BYTES, reservation, NULL);
}

bool jetkvm_memory_policy_wait_reserve_tls_open_connection(
    const char *owner, jetkvm_memory_policy_reservation_t *reservation)
{
    return wait_reserve_tls_with_domain(
        owner, JETKVM_MEMORY_TLS_REQUEST_RESERVE_BYTES,
        JETKVM_MEMORY_TLS_LINK_RESERVE_BYTES, reservation);
}

void jetkvm_memory_policy_release(
    jetkvm_memory_policy_reservation_t *reservation)
{
    if (reservation == NULL || !reservation->active ||
        reservation->slot >= JETKVM_MEMORY_RESERVATION_SLOTS) {
        return;
    }
    if (s_reservation_mutex == NULL ||
        xSemaphoreTake(s_reservation_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    reservation_slot_t *slot = &s_reservations[reservation->slot];
    if (slot->active && slot->generation == reservation->generation) {
        const bool tls = slot->tls;
        if (tls) {
            watch_held_tls(
                (uint32_t)heap_caps_get_free_size(
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (uint32_t)heap_caps_get_free_size(
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        }
        memset(slot, 0, sizeof(*slot));
        if (tls) {
            bool held = false;
            for (size_t i = 0U; i < JETKVM_MEMORY_RESERVATION_SLOTS; ++i) {
                held = held ||
                       (s_reservations[i].active && s_reservations[i].tls);
            }
            watch_run_locked(held);
        }
    }
    xSemaphoreGive(s_reservation_mutex);
    memset(reservation, 0, sizeof(*reservation));
}

bool jetkvm_memory_policy_shrink(
    jetkvm_memory_policy_reservation_t *reservation,
    const jetkvm_memory_policy_request_t *remaining_request)
{
    if (reservation == NULL || !reservation->active ||
        reservation->slot >= JETKVM_MEMORY_RESERVATION_SLOTS ||
        !request_shape_valid(remaining_request)) {
        return false;
    }
    bool updated = false;
    if (s_reservation_mutex == NULL ||
        xSemaphoreTake(s_reservation_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    reservation_slot_t *slot = &s_reservations[reservation->slot];
    if (slot->active && slot->generation == reservation->generation &&
        remaining_request->internal_bytes <= slot->request.internal_bytes &&
        remaining_request->internal_largest_block <=
            slot->request.internal_largest_block &&
        remaining_request->dma_bytes <= slot->request.dma_bytes &&
        remaining_request->dma_largest_block <=
            slot->request.dma_largest_block &&
        remaining_request->psram_bytes <= slot->request.psram_bytes &&
        remaining_request->psram_largest_block <=
            slot->request.psram_largest_block &&
        remaining_request->storage_ingress_bytes <=
            slot->request.storage_ingress_bytes) {
        slot->request = *remaining_request;
        updated = true;
    }
    xSemaphoreGive(s_reservation_mutex);
    return updated;
}

esp_err_t jetkvm_memory_policy_video_rebuild_begin(
    const char *owner, size_t new_working_set_bytes,
    jetkvm_memory_policy_video_rebuild_t *transaction)
{
    if (transaction == NULL || transaction->active) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_reservation_mutex == NULL ||
        xSemaphoreTake(s_reservation_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    bool pending_psram_materialization = false;
    for (size_t i = 0U; i < JETKVM_MEMORY_RESERVATION_SLOTS; ++i) {
        if (s_reservations[i].active &&
            s_reservations[i].request.psram_bytes != 0U) {
            pending_psram_materialization = true;
            break;
        }
    }

    const size_t psram_free = heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const bool capacity_fits = has_projected_floor(
        psram_free, 0U, new_working_set_bytes,
        JETKVM_MEMORY_PSRAM_ADMISSION_FLOOR_BYTES);
    /* The capture slots and the encoder DB are the video pool. A rebuild
     * allocates nothing in PSRAM, except in the debug captures: the BGR888
     * diagnostic's output and the CSI driver's backup buffer. The block
     * must hold their sum, which is more than either needs. */
    const bool largest_fits = new_working_set_bytes == 0U ||
        psram_largest >= new_working_set_bytes;
    esp_err_t result = ESP_OK;

    const bool another_video_rebuild_active = s_video_rebuild.active;
    if (another_video_rebuild_active || pending_psram_materialization) {
        result = ESP_ERR_INVALID_STATE;
    } else if (!capacity_fits || !largest_fits) {
        result = ESP_ERR_NO_MEM;
    } else {
        uint32_t generation = s_next_video_rebuild_generation++;
        if (generation == 0U) {
            generation = s_next_video_rebuild_generation++;
        }
        s_video_rebuild = (video_rebuild_state_t) {
            .active = true,
            .generation = generation,
            .owner = owner,
            .working_set_bytes = new_working_set_bytes,
        };
        *transaction = (jetkvm_memory_policy_video_rebuild_t) {
            .generation = generation,
            .active = true,
        };
    }
    xSemaphoreGive(s_reservation_mutex);

    if (result != ESP_OK) {
        atomic_fetch_add_explicit(&s_video_rebuild_begin_denials, 1U,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&s_admission_denials, 1U,
                                  memory_order_relaxed);
        ESP_LOGW(TAG,
                 "video rebuild denied owner=%s working-set=%u free=%u "
                 "largest=%u pending-psram=%d active=%d",
                 owner ? owner : "unknown",
                 (unsigned)new_working_set_bytes, (unsigned)psram_free,
                 (unsigned)psram_largest, pending_psram_materialization,
                 another_video_rebuild_active);
        return result;
    }

    ESP_LOGI(TAG,
             "video rebuild begin owner=%s working-set=%u free=%u largest=%u",
             owner ? owner : "unknown", (unsigned)new_working_set_bytes,
             (unsigned)psram_free, (unsigned)psram_largest);
    return ESP_OK;
}

esp_err_t jetkvm_memory_policy_video_rebuild_finish(
    jetkvm_memory_policy_video_rebuild_t *transaction, bool succeeded)
{
    if (transaction == NULL || !transaction->active) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_reservation_mutex == NULL ||
        xSemaphoreTake(s_reservation_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_video_rebuild.active ||
        s_video_rebuild.generation != transaction->generation) {
        xSemaphoreGive(s_reservation_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    const char *owner = s_video_rebuild.owner;
    const size_t working_set_bytes = s_video_rebuild.working_set_bytes;
    memset(&s_video_rebuild, 0, sizeof(s_video_rebuild));
    xSemaphoreGive(s_reservation_mutex);
    memset(transaction, 0, sizeof(*transaction));

    if (!succeeded) {
        atomic_fetch_add_explicit(&s_video_rebuild_failures, 1U,
                                  memory_order_relaxed);
    }
    ESP_LOGI(TAG, "video rebuild finish owner=%s working-set=%u success=%d",
             owner ? owner : "unknown", (unsigned)working_set_bytes,
             succeeded);
    return ESP_OK;
}

bool jetkvm_memory_policy_admit(
    const char *owner, const jetkvm_memory_policy_request_t *request)
{
    jetkvm_memory_policy_reservation_t reservation = {0};
    if (!jetkvm_memory_policy_reserve(owner, request, &reservation)) {
        return false;
    }
    jetkvm_memory_policy_release(&reservation);
    return true;
}

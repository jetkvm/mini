/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "lvgl.h"

#include <stdint.h>
#include <string.h>

#include "jetkvm_memory_policy.h"

#if LV_USE_STDLIB_MALLOC != LV_STDLIB_CUSTOM
#error "JetKVM LVGL memory domain requires CONFIG_LV_USE_CUSTOM_MALLOC"
#endif

/* The backing multi_heap is created by jetkvm_memory_policy_init() before the
 * screen task starts.  LVGL owns every allocation in this physical domain, so
 * deinit must not return the parent 1.5 MiB carve-out to the general heap. */
void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *memory, size_t bytes)
{
    LV_UNUSED(memory);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    return jetkvm_memory_policy_lvgl_alloc(size);
}

void *lv_realloc_core(void *memory, size_t new_size)
{
    return jetkvm_memory_policy_lvgl_realloc(memory, new_size);
}

void lv_free_core(void *memory)
{
    jetkvm_memory_policy_lvgl_free(memory);
}

void lv_mem_monitor_core(lv_mem_monitor_t *monitor)
{
    if (monitor == NULL) {
        return;
    }
    memset(monitor, 0, sizeof(*monitor));
    jetkvm_memory_policy_snapshot_t memory = {0};
    jetkvm_memory_policy_get_snapshot(&memory);

    monitor->total_size = memory.lvgl_arena_capacity;
    monitor->free_cnt = memory.lvgl_free_blocks;
    monitor->free_size = memory.lvgl_arena_free;
    monitor->free_biggest_size = memory.lvgl_arena_largest;
    monitor->used_cnt = memory.lvgl_allocated_blocks;
    monitor->max_used = memory.lvgl_arena_peak_used;
    if (monitor->total_size != 0U) {
        uint64_t used = monitor->total_size >= monitor->free_size ?
            monitor->total_size - monitor->free_size : 0U;
        uint64_t percent = used * 100U / monitor->total_size;
        monitor->used_pct = percent > 100U ? 100U : (uint8_t)percent;
    }
    if (monitor->free_size != 0U) {
        uint64_t largest_percent =
            (uint64_t)monitor->free_biggest_size * 100U /
            monitor->free_size;
        monitor->frag_pct = largest_percent >= 100U ?
            0U : (uint8_t)(100U - largest_percent);
    }
}

lv_result_t lv_mem_test_core(void)
{
    return jetkvm_memory_policy_lvgl_check() ?
        LV_RESULT_OK : LV_RESULT_INVALID;
}

#!/usr/bin/env python3
"""Run the production snapshot code and reject heap walks in live telemetry."""
from pathlib import Path
import re
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
source = (component / "jetkvm_memory_policy.c").read_text()
header = (component / "include/jetkvm_memory_policy.h").read_text()


def function(name):
    match = re.search(r"(?:static )?(?:void|uint32_t) " + name +
                      r"\([^;]*?\)\s*\{", source, re.S)
    assert match, name
    depth = 1
    pos = match.end()
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[match.start():pos]


types = [t for t in re.findall(r"typedef struct \{.*?\} \w+;", header, re.S)
         if t.endswith((" jetkvm_memory_policy_snapshot_t;",
                        " jetkvm_memory_policy_request_t;",
                        " jetkvm_memory_policy_tls_admission_t;"))]
assert len(types) == 3
declarations = source[source.index("typedef struct {"):
                      source.index("static uint32_t clamp_u32")]
stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
typedef int portMUX_TYPE;
typedef void *esp_timer_handle_t;
typedef int StaticSemaphore_t;
typedef int *SemaphoreHandle_t;
typedef struct {
    size_t total_free_bytes, minimum_free_bytes, largest_free_block;
    size_t allocated_blocks, free_blocks;
} multi_heap_info_t;
typedef multi_heap_info_t *multi_heap_handle_t;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) do {assert(*(lock)==0); ++*(lock);} while (0)
#define portEXIT_CRITICAL(lock) do {assert(*(lock)==1); --*(lock);} while (0)
#define JETKVM_MEMORY_RESERVATION_SLOTS 12U
#define JETKVM_MEMORY_INTERNAL_ADMISSION_FLOOR_BYTES 2100U
#define JETKVM_MEMORY_DMA_ADMISSION_FLOOR_BYTES 2100U
#define JETKVM_MEMORY_INTERNAL_HARD_FLOOR_BYTES 2000U
#define JETKVM_MEMORY_DMA_HARD_FLOOR_BYTES 2000U
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_DMA 4U
#define MALLOC_CAP_SPIRAM 8U
#define portMAX_DELAY 100U
#define pdTRUE 1
static unsigned walks;
static uint64_t now_us = 100;
static multi_heap_info_t caps_heap[16];
'''
functions = r'''
static void require_unlocked(void) {assert(s_snapshot_details_lock == 0);}
static size_t heap_caps_get_free_size(unsigned caps)
{require_unlocked(); return caps_heap[caps].total_free_bytes;}
static size_t heap_caps_get_minimum_free_size(unsigned caps)
{require_unlocked(); return caps_heap[caps].minimum_free_bytes;}
static size_t heap_caps_get_largest_free_block(unsigned caps)
{require_unlocked(); ++walks; return caps_heap[caps].largest_free_block;}
static void multi_heap_get_info(multi_heap_handle_t heap, multi_heap_info_t *info)
{require_unlocked(); assert(heap); ++walks; *info = *heap;}
static size_t multi_heap_free_size(multi_heap_handle_t heap)
{require_unlocked(); assert(heap); return heap->total_free_bytes;}
static size_t multi_heap_minimum_free_size(multi_heap_handle_t heap)
{require_unlocked(); assert(heap); return heap->minimum_free_bytes;}
static int64_t esp_timer_get_time(void) {return now_us;}
static int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned timeout)
{require_unlocked(); assert(timeout == portMAX_DELAY && *semaphore == 0); ++*semaphore; return pdTRUE;}
static void xSemaphoreGive(SemaphoreHandle_t semaphore)
{assert(*semaphore == 1); --*semaphore;}
'''
logic = "\n".join(function(name) for name in (
    "clamp_u32", "watch_held_tls", "memory_policy_snapshot",
    "jetkvm_memory_policy_get_snapshot",
    "jetkvm_memory_policy_get_snapshot_cached"))
checks = r'''
int main(void)
{
    jetkvm_memory_policy_snapshot_t s;
    multi_heap_info_t control = {900, 850, 800, 2, 3};
    multi_heap_info_t lvgl = {700, 650, 600, 4, 5};
    s_control_heap = &control;
    s_lvgl_heap = &lvgl;
    s_lvgl_initial_free = 1000;
    for (unsigned i = 0; i < 16; ++i)
        caps_heap[i] = (multi_heap_info_t){2000 + i, 1500 + i, 1000 + i, 0, 0};
    s_reservation_mutex = &s_reservation_mutex_storage;
    /* A TLS reservation that was admitted at 2,500 B free. */
    jetkvm_memory_policy_tls_admission_t record = {
        .owner = "ota-download", .admitted = 1,
        .held_internal_min = 2500, .held_dma_min = 2500,
    };
    s_reservations[0] = (reservation_slot_t){
        .active = true, .tls = true,
        .request = {.internal_bytes = 100, .dma_bytes = 50,
                    .psram_bytes = 300, .storage_ingress_bytes = 400},
        .record = &record,
        .admitted_internal_free = 2500, .admitted_dma_free = 2500,
        .internal_low = 2500, .dma_low = 2500,
        .internal_reserved_during = 100, .dma_reserved_during = 50,
    };
    jetkvm_memory_policy_get_snapshot(NULL);
    jetkvm_memory_policy_get_snapshot_cached(NULL);
    assert(walks == 0);

    memset(&s, 0xff, sizeof(s));
    jetkvm_memory_policy_get_snapshot_cached(&s);
    assert(walks == 0 && s.detailed_sample_timestamp_us == 0);
    assert(s.internal_largest == 0 && s.control_arena_largest == 0);
    assert(s.lvgl_allocated_blocks == 0 && s.lvgl_free_blocks == 0);
    assert(s.internal_free == 2003 && s.dma_free == 2005 && s.psram_free == 2010);
    assert(s.control_arena_free == 900 && s.lvgl_arena_free == 700);
    assert(s.lvgl_arena_used == 300 && s.lvgl_arena_peak_used == 350);
    assert(s.active_reservations == 1 && s.active_tls_reservations == 1);
    assert(s.reservation_internal_bytes == 100 && s.tls_reservation_dma_bytes == 50);
    /* A snapshot is a look at the held reservation: its reading of the free
     * bytes, 2,003 and 2,005 B, is under the floors of this test (2,100 B).
     * The heap's least free bytes since boot (1,503 and 1,505 B) are sums of
     * the regions' own marks and are no reading. */
    assert(record.held_internal_min == 2003 && record.held_dma_min == 2005);
    assert(record.held_internal_fall_max == 497 && record.held_dma_fall_max == 495);
    assert(record.held_under_floor == 1);
    /* The fall is larger than the reservation and the 100 B between the
     * floors of this test: an overrun. */
    assert(record.held_overruns == 1);

    jetkvm_memory_policy_get_snapshot(&s);
    assert(walks == 5 && s.detailed_sample_timestamp_us == 100);
    assert(s.internal_largest == 1003 && s.dma_largest == 1005 && s.psram_largest == 1010);
    assert(s.control_arena_largest == 800 && s.lvgl_arena_largest == 600);
    assert(s.lvgl_allocated_blocks == 4 && s.lvgl_free_blocks == 5);

    now_us = 200;
    caps_heap[3] = (multi_heap_info_t){1100, 1050, 1000, 0, 0};
    control = (multi_heap_info_t){550, 500, 450, 12, 13};
    lvgl = (multi_heap_info_t){350, 300, 250, 14, 15};
    s_reservations[0].tls = false;
    s_reservations[0].request.internal_bytes = 90;
    s_video_rebuild.active = true;
    s_video_rebuild.working_set_bytes = 12345;
    atomic_store(&s_admission_denials, 9);
    for (unsigned poll = 0; poll < 10; ++poll) {
        jetkvm_memory_policy_get_snapshot_cached(&s);
        assert(walks == 5 && s.detailed_sample_timestamp_us == 100);
        assert(s.internal_free == 1100 && s.internal_minimum == 1050);
        assert(s.control_arena_free == 550 && s.control_arena_minimum == 500);
        assert(s.lvgl_arena_free == 350 && s.lvgl_arena_minimum == 300);
        assert(s.lvgl_arena_used == 650 && s.lvgl_arena_peak_used == 700);
        assert(s.internal_largest == 1003 && s.control_arena_largest == 800);
        assert(s.lvgl_arena_largest == 600 && s.lvgl_allocated_blocks == 4);
        assert(s.lvgl_free_blocks == 5 && s.active_reservations == 1);
        assert(s.active_tls_reservations == 0 && s.tls_reservation_dma_bytes == 0);
        /* One hold counts once, and a reservation that is not TLS is not
         * looked at: the internal heap's 1,100 B is not taken. */
        assert(record.held_under_floor == 1 && record.held_internal_min == 2003);
        assert(s.reservation_internal_bytes == 90 && s.admission_denials == 9);
        assert(s.video_rebuild_active && s.video_rebuild_working_set_bytes == 12345);
    }
    jetkvm_memory_policy_get_snapshot(&s);
    assert(walks == 10 && s.detailed_sample_timestamp_us == 200);
    jetkvm_memory_policy_get_snapshot_cached(&s);
    assert(walks == 10 && s.detailed_sample_timestamp_us == 200);
    assert(s.internal_largest == 1000 && s.control_arena_largest == 450);
    assert(s.lvgl_arena_largest == 250 && s.lvgl_allocated_blocks == 14);
    assert(s.lvgl_free_blocks == 15);
    puts("memory policy snapshots: zero live heap walks, fresh counters, explicit detail age: PASS");
}
'''
with tempfile.TemporaryDirectory(prefix="p4-snapshot-cache-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(stubs + "\n".join(types) + declarations +
                                 functions + logic + checks)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-variable", "-O2", str(path / "test.c"),
                    "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)

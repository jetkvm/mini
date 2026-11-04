#!/usr/bin/env python3
"""A PSRAM buffer beside another owner's DMA ticket, with the real slot logic.

The numbers are a Mini's refusal of an OS Services file drop (82,112 B of
PSRAM) during an upload: 79,835 B of DMA-capable RAM free while an update
check's TLS ticket held back 16,384 B, under the 64 KiB DMA floor, which the
PSRAM buffer does not touch. PSRAM had 539,324 B to spare.
"""

from pathlib import Path
import os
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'jetkvm_memory_policy.c').read_text()
HEADER = (ROOT / 'include/jetkvm_memory_policy.h').read_text()


def function(name):
    match = re.search(r'(?:static )?(?:(?:bool|void|esp_err_t|int32_t|size_t) |jetkvm_memory_policy_tls_admission_t \*)' + name + r'\([^;]*?\)\s*\{', SOURCE, re.S)
    assert match, name
    depth, end = 1, match.end()
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[match.start():end]


logic = '\n'.join(function(name) for name in (
    'has_projected_floor', 'projected_headroom', 'record_tls_admission', 'watch_held_tls',
    'request_shape_valid', 'psram_fits', 'request_fits',
    'video_rebuild_blocks', 'reserve_with_domain', 'jetkvm_memory_policy_reserve',
    'jetkvm_memory_policy_reserve_psram', 'reserve_tls_with_domain',
    'jetkvm_memory_policy_reserve_tls', 'jetkvm_memory_policy_release',
))
macros = []
for name in sorted(set(re.findall(r'JETKVM_MEMORY_\w+', logic))):
    match = re.search(r'^#define ' + name + r'\s+((?:[^\n]*\\\n)*[^\n]*)', HEADER, re.M)
    assert match, name
    macros.append(match.group(0))
types = [value for value in re.findall(r'typedef struct \{.*?\} \w+;', HEADER, re.S)
         if value.endswith((' jetkvm_memory_policy_snapshot_t;',
                            ' jetkvm_memory_policy_request_t;',
                            ' jetkvm_memory_policy_reservation_t;',
                            ' jetkvm_memory_policy_tls_admission_t;'))]
assert len(types) == 4
slot = re.search(r'typedef struct \{.*?\} reservation_slot_t;', SOURCE, re.S).group(0)
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
'''
mock = r'''
/* The Ethernet board: no blocks of the Wi-Fi link in a TLS reservation. */
#define JETKVM_MEMORY_TLS_LINK_BLOCKS 0U
#define JETKVM_MEMORY_WIFI_LINK_BLOCK_BYTES 1604U
typedef int esp_err_t;
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_DMA 4U
#define MALLOC_CAP_SPIRAM 8U
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define ESP_LOGW(...) ((void)0)
static jetkvm_memory_policy_snapshot_t memory;
static int mutex_storage;
static void *s_reservation_mutex = &mutex_storage;
static bool held;
static reservation_slot_t s_reservations[JETKVM_MEMORY_RESERVATION_SLOTS];
static uint32_t s_next_reservation_generation = 1;
static struct { bool active; size_t working_set_bytes; } s_video_rebuild;
static jetkvm_memory_policy_tls_admission_t
    tls_admission_storage[JETKVM_MEMORY_TLS_ADMISSION_OWNERS];
static jetkvm_memory_policy_tls_admission_t *s_tls_admissions = tls_admission_storage;
static atomic_uint s_admission_denials, s_video_rebuild_blocked_materializations;
static int xSemaphoreTake(void *mutex, uint32_t ticks) {
    assert(mutex == &mutex_storage && ticks == portMAX_DELAY && !held);
    held = true; return pdTRUE;
}
static void xSemaphoreGive(void *mutex) {
    assert(mutex == &mutex_storage && held); held = false;
}
/* The timer that reads free memory while a TLS reservation is held does
 * not run in this test. */
static void watch_run_locked(bool run) { (void)run; }
static size_t heap_caps_get_free_size(unsigned caps) {
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return memory.internal_free;
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) return memory.dma_free;
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); return memory.psram_free;
}
static size_t heap_caps_get_largest_free_block(unsigned caps) {
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return memory.internal_largest;
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) return memory.dma_largest;
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); return memory.psram_largest;
}
'''
cases = r'''
#define WINDOW 1310720U
#define DROP 82112U
#define TERMINAL 196848U

/* A channel's buffers, admitted and released. */
static bool channel_fits(size_t size) {
    jetkvm_memory_policy_reservation_t ticket = {0};
    const bool admitted =
        jetkvm_memory_policy_reserve_psram("OS Services channel", size, &ticket);
    assert(admitted == ticket.active);
    jetkvm_memory_policy_release(&ticket);
    assert(!held);
    return admitted;
}

int main(void) {
    memory = (jetkvm_memory_policy_snapshot_t) {
        .internal_free = 118915, .dma_free = 79835 + 16384,
        .psram_free = 6126460, .internal_largest = 65536,
        .dma_largest = 65536, .psram_largest = 5767168,
    };
    /* The upload's ingress lease and the reservation of a TLS connection. */
    const jetkvm_memory_policy_request_t lease = {
        .psram_bytes = WINDOW, .psram_largest_block = 2048U,
        .storage_ingress_bytes = 1835008U,
    };
    jetkvm_memory_policy_reservation_t upload = {0}, tls = {0};
    assert(jetkvm_memory_policy_reserve("http-storage-upload-psram", &lease, &upload));
    assert(jetkvm_memory_policy_reserve_tls("osvc-update", &tls));
    memory.dma_free = JETKVM_MEMORY_DMA_ADMISSION_FLOOR_BYTES +
                      JETKVM_MEMORY_TLS_CONNECTION_RESERVE_BYTES - 1U;

    /* DMA free less the reservation is 1 B under its floor: a request that
     * takes DMA RAM is refused, and so is the drop as a full-domain request. */
    const jetkvm_memory_policy_request_t drop_as_before = {
        .psram_bytes = DROP, .psram_largest_block = DROP,
    };
    jetkvm_memory_policy_reservation_t ticket = {0};
    assert(!jetkvm_memory_policy_reserve("OS Services channel", &drop_as_before, &ticket));
    const jetkvm_memory_policy_request_t with_dma = {
        .internal_bytes = 1U, .dma_bytes = 1U,
    };
    assert(!jetkvm_memory_policy_reserve("dma", &with_dma, &ticket));

    /* A PSRAM-only buffer is admitted: the drop and the terminal. */
    assert(channel_fits(DROP) && channel_fits(TERMINAL));

    /* The PSRAM floor, after the upload's pending window, still decides:
     * one byte more than fits is refused. */
    memory.psram_free = JETKVM_MEMORY_PSRAM_ADMISSION_FLOOR_BYTES + WINDOW + DROP;
    assert(channel_fits(DROP) && !channel_fits(DROP + 1U));
    /* So does the largest block. */
    memory.psram_free = 6126460;
    memory.psram_largest = DROP - 1U;
    assert(!channel_fits(DROP));
    memory.psram_largest = 5767168;
    /* And the pending PSRAM tickets' largest block. */
    jetkvm_memory_policy_release(&upload);
    assert(jetkvm_memory_policy_reserve_psram("big", 1048576U, &upload));
    memory.psram_largest = 1048575U;
    assert(!channel_fits(DROP));

    jetkvm_memory_policy_release(&upload);
    jetkvm_memory_policy_release(&tls);
    assert(!held);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='p4-psram-only-admission-') as temporary:
    directory = Path(temporary)
    source = directory / 'test.c'
    executable = directory / 'test'
    source.write_text('\n'.join([prefix, *macros, *types, slot, mock, logic, cases]))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
print('PSRAM-only buffers beside a DMA ticket, PSRAM floor and blocks kept: PASS')

#!/usr/bin/env python3
"""Strict admissions after a resolution change split the largest PSRAM block.

After 720p60 -> 1080p30 -> 720p60 the Ethernet Mini kept about
10.3 MB of PSRAM free, but its largest free block was 3,080,192 bytes. The
video buffers are allocated once at boot and a mode change does not allocate
them again, so a TLS connect or an upload must be admitted in that state. The
PSRAM floor and each request's real largest block still apply.

This compiles the real reserve functions of jetkvm_memory_policy.c against
fake heap values.
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
    match = re.search(r'(?:static )?(?:(?:bool|void|esp_err_t|int32_t|size_t) |jetkvm_memory_policy_tls_admission_t \*)' + name +
                      r'\([^;]*?\)\s*\{', SOURCE, re.S)
    assert match, name
    depth, end = 1, match.end()
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[match.start():end]


logic = '\n'.join(function(name) for name in (
    'has_projected_floor', 'projected_headroom', 'record_tls_admission', 'watch_held_tls',
    'request_shape_valid', 'psram_fits', 'request_fits', 'video_rebuild_blocks', 'reserve_with_domain',
    'jetkvm_memory_policy_reserve', 'reserve_tls_with_domain',
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
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
'''
mock = r'''
/* The Ethernet board: no blocks of the Wi-Fi link in a TLS reservation. */
#define JETKVM_MEMORY_TLS_LINK_BLOCKS 0U
#define JETKVM_MEMORY_WIFI_LINK_BLOCK_BYTES 1604U
typedef int esp_err_t;
#define ESP_OK 0
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
static reservation_slot_t s_reservations[JETKVM_MEMORY_RESERVATION_SLOTS];
static uint32_t s_next_reservation_generation = 1;
static struct { bool active; size_t working_set_bytes; } s_video_rebuild;
static jetkvm_memory_policy_tls_admission_t
    tls_admission_storage[JETKVM_MEMORY_TLS_ADMISSION_OWNERS];
static jetkvm_memory_policy_tls_admission_t *s_tls_admissions = tls_admission_storage;
static atomic_uint s_admission_denials, s_video_rebuild_blocked_materializations;
static int xSemaphoreTake(void *mutex, uint32_t ticks) {
    (void)mutex; (void)ticks; return pdTRUE;
}
static void xSemaphoreGive(void *mutex) { (void)mutex; }
/* The timer that reads free memory while a TLS reservation is held does
 * not run in this test. */
static void watch_run_locked(bool run) { (void)run; }
static size_t heap_caps_get_free_size(unsigned caps) {
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return memory.internal_free;
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) return memory.dma_free;
    return memory.psram_free;
}
static size_t heap_caps_get_largest_free_block(unsigned caps) {
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) return memory.internal_largest;
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) return memory.dma_largest;
    return memory.psram_largest;
}
'''
cases = r'''
static int failures;
#define EXPECT(condition, what) do { \
    if (!(condition)) { printf("FAIL: %s\n", what); failures++; } \
    else { printf("ok:   %s\n", what); } \
} while (0)

/* The PSRAM fallback request of an HTTP upload: 1.25 MiB TCP window, two
 * 24 KiB halves and the 6 KiB writer stack, largest allocation 48 KiB. */
static const jetkvm_memory_policy_request_t upload = {
    .internal_bytes = 3072, .internal_largest_block = 3072,
    .dma_bytes = 3072, .dma_largest_block = 3072,
    .psram_bytes = 1310720 + 49152 + 6144, .psram_largest_block = 49152,
    .storage_ingress_bytes = 1835008,
};

static bool tls(const char *owner) {
    jetkvm_memory_policy_reservation_t ticket = {0};
    bool accepted = jetkvm_memory_policy_reserve_tls(owner, &ticket);
    jetkvm_memory_policy_release(&ticket);
    return accepted;
}

static bool strict(const jetkvm_memory_policy_request_t *request) {
    jetkvm_memory_policy_reservation_t ticket = {0};
    bool accepted = jetkvm_memory_policy_reserve("http-storage-upload-psram",
                                                 request, &ticket);
    jetkvm_memory_policy_release(&ticket);
    return accepted;
}

int main(void) {
    /* Video runs at 720p60 after 720p60 -> 1080p30 -> 720p60. */
    memory = (jetkvm_memory_policy_snapshot_t) {
        .internal_free = 120663, .dma_free = 89779,
        .internal_largest = 47104, .dma_largest = 47104,
        .psram_free = 10300000, .psram_largest = 3080192,
    };
    EXPECT(tls("https-media-check"),
           "TLS connect admitted: free 10.3 MB, largest 3,080,192");
    EXPECT(tls("cloud-connect"),
           "cloud TLS connect admitted in the same state");
    EXPECT(strict(&upload),
           "HTTP upload admitted in the same state");

    /* The 4 MiB PSRAM floor stays. */
    memory.psram_free = JETKVM_MEMORY_PSRAM_ADMISSION_FLOOR_BYTES - 1;
    EXPECT(!tls("https-media-check"), "TLS refused 1 byte below the 4 MiB floor");
    memory.psram_free = JETKVM_MEMORY_PSRAM_ADMISSION_FLOOR_BYTES;
    EXPECT(tls("https-media-check"), "TLS admitted at the 4 MiB floor");
    memory.psram_free = JETKVM_MEMORY_PSRAM_ADMISSION_FLOOR_BYTES +
                        upload.psram_bytes - 1;
    EXPECT(!strict(&upload), "upload refused 1 byte below floor + request");
    memory.psram_free++;
    EXPECT(strict(&upload), "upload admitted at floor + request");

    /* The request's own largest allocation must still fit. */
    memory.psram_free = 10300000;
    memory.psram_largest = upload.psram_largest_block - 1;
    EXPECT(!strict(&upload), "upload refused when its 48 KiB block does not fit");
    memory.psram_largest = upload.psram_largest_block;
    EXPECT(strict(&upload), "upload admitted when its 48 KiB block fits");

    for (size_t i = 0; i < JETKVM_MEMORY_RESERVATION_SLOTS; i++)
        if (s_reservations[i].active) { puts("FAIL: leaked ticket"); failures++; }
    if (failures) {
        printf("fragmented PSRAM admission: %d FAILED\n", failures);
        return 1;
    }
    puts("fragmented PSRAM admission: strict owners admitted, floors kept: PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='p4-fragmented-admission-') as temporary:
    directory = Path(temporary)
    source = directory / 'test.c'
    executable = directory / 'test'
    source.write_text('\n'.join([prefix, *macros, *types, slot, mock, logic, cases]))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})

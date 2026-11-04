#!/usr/bin/env python3
"""PSRAM buffers during a video rebuild, with the real slot logic.

The memory is the Mini's at the start of the OS Services installer download:
the sender stopped, the Mini switched to HDMI capture,
and the rebuild for the new capture mode was active for under 0.5 s when the
download asked for its 64 KiB PSRAM buffer, with about 6.9 MB of PSRAM free
and an 8 MB block. A rebuild into the video pool allocates no PSRAM, so a
PSRAM-only buffer is judged against memory at once: a file drop's channel
(82,112 B) that opens in that time is admitted. A
rebuild with a PSRAM working set (the BGR888 diagnostic's output) keeps
PSRAM buffers out until it ends, and wait_reserve_psram() waits for that.
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
    'video_rebuild_blocks', 'reserve_with_domain', 'jetkvm_memory_policy_reserve_psram',
    'jetkvm_memory_policy_wait_reserve_psram', 'jetkvm_memory_policy_release',
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
/* vTaskDelay() counts the waits and ends the rebuild at one of them. */
#define pdMS_TO_TICKS(ms) (ms)
static unsigned delays, rebuild_ends_at_delay;
static void vTaskDelay(uint32_t ticks) {
    assert(!held && ticks == JETKVM_MEMORY_PSRAM_RETRY_MS);
    if (++delays == rebuild_ends_at_delay) s_video_rebuild.active = false;
}
'''
cases = r'''
#define BUFFER (64U * 1024U)

static void reset(void) {
    assert(!held);
    memset(s_reservations, 0, sizeof(s_reservations));
    s_video_rebuild.active = false;
    s_video_rebuild.working_set_bytes = 0;
    s_admission_denials = s_video_rebuild_blocked_materializations = 0;
    delays = rebuild_ends_at_delay = 0;
    /* A Mini at the refusal (GET /webrtc/stats). */
    memory = (jetkvm_memory_policy_snapshot_t) {
        .internal_free = 167675, .dma_free = 136791,
        .psram_free = 6998456, .internal_largest = 108544,
        .dma_largest = 108544, .psram_largest = 7995392,
    };
}

/* A rebuild that allocates the BGR888 diagnostic's output. */
static void rebuild_with_working_set(void) {
    s_video_rebuild.active = true;
    s_video_rebuild.working_set_bytes = 1920U * 1080U * 3U;
}

int main(void) {
    jetkvm_memory_policy_reservation_t ticket = {0};

    /* A rebuild into the video pool: a file drop's channel buffer is
     * admitted at once, with no denial, and so is the download's. */
    reset();
    s_video_rebuild.active = true;
    assert(jetkvm_memory_policy_reserve_psram("OS Services channel", 82112U, &ticket));
    assert(ticket.active && s_admission_denials == 0 &&
           s_video_rebuild_blocked_materializations == 0);
    jetkvm_memory_policy_release(&ticket);
    assert(jetkvm_memory_policy_wait_reserve_psram("storage-download", BUFFER, &ticket));
    assert(ticket.active && delays == 0 && s_admission_denials == 0);
    jetkvm_memory_policy_release(&ticket);
    /* Memory still decides: the PSRAM floor refuses it. */
    memory.psram_free = JETKVM_MEMORY_PSRAM_ADMISSION_FLOOR_BYTES + BUFFER - 1U;
    assert(!jetkvm_memory_policy_reserve_psram("storage-download", BUFFER, &ticket));
    assert(!ticket.active && s_admission_denials == 1 &&
           s_video_rebuild_blocked_materializations == 0);

    /* The refusal: reserve_psram() during a rebuild with a working set,
     * with memory to spare. */
    reset();
    rebuild_with_working_set();
    assert(!jetkvm_memory_policy_reserve_psram("storage-download", BUFFER, &ticket));
    assert(!ticket.active && s_admission_denials == 1 &&
           s_video_rebuild_blocked_materializations == 1);

    /* The wait: admitted when the rebuild ends, with no denial recorded. */
    reset();
    rebuild_with_working_set();
    rebuild_ends_at_delay = 3;
    assert(jetkvm_memory_policy_wait_reserve_psram("storage-download", BUFFER, &ticket));
    assert(ticket.active && delays == 3 && s_admission_denials == 0);
    assert(s_reservations[ticket.slot].request.psram_bytes == BUFFER &&
           s_reservations[ticket.slot].request.psram_largest_block == BUFFER);
    jetkvm_memory_policy_release(&ticket);

    /* A rebuild that outlasts the wait: one denial, after the whole wait. */
    reset();
    rebuild_with_working_set();
    assert(!jetkvm_memory_policy_wait_reserve_psram("storage-download", BUFFER, &ticket));
    assert(!ticket.active);
    assert(delays == JETKVM_MEMORY_PSRAM_WAIT_MS / JETKVM_MEMORY_PSRAM_RETRY_MS);
    assert(s_admission_denials == 1 && s_video_rebuild_blocked_materializations == 1);

    /* A rebuild over a PSRAM shortage: refused when the rebuild ends, not
     * after the whole wait. */
    reset();
    rebuild_with_working_set();
    rebuild_ends_at_delay = 3;
    memory.psram_free = JETKVM_MEMORY_PSRAM_ADMISSION_FLOOR_BYTES + BUFFER - 1U;
    assert(!jetkvm_memory_policy_wait_reserve_psram("storage-download", BUFFER, &ticket));
    assert(!ticket.active && delays == 3 && s_admission_denials == 1 &&
           s_video_rebuild_blocked_materializations == 0);

    /* No rebuild: admitted at once. */
    reset();
    assert(jetkvm_memory_policy_wait_reserve_psram("storage-download", BUFFER, &ticket));
    assert(ticket.active && delays == 0);
    jetkvm_memory_policy_release(&ticket);

    /* PSRAM itself short (the floor, then the largest block): refused at
     * once, without a wait. */
    reset();
    memory.psram_free = JETKVM_MEMORY_PSRAM_ADMISSION_FLOOR_BYTES + BUFFER - 1U;
    assert(!jetkvm_memory_policy_wait_reserve_psram("storage-download", BUFFER, &ticket));
    assert(!ticket.active && delays == 0 && s_admission_denials == 1);
    reset();
    memory.psram_largest = BUFFER - 1U;
    assert(!jetkvm_memory_policy_wait_reserve_psram("storage-download", BUFFER, &ticket));
    assert(!ticket.active && delays == 0 && s_admission_denials == 1);

    /* A full slot table: refused at once. */
    reset();
    for (size_t i = 0; i < JETKVM_MEMORY_RESERVATION_SLOTS; i++)
        s_reservations[i].active = true;
    assert(!jetkvm_memory_policy_wait_reserve_psram("storage-download", BUFFER, &ticket));
    assert(!ticket.active && delays == 0 && s_admission_denials == 1);

    /* No reservation to fill: refused, nothing dereferenced. */
    reset();
    assert(!jetkvm_memory_policy_wait_reserve_psram("storage-download", BUFFER, NULL));
    assert(delays == 0);

    reset();
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='p4-psram-wait-admission-') as temporary:
    directory = Path(temporary)
    source = directory / 'test.c'
    executable = directory / 'test'
    source.write_text('\n'.join([prefix, *macros, *types, slot, mock, logic, cases]))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
print('PSRAM buffers beside a rebuild into the video pool; a rebuild with a working set is waited out: PASS')

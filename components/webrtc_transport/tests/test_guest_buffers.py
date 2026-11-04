#!/usr/bin/env python3
"""Guest video in lent buffers (production code): the slots switch to them
and back only once no mode is set and no slot is filled or sent, leases
carry the lent buffer and its size, and the CPU's writes leave the cache
before a buffer changes hands."""
from pathlib import Path
import re
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
source = (component / 'webrtc_transport.c').read_text()
header = (component / 'include/webrtc_transport.h').read_text()
internal = (component / 'webrtc_transport_ctx_internal.h').read_text()

def function(signature):
    start = source.index(signature)
    pos = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]

def structure(text, name):
    return re.search(r'typedef struct \{(?:(?!typedef struct).)*?\} '+name+r';', text, re.S)[0]

functions = '\n'.join(function(s) for s in (
    'static int reserve_slot_locked(',
    'static bool reservation_matches_locked(',
    'static esp_err_t lease_access_unit(',
    'esp_err_t webrtc_transport_reserve_guest_access_unit(',
    'void webrtc_transport_cancel_access_unit(',
    'static void note_cpu_writes(',
    'void webrtc_transport_cancel_cpu_access_unit(',
    'esp_err_t webrtc_transport_set_guest_buffers(',
))
stat_fields = sorted(set(re.findall(r'ctx->stats\.(\w+)', functions)) - {'stage_age'})
types = '\n'.join([
    re.search(r'typedef enum \{[^}]+\} au_slot_state_t;', internal)[0],
    structure(internal, 'au_slot_t'),
    structure(header, 'webrtc_transport_au_reservation_t'),
])
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_WEBRTC_TRANSPORT_AU_SLOTS 2
#define CONFIG_WEBRTC_TRANSPORT_MAX_AU_BYTES 512
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define ESP_CACHE_MSYNC_FLAG_INVALIDATE 1
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 4
typedef int esp_err_t;
typedef void *TaskHandle_t;
static int s_task;
static TaskHandle_t xTaskGetCurrentTaskHandle(void) {return &s_task;}
static int lock_depth;
#define portENTER_CRITICAL(mux) do {(void)(mux); assert(!lock_depth); ++lock_depth;} while(0)
#define portEXIT_CRITICAL(mux) do {(void)(mux); assert(lock_depth==1); --lock_depth;} while(0)
''' + types + r'''
typedef struct {
    int lock;
    au_slot_t slots[2];
    bool started, session_active, mode_valid, video_paused;
    uint16_t width, height, fps;
    uint32_t generation, video_epoch, submitters;
    TaskHandle_t slot_waiter;
    uint64_t next_au_order;
    struct { struct {uint32_t epoch;} stage_age;
        /* STATS */
    } stats;
} transport_ctx_t;
static transport_ctx_t s_ctx;
static unsigned notifications;
static void notify_peer_task(transport_ctx_t *ctx) {(void)ctx; assert(!lock_depth); ++notifications;}
/* The mode drain: clears the mode, then reports whether the slots went
 * quiet in time. */
static esp_err_t s_drain;
static unsigned s_drains;
static esp_err_t webrtc_transport_clear_mode_and_wait(uint32_t timeout_ms)
{
    assert(timeout_ms == 3000 && !lock_depth);
    ++s_drains;
    s_ctx.mode_valid = false;
    if (s_drain == ESP_OK)
        for (unsigned i = 0; i < 2; i++) assert(s_ctx.slots[i].state == AU_SLOT_FREE);
    return s_drain;
}
/* Cache write-backs: where, how much, and with invalidation. */
static struct { void *data; size_t size; } s_cleaned[4];
static unsigned s_cleans;
static esp_err_t esp_cache_msync(void *data, size_t size, int flags)
{
    assert(!lock_depth && size % 128 == 0 && s_cleans < 4);
    if (s_ctx.submitters) {
        assert(flags == ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    } else {
        assert(flags == (ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE));
    }
    s_cleaned[s_cleans].data = data;
    s_cleaned[s_cleans++].size = size;
    return ESP_OK;
}
void webrtc_transport_cancel_access_unit(webrtc_transport_au_reservation_t *reservation);
'''.replace('/* STATS */', '\n'.join('uint64_t '+f+';' for f in stat_fields))
checks = r'''
static uint8_t own[2][512], lent[2][4096];
static void guest_mode(void)
{
    s_ctx.mode_valid = true;
    s_ctx.width = 3840; s_ctx.height = 2160; s_ctx.fps = 30;
}
static webrtc_transport_au_reservation_t lease(void)
{
    webrtc_transport_au_reservation_t r = {0};
    assert(webrtc_transport_reserve_guest_access_unit(3840, 2160, 30, &r) == ESP_OK);
    return r;
}
int main(void)
{
    s_ctx = (transport_ctx_t){.started = true, .session_active = true, .generation = 1};
    for (unsigned i = 0; i < 2; i++) {
        s_ctx.slots[i].own = s_ctx.slots[i].data = own[i];
        s_ctx.slots[i].capacity = 512;
    }
    /* The transport's own slot 0 has a CPU prefix left from earlier. */
    s_ctx.slots[0].cpu_dirty_bytes = 100;
    uint8_t *const buffers[2] = {lent[0], lent[1]};

    /* Lending: the mode drains first; own dirty lines go before the switch. */
    guest_mode();
    assert(webrtc_transport_set_guest_buffers(buffers, 4100, 3000) == ESP_OK);
    assert(s_drains == 1);
    assert(s_cleans == 1 && s_cleaned[0].data == own[0] && s_cleaned[0].size == 128);
    guest_mode();
    webrtc_transport_au_reservation_t a = lease(), b = lease();
    assert(a.data == lent[0] && b.data == lent[1] && a.capacity == 4096 && b.capacity == 4096);

    /* The CPU writes; the frames end. */
    webrtc_transport_cancel_cpu_access_unit(&a, 3000);
    webrtc_transport_cancel_cpu_access_unit(&b, 129);
    assert(s_ctx.submitters == 0);

    /* A drain that times out keeps the lent buffers in place. */
    s_drain = ESP_ERR_TIMEOUT;
    assert(webrtc_transport_set_guest_buffers(NULL, 0, 3000) == ESP_ERR_TIMEOUT);
    assert(s_ctx.slots[0].data == lent[0] && s_cleans == 1);

    /* Giving them back: each written prefix leaves the cache, rounded to
     * whole lines, and the slots are the transport's own again. */
    s_drain = ESP_OK;
    assert(webrtc_transport_set_guest_buffers(NULL, 0, 3000) == ESP_OK);
    assert(s_cleans == 3);
    assert(s_cleaned[1].data == lent[0] && s_cleaned[1].size == 3072);
    assert(s_cleaned[2].data == lent[1] && s_cleaned[2].size == 256);
    guest_mode();
    a = lease();
    assert(a.data == own[0] && a.capacity == 512);
    webrtc_transport_cancel_access_unit(&a);
    assert(s_ctx.slots[0].cpu_dirty_bytes == 0 && s_ctx.slots[1].cpu_dirty_bytes == 0);
    puts("guest buffers: switch after the drain, leases, cache write-back: OK");
}
'''
with tempfile.TemporaryDirectory(prefix='webrtc-guest-buffers-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(preamble + functions + checks)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)

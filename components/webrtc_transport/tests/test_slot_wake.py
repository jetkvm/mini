#!/usr/bin/env python3
"""A service video frame that waits for a slot (production code): the task
that found every slot taken is woken by each slot the peer task frees, after
a sent frame, a failed send or a frame dropped for sync, and only once."""
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
    'static bool service_video_locked(',
    'static int take_next_ready_slot(',
    'static TaskHandle_t free_sending_slot_locked(',
    'static void release_sending_slot(',
    'static bool reservation_matches_locked(',
    'static esp_err_t lease_access_unit(',
    'esp_err_t webrtc_transport_reserve_guest_access_unit(',
    'void webrtc_transport_cancel_access_unit(',
    'static bool slot_is_decodable_sync(',
    'static void send_next_access_unit(',
))
special = {'stage_age', 'au_take_wait', 'video_send_by_size'}
stat_fields = sorted(set(re.findall(r'ctx->stats\.(\w+)', functions)) - special)
types = '\n'.join([
    re.search(r'typedef enum \{[^}]+\} au_slot_state_t;', internal)[0],
    structure(internal, 'au_slot_t'),
    structure(internal, 'active_send_trace_t'),
    structure(header, 'webrtc_transport_send_cost_t'),
    structure(header, 'webrtc_transport_au_reservation_t'),
])
preamble = r'''
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_WEBRTC_TRANSPORT_AU_SLOTS 2
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 4
#define ESP_PEER_ERR_NONE 0
#define ESP_PEER_ERR_WOULD_BLOCK (-9)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef void *TaskHandle_t;
static int s_task, notified;
static TaskHandle_t xTaskGetCurrentTaskHandle(void) {return &s_task;}
static int lock_depth;
#define portENTER_CRITICAL(mux) do {(void)(mux); assert(!lock_depth); ++lock_depth;} while(0)
#define portEXIT_CRITICAL(mux) do {(void)(mux); assert(lock_depth==1); --lock_depth;} while(0)
static void xTaskNotifyGive(TaskHandle_t task) {assert(!lock_depth && task==&s_task); ++notified;}
typedef struct { struct { uint64_t count, total_us; } lwip_sendto, srtp_protect; }
    webrtc_transport_network_timing_stats_t;
typedef int webrtc_transport_stage_age_summary_t;
typedef struct { uint32_t pts; uint8_t *data; int size; } esp_peer_video_frame_t;
''' + types + r'''
typedef struct {
    int lock;
    au_slot_t slots[2];
    bool started, session_active, mode_valid, video_paused, guest_mode;
    bool peer_connected, waiting_for_sync, sending_video;
    void *peer;
    uint16_t width, height, fps;
    uint32_t generation, video_epoch, active_peer_generation, submitters;
    uint32_t last_sent_au_generation;
    uint32_t sync_idr_pending_generation, sync_idr_committed_generation;
    uint64_t next_au_order, last_sent_au_order;
    TaskHandle_t slot_waiter;
    active_send_trace_t active_send_trace;
    struct { struct {uint32_t epoch;} stage_age;
        int au_take_wait;
        webrtc_transport_send_cost_t video_send_by_size[4];
        /* STATS */
    } stats;
} transport_ctx_t;
static transport_ctx_t s_ctx;
static void notify_peer_task(transport_ctx_t *ctx) {(void)ctx; assert(!lock_depth);}
static int64_t esp_timer_get_time(void) {return 2000;}
static esp_err_t esp_cache_msync(void *data, size_t size, int flags)
{(void)data; (void)size; (void)flags; assert(!lock_depth); return ESP_OK;}
static void record_duration(int *h, int64_t us) {(void)h; (void)us;}
static bool slot_needs_reference_sync(const transport_ctx_t *ctx, const au_slot_t *slot)
{(void)ctx; (void)slot; return false;}
static void request_video_recovery_locked(transport_ctx_t *ctx) {(void)ctx;}
static void record_send_intervals_locked(transport_ctx_t *ctx, uint32_t generation,
                                         int64_t encode_done_us, int64_t send_start_us)
{(void)ctx; (void)generation; (void)encode_done_us; (void)send_start_us;}
static void aggregate_active_trace_locked(transport_ctx_t *ctx, int64_t now_us)
{(void)ctx; (void)now_us;}
static bool fail_sync_idr_locked(transport_ctx_t *ctx, uint32_t generation, bool requeue)
{(void)ctx; (void)generation; (void)requeue; return false;}
static void webrtc_network_timing_begin(uint32_t epoch, uint32_t rtp, int64_t eof)
{(void)epoch; (void)rtp; (void)eof;}
static void webrtc_network_timing_cancel(uint32_t epoch, uint32_t rtp, int64_t eof)
{(void)epoch; (void)rtp; (void)eof;}
static void webrtc_network_timing_snapshot(webrtc_transport_network_timing_stats_t *s,
                                           webrtc_transport_stage_age_summary_t *a)
{memset(s, 0, sizeof(*s)); *a = 0;}
static void jetkvm_virtual_media_note_video_frame(void) {}
/* The send runs outside the lock, on a slot the peer task holds. */
static int send_result, sends;
static int esp_peer_send_video(void *peer, esp_peer_video_frame_t *frame)
{
    assert(!lock_depth && peer == &s_ctx && frame->size > 0);
    ++sends;
    return send_result;
}
void webrtc_transport_cancel_access_unit(webrtc_transport_au_reservation_t *reservation);
'''.replace('/* STATS */', '\n'.join('uint64_t '+f+';' for f in stat_fields))
checks = r'''
static uint8_t pool[2][512];
static void reset(void)
{
    s_ctx = (transport_ctx_t){.started=true, .session_active=true, .mode_valid=true,
        .guest_mode=true, .peer_connected=true, .peer=&s_ctx,
        .width=1920, .height=1080, .fps=60, .generation=3, .active_peer_generation=3};
    for (unsigned i = 0; i < 2; i++) {
        s_ctx.slots[i].data = s_ctx.slots[i].own = pool[i];
        s_ctx.slots[i].capacity = 512;
    }
    notified = sends = 0;
    send_result = ESP_PEER_ERR_NONE;
}
static webrtc_transport_au_reservation_t queue_frame(void)
{
    webrtc_transport_au_reservation_t r = {0};
    assert(webrtc_transport_reserve_guest_access_unit(1920, 1080, 60, &r) == ESP_OK);
    au_slot_t *slot = &s_ctx.slots[r.slot];
    slot->size = 64;
    slot->state = AU_SLOT_READY;
    s_ctx.submitters--;
    return r;
}
/* Both slots hold frames; the next frame finds none and parks its task. */
static void fill_both_and_wait(void)
{
    queue_frame();
    queue_frame();
    webrtc_transport_au_reservation_t none = {0};
    assert(webrtc_transport_reserve_guest_access_unit(1920, 1080, 60, &none) == ESP_ERR_NO_MEM);
    assert(!none.data && s_ctx.slot_waiter == &s_task && notified == 0);
}
int main(void)
{
    /* A sent frame frees its slot and wakes the waiting task once. */
    reset();
    fill_both_and_wait();
    send_next_access_unit(&s_ctx, false);
    assert(sends == 1 && s_ctx.stats.access_units_sent == 1);
    assert(s_ctx.slots[0].state == AU_SLOT_FREE && s_ctx.slots[0].size == 0);
    assert(notified == 1 && !s_ctx.slot_waiter);
    webrtc_transport_au_reservation_t next = {0};
    assert(webrtc_transport_reserve_guest_access_unit(1920, 1080, 60, &next) == ESP_OK);
    assert(next.slot == 0);
    webrtc_transport_cancel_access_unit(&next);
    /* The next send has no waiter to wake. */
    send_next_access_unit(&s_ctx, false);
    assert(sends == 2 && s_ctx.slots[1].state == AU_SLOT_FREE && notified == 1);

    /* A send that fails frees the slot and wakes the task too. */
    reset();
    fill_both_and_wait();
    send_result = ESP_PEER_ERR_WOULD_BLOCK;
    send_next_access_unit(&s_ctx, false);
    assert(sends == 1 && s_ctx.stats.access_units_would_block == 1);
    assert(s_ctx.slots[0].state == AU_SLOT_FREE && notified == 1 && !s_ctx.slot_waiter);

    /* So does a frame dropped while the peer waits for a sync IDR. */
    reset();
    fill_both_and_wait();
    s_ctx.waiting_for_sync = true;
    send_next_access_unit(&s_ctx, false);
    assert(sends == 0 && s_ctx.stats.access_units_waiting_for_sync == 1);
    assert(s_ctx.slots[0].state == AU_SLOT_FREE && notified == 1 && !s_ctx.slot_waiter);

    assert(!lock_depth);
    puts("slot wake: a sent, failed or dropped frame wakes the task waiting for a slot: OK");
}
'''
with tempfile.TemporaryDirectory(prefix='webrtc-slot-wake-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(preamble + functions + checks)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)

#!/usr/bin/env python3
"""Run the real mode teardown and peer release waits with a simulated peer task."""
from pathlib import Path
import subprocess
import tempfile
source = (Path(__file__).resolve().parents[1] / 'webrtc_transport.c').read_text()
def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_ERR_NOT_FOUND 2
#define CONFIG_WEBRTC_TRANSPORT_AU_SLOTS 2
static const char *TAG = "test";
static void log_stub(const char *tag, const char *format, ...) {(void)tag; (void)format;}
#define ESP_LOGI log_stub
#define ESP_LOGW log_stub
#define ESP_LOGE log_stub
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdMS_TO_TICKS(x) (x)
typedef int esp_err_t;
typedef enum {AU_SLOT_FREE, AU_SLOT_FILLING, AU_SLOT_READY, AU_SLOT_SENDING} au_slot_state_t;
typedef struct { au_slot_state_t state; } au_slot_t;
typedef struct {
 int lock;
 bool started, mode_valid, rebuild_requested, closing_peer, rpc_data_channel_open;
 bool rpc_terminal_video_event_pending;
 uint32_t mode_teardown_waiters, outbound_count, active_peer_generation, submitters;
 uint32_t generation, active_peer_session, rpc_terminal_video_session_id;
 uint32_t rpc_terminal_video_generation;
 uint16_t rpc_data_channel_stream_id, rpc_terminal_video_stream_id;
 void *peer;
 au_slot_t slots[2];
 struct { uint64_t video_peer_releases, rpc_event_notifications_queued,
          rpc_event_notifications_coalesced; } stats;
} transport_ctx_t;
static transport_ctx_t s_ctx;
static int64_t now;
static bool stall;
static void notify_peer_task(transport_ctx_t *ctx) {(void)ctx;}
static int64_t esp_timer_get_time(void) {return now;}
static uint32_t next_generation_locked(transport_ctx_t *ctx) {return ++ctx->generation;}
static void schedule_cloud_invalidation_locked(transport_ctx_t *ctx, uint32_t a, uint32_t b)
{(void)ctx; (void)a; (void)b;}
static void webrtc_transport_clear_mode(void) {
 assert(s_ctx.mode_teardown_waiters > 0);
 s_ctx.mode_valid = false;
}
'''
# The simulated peer task finishes one step per tick: a send in progress,
# then a requested close (peer pointer first, closing_peer after).
delay = r'''
static void vTaskDelay(int ticks) {
 now += ticks * 1000;
 assert(s_ctx.mode_teardown_waiters > 0); /* No peer opens meanwhile. */
 if (stall) return;
 s_ctx.submitters = 0;
 s_ctx.slots[0].state = AU_SLOT_FREE;
 if (s_ctx.rebuild_requested) {
  s_ctx.rebuild_requested = false; s_ctx.peer = NULL; s_ctx.closing_peer = true;
  s_ctx.active_peer_generation = 0;
 } else if (s_ctx.closing_peer) {
  s_ctx.closing_peer = false;
 }
}
'''
test = r'''
int main(void) {
 int peer;
 s_ctx.started = true; s_ctx.mode_valid = true;
 s_ctx.peer = &peer; s_ctx.active_peer_generation = 1; s_ctx.generation = 1;
 s_ctx.active_peer_session = 3; s_ctx.rpc_data_channel_open = true;
 /* Video teardown waits for the encoder and the sender, and keeps the peer. */
 s_ctx.submitters = 1; s_ctx.slots[0].state = AU_SLOT_SENDING;
 assert(webrtc_transport_clear_mode_and_wait(100) == ESP_OK);
 assert(now > 0 && !s_ctx.mode_valid && s_ctx.mode_teardown_waiters == 0);
 assert(s_ctx.peer == &peer && s_ctx.generation == 1 && !s_ctx.rebuild_requested);
 stall = true; s_ctx.slots[0].state = AU_SLOT_SENDING;
 assert(webrtc_transport_clear_mode_and_wait(10) == ESP_ERR_TIMEOUT);
 assert(s_ctx.mode_teardown_waiters == 0 && s_ctx.peer == &peer);
 stall = false; s_ctx.slots[0].state = AU_SLOT_FREE;
 /* The memory fallback closes the peer and waits past closing_peer. */
 assert(webrtc_transport_release_peer_and_wait(100) == ESP_OK);
 assert(!s_ctx.peer && !s_ctx.closing_peer && s_ctx.generation == 2);
 assert(s_ctx.stats.video_peer_releases == 1 && s_ctx.mode_teardown_waiters == 0);
 assert(s_ctx.rpc_terminal_video_event_pending && s_ctx.rpc_terminal_video_generation == 1);
 /* Without a peer there is nothing to release. */
 assert(webrtc_transport_release_peer_and_wait(100) == ESP_ERR_NOT_FOUND);
 assert(s_ctx.stats.video_peer_releases == 1 && s_ctx.mode_teardown_waiters == 0);
 puts("mode teardown keeps the peer / memory release: PASS");
}
'''
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory)
    (p/'test.c').write_text(stub + delay + '\n'.join(function(s) for s in (
        'static bool video_quiet_locked(',
        'static bool peer_released_locked(',
        'static esp_err_t wait_for_teardown(',
        'esp_err_t webrtc_transport_clear_mode_and_wait(',
        'esp_err_t webrtc_transport_release_peer_and_wait(',
    )) + test)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)

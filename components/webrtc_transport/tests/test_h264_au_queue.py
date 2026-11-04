#!/usr/bin/env python3
"""Execute the firmware's AU queue and PLI recovery functions with host stubs."""
from pathlib import Path
import subprocess
import tempfile
import re

component = Path(__file__).resolve().parents[1]
source = (component / "webrtc_transport.c").read_text()
wrappers = (component / "network_timing_wrappers.c").read_text()


def function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


peer_header = (component.parents[1] / "third_party/esp_peer/include/esp_peer.h").read_text()
peer_states = re.search(r"typedef enum \{.*?\} esp_peer_state_t;", peer_header, re.S)[0]

stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rtcp_feedback.h"
#define CONFIG_WEBRTC_TRANSPORT_AU_SLOTS 2
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define ESP_LOGI(...) ((void)0)
#define ESP_PEER_ERR_NONE 0
#define CONFIG_WEBRTC_TRANSPORT_RECONNECT_DELAY_MS 1000
typedef enum {WEBRTC_TRANSPORT_PEER_GATHERING, WEBRTC_TRANSPORT_PEER_PAIRING,
 WEBRTC_TRANSPORT_PEER_CONNECTING, WEBRTC_TRANSPORT_PEER_CONNECTED,
 WEBRTC_TRANSPORT_PEER_FAILED, WEBRTC_TRANSPORT_PEER_WAITING
} webrtc_transport_peer_state_t;
typedef enum {AU_SLOT_FREE, AU_SLOT_FILLING, AU_SLOT_READY, AU_SLOT_SENDING} state_t;
typedef struct {
    state_t state;
    uint64_t order;
    uint32_t generation;
    bool keyframe, has_idr, has_sps, has_pps;
} au_slot_t;
typedef struct { int32_t lost; uint32_t sequence; unsigned reports; } webrtc_bitrate_controller_t;
static void webrtc_bitrate_loss(webrtc_bitrate_controller_t *c, int32_t lost, uint32_t sequence)
{c->lost = lost; c->sequence = sequence; c->reports++;}
typedef struct {
    int lock;
    webrtc_bitrate_controller_t bitrate_controller;
    au_slot_t slots[2];
    uint32_t active_peer_generation, generation, last_sent_au_generation;
    uint64_t last_sent_au_order;
    bool waiting_for_sync, started, session_active, peer_connected;
    bool mode_valid, guest_mode;
    bool closing_peer, rebuild_requested, video_paused;
    void *peer, *task;
    uint32_t video_ssrc, video_ssrc_generation, last_pli_generation;
    uint32_t sync_idr_pending_generation, fps;
    int64_t last_pli_request_us;
    uint32_t sync_idr_last_requested_generation;
    bool peer_tls_release_pending;
    int64_t reconnect_not_before_us;
    struct {bool waiting_for_sync_frame;
            bool peer_connected;
            int peer_state;
            uint32_t peer_connects;
            uint32_t sync_idr_pending_generation, sync_idr_requests;
            uint32_t video_rtcp_loss_reports, video_rtcp_highest_sequence;
            int32_t video_rtcp_cumulative_lost;} stats;
} transport_ctx_t;
static transport_ctx_t s_ctx;
static int64_t test_now;
static int task_identity;
static void *xTaskGetCurrentTaskHandle(void) {return &task_identity;}
static int64_t esp_timer_get_time(void) {return test_now;}
static unsigned invalidations;
static void next_generation_locked(transport_ctx_t *ctx) {ctx->generation++;}
static void schedule_cloud_invalidation_locked(transport_ctx_t *ctx, uint32_t old, uint32_t next)
{(void)ctx; (void)old; (void)next;}
static void hid_input_invalidate_generation(uint32_t gen, const char *why)
{(void)why; if (gen) invalidations++;}
typedef void *srtp_t;
typedef int srtp_err_status_t;
#define srtp_err_status_ok 0
static int decrypt_result;
srtp_err_status_t __real_srtp_unprotect_rtcp(
    srtp_t ctx, const uint8_t *in, size_t len, uint8_t *out, size_t *out_len)
{
    (void)ctx;
    if (decrypt_result) return decrypt_result;
    memcpy(out, in, len); *out_len = len; return 0;
}
'''
functions = "\n".join(function(source, signature) for signature in (
    "static bool slot_needs_reference_sync(",
    "static void request_video_recovery_locked(",
    "static void handle_video_pli(",
    "void webrtc_transport_on_rtcp(",
    "static int reserve_slot_locked(",
    "static bool service_video_locked(",
    "static int take_next_ready_slot(",
    "static webrtc_transport_peer_state_t map_peer_state(",
    "static int peer_on_state(",
))
functions += function(wrappers, "srtp_err_status_t __wrap_srtp_unprotect_rtcp(")
checks = r'''
int main(void)
{
    transport_ctx_t q = {0};
    q.slots[0] = (au_slot_t){.state=AU_SLOT_READY, .order=2};
    q.slots[1] = (au_slot_t){.state=AU_SLOT_READY, .order=1};
    assert(reserve_slot_locked(&q) == -1); /* Never evict a queued reference. */
    assert(q.slots[0].state == AU_SLOT_READY && q.slots[1].state == AU_SLOT_READY);
    assert(take_next_ready_slot(&q, false) == 1);
    assert(q.slots[0].state == AU_SLOT_READY); /* Preserve the successor. */
    q.slots[1].state = AU_SLOT_FREE;
    assert(take_next_ready_slot(&q, false) == 0);
    q.slots[0] = (au_slot_t){.state=AU_SLOT_FILLING, .order=3};
    q.slots[1] = (au_slot_t){.state=AU_SLOT_READY, .order=4};
    assert(take_next_ready_slot(&q, false) == -1); /* Do not overtake a copying AU. */
    q.slots[0].state = AU_SLOT_READY;
    assert(take_next_ready_slot(&q, false) == 0);

    /* Between the steps of a pass a keyframe stays, and the frame behind
     * it keeps its place; so does every service video frame, while the mode
     * that made it service video is valid. */
    q.slots[0] = (au_slot_t){.state=AU_SLOT_READY, .order=5, .keyframe=true};
    q.slots[1] = (au_slot_t){.state=AU_SLOT_READY, .order=6};
    assert(take_next_ready_slot(&q, true) == -1);
    assert(q.slots[0].state == AU_SLOT_READY && q.slots[1].state == AU_SLOT_READY);
    assert(take_next_ready_slot(&q, false) == 0);
    q.slots[0].state = AU_SLOT_FREE;
    assert(take_next_ready_slot(&q, true) == 1);
    q.slots[1] = (au_slot_t){.state=AU_SLOT_READY, .order=7};
    q.mode_valid = q.guest_mode = true;
    assert(take_next_ready_slot(&q, true) == -1);
    assert(take_next_ready_slot(&q, false) == 1);
    q.slots[1].state = AU_SLOT_READY;
    q.mode_valid = false;
    assert(take_next_ready_slot(&q, true) == 1);
    q.slots[1].state = AU_SLOT_FREE;
    q.guest_mode = false;

    q.active_peer_generation = q.last_sent_au_generation = 7;
    q.last_sent_au_order = 10;
    au_slot_t next = {.generation=7, .order=11};
    assert(!slot_needs_reference_sync(&q, &next));
    next.order = 12; /* An encoded frame was rejected or failed to send. */
    assert(slot_needs_reference_sync(&q, &next));
    next.keyframe = next.has_idr = next.has_sps = next.has_pps = true;
    assert(!slot_needs_reference_sync(&q, &next));
    next.has_pps = false;
    assert(slot_needs_reference_sync(&q, &next));
    request_video_recovery_locked(&q);
    assert(q.waiting_for_sync && q.sync_idr_pending_generation == 7);
    assert(q.stats.sync_idr_requests == 1);
    request_video_recovery_locked(&q);
    assert(q.stats.sync_idr_requests == 1); /* Coalesce existing recovery. */
    assert(!slot_needs_reference_sync(&q, &next));

    s_ctx = (transport_ctx_t){.started=true, .session_active=true,
        .peer_connected=true, .peer=&q, .task=&task_identity,
        .active_peer_generation=9, .generation=9,
        .video_ssrc=1, .video_ssrc_generation=9, .fps=60};
    const uint8_t pli[] = {0x81, 206, 0, 2, 0, 0, 0, 7, 0, 0, 0, 1};
    uint8_t clear[12]; size_t length=sizeof(clear);
    test_now = 1000000;
    decrypt_result = 7;
    assert(__wrap_srtp_unprotect_rtcp(NULL, pli, sizeof(pli), clear, &length) == 7);
    assert(s_ctx.stats.sync_idr_requests == 0); /* Unauthenticated: no request. */
    decrypt_result = 0;
    assert(__wrap_srtp_unprotect_rtcp(NULL, pli, sizeof(pli), clear, &length) == 0);
    assert(s_ctx.waiting_for_sync && s_ctx.sync_idr_pending_generation == 9);
    assert(s_ctx.stats.sync_idr_requests == 1);
    test_now += 1000000;
    webrtc_transport_on_rtcp(pli, sizeof(pli));
    assert(s_ctx.stats.sync_idr_requests == 1); /* Already waiting. */
    s_ctx.waiting_for_sync = false;
    test_now = 1100000;
    webrtc_transport_on_rtcp(pli, sizeof(pli));
    assert(s_ctx.stats.sync_idr_requests == 1); /* Within the same GOP. */
    test_now = 1500000;
    webrtc_transport_on_rtcp(pli, sizeof(pli));
    assert(s_ctx.stats.sync_idr_requests == 2);
    s_ctx.waiting_for_sync = false;
    test_now = 3000000;
    s_ctx.video_ssrc = 2;
    webrtc_transport_on_rtcp(pli, sizeof(pli));
    assert(s_ctx.stats.sync_idr_requests == 2); /* Other media. */
    s_ctx.video_ssrc = 1;
    s_ctx.generation++;
    webrtc_transport_on_rtcp(pli, sizeof(pli));
    assert(s_ctx.stats.sync_idr_requests == 2); /* Stale peer. */
    s_ctx.generation--;
    /* A receiver report block for the video source reaches the controller;
     * one for another source does not. */
    uint8_t rr[8 + 24] = {0x81, 201, 0, 7, 0, 0, 0, 9};
    rr[8 + 3] = 1; rr[8 + 7] = 5; rr[8 + 11] = 200;
    webrtc_transport_on_rtcp(rr, sizeof(rr));
    assert(s_ctx.bitrate_controller.reports == 1 && s_ctx.bitrate_controller.lost == 5 &&
           s_ctx.bitrate_controller.sequence == 200 && s_ctx.stats.video_rtcp_loss_reports == 1);
    rr[8 + 3] = 2;
    webrtc_transport_on_rtcp(rr, sizeof(rr));
    assert(s_ctx.bitrate_controller.reports == 1);
    rr[8 + 3] = 1;
    s_ctx.task = NULL;
    webrtc_transport_on_rtcp(pli, sizeof(pli));
    webrtc_transport_on_rtcp(rr, sizeof(rr));
    assert(s_ctx.stats.sync_idr_requests == 2); /* Wrong library owner. */
    assert(s_ctx.bitrate_controller.reports == 1);
    /* Vendor media feedback must preserve the active connection. */
    s_ctx = (transport_ctx_t){.started=true, .session_active=true,
        .peer=&q, .task=&task_identity, .active_peer_generation=12,
        .generation=12, .video_ssrc=1, .video_ssrc_generation=12, .fps=30};
    peer_on_state(ESP_PEER_STATE_CONNECTED, &s_ctx);
    assert(!s_ctx.peer_connected); /* Wait for DTLS/SCTP readiness. */
    peer_on_state(ESP_PEER_STATE_DATA_CHANNEL_CONNECTED, &s_ctx);
    assert(s_ctx.peer_connected && s_ctx.stats.peer_connects == 1);
    s_ctx.waiting_for_sync = false; /* Initial IDR completed. */
    s_ctx.sync_idr_pending_generation = 0;
    unsigned requests = s_ctx.stats.sync_idr_requests;
    test_now += 1000000;
    peer_on_state(ESP_PEER_STATE_VIDEO_PLI_RECEIVED, &s_ctx);
    assert(s_ctx.peer_connected && s_ctx.stats.peer_connected);
    assert(s_ctx.stats.peer_state == WEBRTC_TRANSPORT_PEER_CONNECTED);
    assert(!s_ctx.rebuild_requested && s_ctx.generation == 12 && !invalidations);
    assert(s_ctx.stats.peer_connects == 1);
    assert(s_ctx.waiting_for_sync && s_ctx.sync_idr_pending_generation == 12);
    assert(s_ctx.stats.sync_idr_requests == requests + 1);
    /* The decrypt observer and vendor callback may report the same PLI. */
    webrtc_transport_on_rtcp(pli, sizeof(pli));
    peer_on_state(ESP_PEER_STATE_VIDEO_PLI_RECEIVED, &s_ctx);
    assert(s_ctx.stats.sync_idr_requests == requests + 1);
    s_ctx.waiting_for_sync = false;
    test_now += 499999;
    peer_on_state(ESP_PEER_STATE_VIDEO_PLI_RECEIVED, &s_ctx);
    assert(s_ctx.stats.sync_idr_requests == requests + 1);
    test_now++;
    peer_on_state(ESP_PEER_STATE_VIDEO_PLI_RECEIVED, &s_ctx);
    assert(s_ctx.stats.sync_idr_requests == requests + 2);
    s_ctx.waiting_for_sync = false;
    test_now += 1000000;
    s_ctx.video_paused = true;
    peer_on_state(ESP_PEER_STATE_VIDEO_PLI_RECEIVED, &s_ctx);
    assert(s_ctx.peer_connected && s_ctx.stats.sync_idr_requests == requests + 2);
    s_ctx.video_paused = false;
    s_ctx.generation++;
    peer_on_state(ESP_PEER_STATE_VIDEO_PLI_RECEIVED, &s_ctx);
    assert(s_ctx.peer_connected && s_ctx.stats.sync_idr_requests == requests + 2);
    s_ctx.generation--;
    peer_on_state(ESP_PEER_STATE_DISCONNECTED, &s_ctx);
    assert(!s_ctx.peer_connected && s_ctx.rebuild_requested && invalidations == 1);
    peer_on_state(ESP_PEER_STATE_VIDEO_PLI_RECEIVED, &s_ctx);
    assert(!s_ctx.peer_connected && s_ctx.stats.sync_idr_requests == requests + 2);
    puts("H264 FIFO, reference gap, authenticated/vendor PLI and rate limit: OK");
}
'''
with tempfile.TemporaryDirectory(prefix="mini-h264-queue-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(stub + peer_states + functions + checks)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(component), str(path / "test.c"),
                    str(component / "rtcp_feedback.c"),
                    "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)

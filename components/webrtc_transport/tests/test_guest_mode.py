#!/usr/bin/env python3
"""OS Services guest streams: bounds, a new video epoch per stream, the sync
IDR it asks for, the bitrate feedback a restart keeps, and the recovery a
lost frame starts, also while a sync IDR is pending (production code)."""
from pathlib import Path
import re
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
source = (component / 'webrtc_transport.c').read_text()
internal = (component / 'webrtc_transport_ctx_internal.h').read_text()

def function(signature):
    start = source.index(signature)
    pos = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]

functions = '\n'.join(function(s) for s in (
    'static bool supported_mode(',
    'static bool fail_sync_idr_locked(',
    'static void flush_ready_slots_locked(',
    'static bool slot_needs_reference_sync(',
    'static void request_video_recovery_locked(',
    'static void next_video_epoch_locked(',
    'static void reset_bitrate_feedback_locked(',
    'static esp_err_t apply_mode(',
    'esp_err_t webrtc_transport_set_mode(',
    'esp_err_t webrtc_transport_set_guest_mode(',
    'void webrtc_transport_note_video_gap(',
))
stat_fields = sorted(set(re.findall(r'ctx->stats\.(\w+)', functions)) - {'stage_age'})
slot = re.search(r'typedef struct \{(?:(?!typedef struct).)*?\} au_slot_t;', internal, re.S)[0]
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_WEBRTC_TRANSPORT_AU_SLOTS 2
#define ESP_OK 0
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_LOGI(...) ((void)0)
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
typedef int esp_err_t;
''' + re.search(r'typedef enum \{[^}]+\} au_slot_state_t;', internal)[0] + slot + r'''
typedef struct {
    int lock;
    au_slot_t slots[2];
    bool started, mode_valid, guest_mode, waiting_for_sync, peer_connected;
    void *peer;
    uint16_t width, height, fps;
    uint32_t video_epoch, active_peer_generation, last_sent_au_generation;
    uint64_t next_au_order, last_sent_au_order;
    uint32_t sync_idr_pending_generation, sync_idr_inflight_generation,
             sync_idr_committed_generation;
    struct { int last; } bitrate_controller;
    struct { struct { uint32_t epoch; } stage_age;
        /* STATS */
    } stats;
} transport_ctx_t;
static transport_ctx_t s_ctx;
static unsigned notified, events;
static void notify_peer_task(transport_ctx_t *ctx) { (void)ctx; ++notified; }
static void queue_video_input_event_locked(transport_ctx_t *ctx) { (void)ctx; ++events; }
static void reset_stage_age_locked(transport_ctx_t *ctx) { ctx->stats.stage_age.epoch++; }
'''.replace('/* STATS */', '\n'.join('uint64_t ' + f + ';' for f in stat_fields))
checks = r'''
static int peer;
int main(void)
{
    s_ctx = (transport_ctx_t){.started = true, .peer_connected = true, .peer = &peer,
                              .active_peer_generation = 9};
    /* Up to 3840x2160 at 60 fps, any size the hardware path does not take.
     * The session asks the sender for 30 fps above 720p, but a sender that
     * switches from 720p60 may report 60 until it reads that. */
    assert(webrtc_transport_set_guest_mode(3841, 2160, 30) == ESP_ERR_NOT_SUPPORTED);
    assert(webrtc_transport_set_guest_mode(3840, 2161, 30) == ESP_ERR_NOT_SUPPORTED);
    assert(webrtc_transport_set_guest_mode(3840, 2160, 61) == ESP_ERR_NOT_SUPPORTED);
    assert(webrtc_transport_set_guest_mode(1920, 1080, 61) == ESP_ERR_NOT_SUPPORTED);
    assert(webrtc_transport_set_guest_mode(0, 1080, 30) == ESP_ERR_NOT_SUPPORTED);
    assert(webrtc_transport_set_guest_mode(1920, 0, 30) == ESP_ERR_NOT_SUPPORTED);
    assert(webrtc_transport_set_guest_mode(1920, 1080, 0) == ESP_ERR_NOT_SUPPORTED);
    assert(webrtc_transport_set_mode(3840, 2160, 30) == ESP_ERR_NOT_SUPPORTED);
    assert(webrtc_transport_set_guest_mode(3840, 2160, 60) == ESP_OK &&
           s_ctx.width == 3840 && s_ctx.height == 2160 && s_ctx.video_epoch == 1);
    assert(s_ctx.guest_mode);
    s_ctx.mode_valid = false;
    s_ctx.video_epoch = 0;
    s_ctx.sync_idr_pending_generation = 0;
    notified = 0;
    assert(webrtc_transport_set_mode(2560, 1440, 30) == ESP_ERR_NOT_SUPPORTED);
    assert(s_ctx.video_epoch == 0 && !s_ctx.mode_valid);
    assert(webrtc_transport_set_guest_mode(2560, 1440, 60) == ESP_OK);
    assert(s_ctx.mode_valid && s_ctx.width == 2560 && s_ctx.fps == 60);
    assert(s_ctx.video_epoch == 1 && s_ctx.sync_idr_pending_generation == 9);
    /* Each stream is a new epoch, in the same mode too: what is queued from
     * the last one is flushed and a sync IDR is asked for again. */
    s_ctx.sync_idr_pending_generation = 0;
    s_ctx.slots[0].state = AU_SLOT_READY;
    s_ctx.slots[1].state = AU_SLOT_SENDING;
    assert(webrtc_transport_set_guest_mode(2560, 1440, 60) == ESP_OK);
    assert(s_ctx.video_epoch == 2 && s_ctx.waiting_for_sync);
    assert(s_ctx.slots[0].state == AU_SLOT_FREE && s_ctx.slots[1].state == AU_SLOT_SENDING);
    assert(s_ctx.sync_idr_pending_generation == 9 && notified == 2);
    /* A stream that restarts at the same size (a new encoder for a new
     * target) keeps the bitrate feedback; a new size or the hardware path
     * starts it again. */
    s_ctx.bitrate_controller.last = 7;
    assert(webrtc_transport_set_guest_mode(2560, 1440, 30) == ESP_OK);
    assert(s_ctx.video_epoch == 3 && s_ctx.bitrate_controller.last == 7);
    assert(webrtc_transport_set_guest_mode(1280, 720, 30) == ESP_OK);
    assert(s_ctx.video_epoch == 4 && s_ctx.bitrate_controller.last == 0);
    s_ctx.bitrate_controller.last = 7;
    /* The hardware path keeps its mode across calls, and the mode is no
     * longer service video's, also when service video had the same size. */
    assert(s_ctx.guest_mode);
    assert(webrtc_transport_set_mode(1920, 1080, 30) == ESP_OK && s_ctx.video_epoch == 5);
    assert(s_ctx.bitrate_controller.last == 0 && !s_ctx.guest_mode);
    assert(webrtc_transport_set_mode(1920, 1080, 30) == ESP_OK && s_ctx.video_epoch == 5);
    assert(webrtc_transport_set_guest_mode(1920, 1080, 30) == ESP_OK && s_ctx.guest_mode);
    assert(webrtc_transport_set_mode(1920, 1080, 30) == ESP_OK && !s_ctx.guest_mode);
    /* A lost frame: the viewer waits for a sync IDR, asked for once. */
    s_ctx.waiting_for_sync = false;
    s_ctx.sync_idr_pending_generation = 0;
    const uint64_t requests = s_ctx.stats.sync_idr_requests;
    webrtc_transport_note_video_gap();
    assert(s_ctx.waiting_for_sync && s_ctx.sync_idr_pending_generation == 9);
    webrtc_transport_note_video_gap();
    assert(s_ctx.stats.sync_idr_requests == requests + 1);
    /* A frame lost while that sync IDR is still queued: the IDR clears the
     * wait, but the P frame after it must not reach the browser, which never
     * got the frame it references. */
    const au_slot_t sync = {.generation = 9, .order = ++s_ctx.next_au_order,
                            .keyframe = true, .has_idr = true, .has_sps = true,
                            .has_pps = true};
    webrtc_transport_note_video_gap();
    s_ctx.last_sent_au_generation = 9; /* The IDR is sent. */
    s_ctx.last_sent_au_order = sync.order;
    s_ctx.waiting_for_sync = false;
    const au_slot_t next = {.generation = 9, .order = ++s_ctx.next_au_order};
    assert(slot_needs_reference_sync(&s_ctx, &next));
    /* Without a viewer there is nothing to recover. */
    s_ctx.waiting_for_sync = false;
    s_ctx.sync_idr_pending_generation = 0;
    s_ctx.peer_connected = false;
    webrtc_transport_note_video_gap();
    assert(!s_ctx.waiting_for_sync && s_ctx.sync_idr_pending_generation == 0);
    puts("guest streams: bounds, epoch per stream, sync IDR, kept feedback, gap recovery: OK");
}
'''
with tempfile.TemporaryDirectory(prefix='webrtc-guest-mode-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(preamble + functions + checks)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)

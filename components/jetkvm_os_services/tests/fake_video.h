/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* The service video hooks for the session tests: a display that presents
 * when the test says so, a single access-unit slot the test can keep busy,
 * and a keyframe request source that requeues what it gets back, as the
 * WebRTC transport does. One instance, g_fake_video. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os_services_session.h"

/* What video_stream() reports unless a test sets capacity. */
#define FAKE_VIDEO_SLOT_BYTES (3U * 1024U * 1024U)
#define FAKE_VIDEO_FRAMES 256U
#define FAKE_VIDEO_EVENTS 64U

typedef struct {
    osvc_video_frame_t meta;
    /* FNV-1a of the committed bytes. */
    uint32_t hash;
} fake_video_frame_t;

typedef struct {
    uint32_t generation;
    bool delivered;
} fake_keyframe_done_t;

typedef struct {
    /* display_present() and display_restore(). */
    unsigned presents;
    uint32_t present_width;
    uint32_t present_height;
    uint32_t serials[FAKE_VIDEO_EVENTS];
    bool present_fails;
    unsigned restores;
    /* video_stream() and video_stopped(). */
    unsigned streams;
    uint32_t stream_width;
    uint32_t stream_height;
    uint32_t stream_fps;
    bool stream_refused;
    unsigned stopped;
    /* The slot; busy keeps it taken, no_viewer refuses frames. capacity is
     * what video_stream() reports for the stream it starts. */
    uint8_t slot[FAKE_VIDEO_SLOT_BYTES];
    size_t capacity;
    bool reserved;
    bool busy;
    bool no_viewer;
    unsigned reserves;
    unsigned cancels;
    size_t cancel_written;
    fake_video_frame_t frames[FAKE_VIDEO_FRAMES];
    size_t frame_count;
    bool refuse_commit;
    unsigned gaps;
    /* The request keyframe_wanted() hands out; given back undelivered, it
     * is pending again. */
    uint32_t keyframe_pending;
    fake_keyframe_done_t keyframe_done[FAKE_VIDEO_EVENTS];
    size_t keyframe_done_count;
    /* video_network(): an interval ends at each whole second, with estimate_kbps
     * (0: the ceiling), reported as a cut unless stale, and sent_kbps;
     * manual is a manual video quality. */
    bool manual;
    bool stale;
    uint32_t estimate_kbps;
    uint32_t sent_kbps;
    int64_t interval_ms;
    uint32_t ceiling_kbps;
    /* now_us() is the platform's now_ms() * 1000 plus this. */
    int64_t us_offset;
    int64_t (*now_ms)(void *ctx);
    void *clock_ctx;
} fake_video_t;

extern fake_video_t g_fake_video;

/* Clears g_fake_video and adds its hooks to platform (after the others). */
void fake_video_attach(osvc_platform_t *platform);
uint32_t fake_video_hash(const uint8_t *data, size_t length);

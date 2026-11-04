/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "fake_video.h"

#include <assert.h>
#include <string.h>

fake_video_t g_fake_video;

uint32_t fake_video_hash(const uint8_t *data, size_t length)
{
    uint32_t hash = 2166136261U;
    for (size_t i = 0U; i < length; ++i) {
        hash = (hash ^ data[i]) * 16777619U;
    }
    return hash;
}

static int64_t video_now_us(void *ctx)
{
    (void)ctx;
    fake_video_t *v = &g_fake_video;
    return v->now_ms(v->clock_ctx) * 1000 + v->us_offset;
}

static bool video_present(void *ctx, uint32_t width, uint32_t height,
                          uint32_t serial)
{
    (void)ctx;
    fake_video_t *v = &g_fake_video;
    assert(serial != 0U && v->presents < FAKE_VIDEO_EVENTS);
    v->serials[v->presents++] = serial;
    v->present_width = width;
    v->present_height = height;
    return !v->present_fails;
}

static void video_restore(void *ctx)
{
    (void)ctx;
    g_fake_video.restores++;
}

static size_t video_stream(void *ctx, uint32_t width, uint32_t height,
                           uint32_t fps)
{
    (void)ctx;
    fake_video_t *v = &g_fake_video;
    assert(!v->reserved);
    v->streams++;
    v->stream_width = width;
    v->stream_height = height;
    v->stream_fps = fps;
    return v->stream_refused ? 0U : v->capacity;
}

static void video_stopped(void *ctx)
{
    (void)ctx;
    assert(!g_fake_video.reserved);
    g_fake_video.stopped++;
}

static uint8_t *frame_reserve(void *ctx, size_t length, bool *busy)
{
    (void)ctx;
    fake_video_t *v = &g_fake_video;
    assert(!v->reserved && length <= v->capacity);
    v->reserves++;
    *busy = v->busy;
    if (v->busy || v->no_viewer) {
        return NULL;
    }
    v->reserved = true;
    return v->slot;
}

static bool frame_commit(void *ctx, const osvc_video_frame_t *frame)
{
    (void)ctx;
    fake_video_t *v = &g_fake_video;
    assert(v->reserved && frame->length <= v->capacity);
    assert(v->frame_count < FAKE_VIDEO_FRAMES);
    v->reserved = false;
    v->frames[v->frame_count++] = (fake_video_frame_t){
        .meta = *frame,
        .hash = fake_video_hash(v->slot, frame->length),
    };
    return !v->refuse_commit;
}

static void frame_cancel(void *ctx, size_t written)
{
    (void)ctx;
    fake_video_t *v = &g_fake_video;
    assert(v->reserved && written <= v->capacity);
    v->reserved = false;
    v->cancels++;
    v->cancel_written = written;
}

static void video_gap(void *ctx)
{
    (void)ctx;
    g_fake_video.gaps++;
}

static uint32_t keyframe_wanted(void *ctx)
{
    (void)ctx;
    const uint32_t generation = g_fake_video.keyframe_pending;
    g_fake_video.keyframe_pending = 0U;
    return generation;
}

static void keyframe_done(void *ctx, uint32_t generation, bool delivered)
{
    (void)ctx;
    fake_video_t *v = &g_fake_video;
    assert(generation != 0U && v->keyframe_done_count < FAKE_VIDEO_EVENTS);
    v->keyframe_done[v->keyframe_done_count++] =
        (fake_keyframe_done_t){generation, delivered};
    if (!delivered) {
        v->keyframe_pending = generation;
    }
}

static void video_network(void *ctx, uint32_t ceiling_kbps,
                          osvc_video_network_t *network)
{
    (void)ctx;
    fake_video_t *v = &g_fake_video;
    const int64_t now = v->now_ms(v->clock_ctx);
    v->ceiling_kbps = ceiling_kbps;
    *network = (osvc_video_network_t){.adaptive = !v->manual};
    if (now / 1000 != v->interval_ms / 1000) {
        v->interval_ms = now;
        network->interval = true;
        network->estimate_kbps =
            v->estimate_kbps != 0U ? v->estimate_kbps : ceiling_kbps;
        network->cut = !v->stale;
        network->sent_kbps = v->sent_kbps;
    }
}

void fake_video_attach(osvc_platform_t *platform)
{
    memset(&g_fake_video, 0, sizeof(g_fake_video));
    g_fake_video.capacity = FAKE_VIDEO_SLOT_BYTES;
    g_fake_video.now_ms = platform->now_ms;
    g_fake_video.clock_ctx = platform->ctx;
    g_fake_video.interval_ms = platform->now_ms(platform->ctx);
    platform->now_us = video_now_us;
    platform->display_present = video_present;
    platform->display_restore = video_restore;
    platform->video_stream = video_stream;
    platform->video_stopped = video_stopped;
    platform->frame_reserve = frame_reserve;
    platform->frame_commit = frame_commit;
    platform->frame_cancel = frame_cancel;
    platform->video_gap = video_gap;
    platform->keyframe_wanted = keyframe_wanted;
    platform->keyframe_done = keyframe_done;
    platform->video_network = video_network;
}

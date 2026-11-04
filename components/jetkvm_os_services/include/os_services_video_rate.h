/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * Service video bitrate adaptation: the target_kbps and maximum_fps that
 * video.config carries, from an estimate of what the network carries. A
 * cut follows the estimate down at once, by at most half; a raise waits,
 * and goes up in steps, because each change can cost the sender a new
 * encoder and an IDR.
 * Below a floor the frame rate halves before the picture falls apart.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* After any change, no cut for this long: the sender applies the target,
 * and a reopened encoder's IDR crosses the link first. */
#define OSVC_VIDEO_RATE_SETTLE_MS 2000
/* Between a change and a raise; doubled after a raise that had to be cut
 * back within it, up to the maximum. */
#define OSVC_VIDEO_RATE_RAISE_MS 10000
#define OSVC_VIDEO_RATE_RAISE_MAX_MS 60000

typedef struct {
    /* The static target, and the target in force. */
    uint32_t ceiling_kbps;
    uint32_t kbps;
    /* The frame rate is halved. */
    bool low_fps;
    int64_t changed_ms;
    int64_t raise_wait_ms;
    /* The last change was a raise. */
    bool raised;
    uint32_t cuts;
    uint32_t raises;
} osvc_video_rate_t;

/* Starts at the ceiling. */
void osvc_video_rate_reset(osvc_video_rate_t *rate, uint32_t ceiling_kbps,
                           int64_t now_ms);

/* One evaluation with the network's estimate. True when the target or the
 * frame rate changed. */
bool osvc_video_rate_step(osvc_video_rate_t *rate, uint32_t estimate_kbps,
                          int64_t now_ms);

/* A new stream starts: no change for OSVC_VIDEO_RATE_SETTLE_MS, and the
 * raise wait starts again. */
void osvc_video_rate_hold(osvc_video_rate_t *rate, int64_t now_ms);

/* The frame-rate cap under fps_cap. */
uint32_t osvc_video_rate_fps(const osvc_video_rate_t *rate, uint32_t fps_cap);

#ifdef __cplusplus
}
#endif

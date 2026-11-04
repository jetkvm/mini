/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "os_services_video_rate.h"

/* Targets move in steps of this many kbps. */
#define STEP_KBPS 100U

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static uint32_t max_u32(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

void osvc_video_rate_reset(osvc_video_rate_t *rate, uint32_t ceiling_kbps,
                           int64_t now_ms)
{
    *rate = (osvc_video_rate_t){
        .ceiling_kbps = ceiling_kbps,
        .kbps = ceiling_kbps,
        .changed_ms = now_ms,
        .raise_wait_ms = OSVC_VIDEO_RATE_RAISE_MS,
    };
}

/* The target for an estimate: in steps, from an eighth of the ceiling up
 * to the ceiling. */
static uint32_t bounded(const osvc_video_rate_t *rate, uint32_t kbps)
{
    return max_u32(rate->ceiling_kbps / 8U,
                   min_u32(rate->ceiling_kbps, kbps - kbps % STEP_KBPS));
}

/* The next target: the estimate when it is clearly lower, but at least
 * half the target, so that one burst of loss (a large keyframe) does not
 * take the target to the floor; when it is clearly higher, or reaches the
 * ceiling, at most half again as much. */
static uint32_t next_kbps(const osvc_video_rate_t *rate, uint32_t estimate,
                          int64_t since)
{
    const uint32_t kbps = rate->kbps;
    if (estimate < kbps - kbps / 10U && since >= OSVC_VIDEO_RATE_SETTLE_MS) {
        const uint32_t half = (kbps / 2U + STEP_KBPS - 1U) / STEP_KBPS * STEP_KBPS;
        return bounded(rate, max_u32(estimate, half));
    }
    if ((estimate >= kbps + kbps / 5U || estimate >= rate->ceiling_kbps) &&
        since >= rate->raise_wait_ms) {
        return bounded(rate, min_u32(estimate, kbps + kbps / 2U));
    }
    return kbps;
}

bool osvc_video_rate_step(osvc_video_rate_t *rate, uint32_t estimate_kbps,
                          int64_t now_ms)
{
    const int64_t since = now_ms - rate->changed_ms;
    if (rate->raised && since >= OSVC_VIDEO_RATE_RAISE_MS) {
        /* The last raise held. */
        rate->raise_wait_ms = OSVC_VIDEO_RATE_RAISE_MS;
    }
    const uint32_t kbps = next_kbps(rate, estimate_kbps, since);
    if (kbps == rate->kbps) {
        return false;
    }
    const bool raise = kbps > rate->kbps;
    if (!raise && rate->raised && since < OSVC_VIDEO_RATE_RAISE_MS) {
        /* The last raise was too much: probe less often. */
        rate->raise_wait_ms = rate->raise_wait_ms * 2 < OSVC_VIDEO_RATE_RAISE_MAX_MS ?
                                  rate->raise_wait_ms * 2 :
                                  OSVC_VIDEO_RATE_RAISE_MAX_MS;
    }
    /* Half the frame rate under 3/16 of the ceiling (1.5 Mbit/s at 1080p);
     * the full rate again from half as much more. */
    const uint32_t fps_floor = rate->ceiling_kbps * 3U / 16U;
    rate->low_fps = kbps < fps_floor ||
                    (rate->low_fps && kbps < fps_floor + fps_floor / 2U);
    rate->kbps = kbps;
    rate->raised = raise;
    rate->changed_ms = now_ms;
    if (raise) {
        rate->raises++;
    } else {
        rate->cuts++;
    }
    return true;
}

void osvc_video_rate_hold(osvc_video_rate_t *rate, int64_t now_ms)
{
    rate->changed_ms = now_ms;
}

uint32_t osvc_video_rate_fps(const osvc_video_rate_t *rate, uint32_t fps_cap)
{
    return rate->low_fps && fps_cap > 1U ? fps_cap / 2U : fps_cap;
}

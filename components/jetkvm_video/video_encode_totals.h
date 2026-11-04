/* Encode times of the hardware H.264 encoder since boot, for /webrtc/stats.
 * No lock and no clock of its own: the caller holds the lock of the video
 * metrics and gives the time. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The longest times are kept per window of this length. A report has the
 * longest time of the current window and of the window before it, so a
 * frame's time stays in the reports for one to two window lengths after the
 * frame. Windows follow one another without a gap while frames come; after
 * a full window without a frame, the next frame starts a new one. */
#define VIDEO_ENCODE_LONGEST_WINDOW_US (2 * 1000 * 1000)

typedef struct {
    uint32_t p_encode_us;
    uint32_t idr_encode_us;
    uint32_t handoff_us;
} video_encode_longest_t;

/* All zero at boot; never reset. */
typedef struct {
    uint32_t p_frames;
    uint32_t idr_frames;
    uint64_t p_encode_us;
    uint64_t idr_encode_us;
    uint64_t handoff_us;
    int64_t window_end_us;
    video_encode_longest_t current;
    video_encode_longest_t previous;
} video_encode_totals_t;

/* One encoded frame: now_us is the end of its encode. */
static inline void video_encode_totals_add(video_encode_totals_t *totals,
                                           int64_t now_us, bool keyframe,
                                           uint32_t encode_us,
                                           uint32_t handoff_us)
{
    if (now_us >= totals->window_end_us) {
        if (now_us - totals->window_end_us < VIDEO_ENCODE_LONGEST_WINDOW_US) {
            totals->previous = totals->current;
            totals->window_end_us += VIDEO_ENCODE_LONGEST_WINDOW_US;
        } else {
            totals->previous = (video_encode_longest_t) {0};
            totals->window_end_us = now_us + VIDEO_ENCODE_LONGEST_WINDOW_US;
        }
        totals->current = (video_encode_longest_t) {0};
    }
    if (keyframe) {
        totals->idr_frames++;
        totals->idr_encode_us += encode_us;
        if (encode_us > totals->current.idr_encode_us) {
            totals->current.idr_encode_us = encode_us;
        }
    } else {
        totals->p_frames++;
        totals->p_encode_us += encode_us;
        if (encode_us > totals->current.p_encode_us) {
            totals->current.p_encode_us = encode_us;
        }
    }
    totals->handoff_us += handoff_us;
    if (handoff_us > totals->current.handoff_us) {
        totals->current.handoff_us = handoff_us;
    }
}

/* The longest times a report at now_us has; zero for a kind of frame that
 * was not encoded in the two windows. */
static inline video_encode_longest_t video_encode_totals_longest(
    const video_encode_totals_t *totals, int64_t now_us)
{
    if (now_us >= totals->window_end_us) {
        /* The current window has ended: it is the window before now. */
        return now_us - totals->window_end_us < VIDEO_ENCODE_LONGEST_WINDOW_US ?
               totals->current : (video_encode_longest_t) {0};
    }
    video_encode_longest_t longest = totals->current;
    if (totals->previous.p_encode_us > longest.p_encode_us) {
        longest.p_encode_us = totals->previous.p_encode_us;
    }
    if (totals->previous.idr_encode_us > longest.idr_encode_us) {
        longest.idr_encode_us = totals->previous.idr_encode_us;
    }
    if (totals->previous.handoff_us > longest.handoff_us) {
        longest.handoff_us = totals->previous.handoff_us;
    }
    return longest;
}

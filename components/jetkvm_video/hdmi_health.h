/* When the capture task resets the HDMI bridge because the receiver reports
 * a signal but no usable video comes. After an HPD pulse of the Mini, for
 * example the EDID restore at the end of an OS Services guest display, the
 * TC358743 can stay in a state in which it measures the source's
 * 1920x1080 as 960, 1280 or 959 pixels wide, or keeps the right size while
 * frames arrive at a few a second. Only a reset of the bridge, as at boot,
 * ends it; a rebuild of the capture pipeline does not.
 *
 * The capture task samples each pass in which it expects video: HPD high, a
 * detected mode, no guest display, video sleep or install hold. Video is
 * judged in windows of HDMI_HEALTH_WINDOW_US. A window is bad when fewer
 * than half the frames of the detected rates arrived, or when the transport
 * cannot stream the mode. Each pass expects the rate detected at that pass
 * for the time since the pass before, so a source that changes between
 * 1920x1080@30 and 1280x720@60 within a window is held to the frames of
 * each mode for the time of that mode. After HDMI_HEALTH_BAD_WINDOWS bad
 * windows in a row the caller resets the bridge, at most
 * HDMI_HEALTH_MAX_RESETS times until video runs well for
 * HDMI_HEALTH_GOOD_WINDOWS windows or the Mini raises HPD for another
 * reason.
 *
 * No clock of its own: the caller gives the time. Capture task only. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HDMI_HEALTH_WINDOW_US (INT64_C(10) * 1000 * 1000)
/* A mode switch of a working source costs at most a few seconds of one
 * window. */
#define HDMI_HEALTH_BAD_WINDOWS 2
/* A minute of good video ends the episode and makes resets available
 * again. */
#define HDMI_HEALTH_GOOD_WINDOWS 6
#define HDMI_HEALTH_MAX_RESETS 3

typedef enum {
    HDMI_HEALTH_IDLE,
    /* The caller must reset the bridge now. */
    HDMI_HEALTH_RESET,
    /* The last allowed reset did not bring video back; no more resets. */
    HDMI_HEALTH_GIVE_UP,
} hdmi_health_action_t;

typedef struct {
    /* A window runs since window_start_us. */
    bool open;
    int64_t window_start_us;
    /* The caller's frame counter at the last sample. */
    uint32_t last_counter;
    /* Frames since the window opened. */
    uint32_t frames;
    /* The caller's time at the last sample. */
    int64_t last_sample_us;
    /* Frames the detected rates call for since the window opened, in
     * frames per second times microseconds. */
    int64_t expected_fps_us;
    /* A pass in the window had a mode the transport cannot stream. */
    bool unstreamable;
    uint32_t bad_windows;
    uint32_t good_windows;
    /* Resets since the last arm or the last good minute. */
    uint32_t resets;
    bool gave_up;
} hdmi_health_t;

/* The Mini raised HPD for another reason than a reset of its own (boot,
 * EDID write, guest display restored, host display shown again). */
static inline void hdmi_health_arm(hdmi_health_t *health)
{
    *health = (hdmi_health_t) {0};
}

/* A pass without expected video: no signal, HPD low, guest display, video
 * sleep, install hold or screen-only mode. The window in progress is
 * dropped; the counts of bad windows and resets stay. */
static inline void hdmi_health_pause(hdmi_health_t *health)
{
    health->open = false;
}

/* The receiver detects a mode at `fps` and the caller expects video. counter
 * is the capture's frame counter, which may restart at zero; streamable is
 * false when the transport refused the mode. Returns HDMI_HEALTH_RESET when
 * the caller must reset the bridge now, and HDMI_HEALTH_GIVE_UP once when a
 * reset would be due but all are spent. */
static inline hdmi_health_action_t
hdmi_health_sample(hdmi_health_t *health, int64_t now_us, uint32_t counter,
                   uint16_t fps, bool streamable)
{
    if (!health->open) {
        health->open = true;
        health->window_start_us = now_us;
        health->last_sample_us = now_us;
        health->last_counter = counter;
        health->frames = 0;
        health->expected_fps_us = 0;
        health->unstreamable = false;
    }
    health->frames += counter >= health->last_counter ?
        counter - health->last_counter : counter;
    health->last_counter = counter;
    /* A mode without a frame interval is held to one frame a second. */
    health->expected_fps_us +=
        (int64_t)(fps ? fps : 1) * (now_us - health->last_sample_us);
    health->last_sample_us = now_us;
    if (!streamable) {
        health->unstreamable = true;
    }
    const int64_t elapsed_us = now_us - health->window_start_us;
    if (elapsed_us < HDMI_HEALTH_WINDOW_US) {
        return HDMI_HEALTH_IDLE;
    }
    const bool good = !health->unstreamable &&
        (int64_t)health->frames * 2 * 1000000 >= health->expected_fps_us;
    health->open = false;
    if (good) {
        health->bad_windows = 0;
        if (++health->good_windows >= HDMI_HEALTH_GOOD_WINDOWS) {
            health->good_windows = 0;
            health->resets = 0;
            health->gave_up = false;
        }
        return HDMI_HEALTH_IDLE;
    }
    health->good_windows = 0;
    if (++health->bad_windows < HDMI_HEALTH_BAD_WINDOWS) {
        return HDMI_HEALTH_IDLE;
    }
    health->bad_windows = 0;
    if (health->resets < HDMI_HEALTH_MAX_RESETS) {
        health->resets++;
        return HDMI_HEALTH_RESET;
    }
    if (health->gave_up) {
        return HDMI_HEALTH_IDLE;
    }
    health->gave_up = true;
    return HDMI_HEALTH_GIVE_UP;
}

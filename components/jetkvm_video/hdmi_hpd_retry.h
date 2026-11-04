/* When the capture task pulses HPD because the source sends no video after
 * the Mini raised HPD. Some sources do not start their output after a hot
 * plug: a Mac's DP-to-HDMI converter can stay off with +5 V on the cable
 * and the Mini listed as a display, until it sees another hot plug.
 *
 * The retry is armed by each HPD rise of the Mini (boot, EDID write, host
 * display shown again) and disarmed by the first detected signal after it.
 * A source that sent video once and then stops (display sleep, host off)
 * gets no pulse: an HPD pulse turns a sleeping Linux output on again.
 * After HDMI_HPD_RETRY_MAX_PULSES pulses without a signal the retry gives
 * up until the Mini raises HPD again.
 *
 * No clock of its own: the caller gives the time. Capture task only. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Wait without a signal before each pulse, counted from the HPD rise
 * before it. A working source sends video within a few seconds of the rise.
 * The waits grow, so a host that stays without output, such as one asleep
 * when the Mini restarts, sees one hot plug a minute at most. */
#define HDMI_HPD_RETRY_FIRST_US  (INT64_C(10) * 1000 * 1000)
#define HDMI_HPD_RETRY_SECOND_US (INT64_C(30) * 1000 * 1000)
#define HDMI_HPD_RETRY_LATER_US  (INT64_C(60) * 1000 * 1000)
/* Pulses after one HPD rise of the Mini. A source that stays without video
 * after these is left alone until the Mini's next HPD rise. */
#define HDMI_HPD_RETRY_MAX_PULSES 5

typedef enum {
    HDMI_HPD_RETRY_IDLE,
    /* The caller must pulse HPD now. */
    HDMI_HPD_RETRY_PULSE,
    /* The last pulse got no signal either; the retry is disarmed. */
    HDMI_HPD_RETRY_GIVE_UP,
} hdmi_hpd_retry_action_t;

typedef struct {
    /* No signal detected since the Mini's last HPD rise. */
    bool armed;
    /* The wait for the next pulse runs since wait_start_us. */
    bool waiting;
    int64_t wait_start_us;
    /* Pulses since the last arm. */
    uint32_t pulses;
    /* Pulses since the last arm or since +5 V returned; picks the wait. */
    uint32_t pulses_since_5v;
} hdmi_hpd_retry_t;

static inline int64_t hdmi_hpd_retry_wait_us(uint32_t pulses)
{
    return pulses == 0 ? HDMI_HPD_RETRY_FIRST_US :
           pulses == 1 ? HDMI_HPD_RETRY_SECOND_US :
                         HDMI_HPD_RETRY_LATER_US;
}

/* The Mini raised HPD, or holds it low and raises it later. Also after a
 * give-up. */
static inline void hdmi_hpd_retry_arm(hdmi_hpd_retry_t *retry)
{
    *retry = (hdmi_hpd_retry_t) {.armed = true};
}

/* The receiver detected a signal. */
static inline void hdmi_hpd_retry_signal(hdmi_hpd_retry_t *retry)
{
    *retry = (hdmi_hpd_retry_t) {0};
}

/* One pass of the capture loop with HPD high and no detected signal.
 * Returns HDMI_HPD_RETRY_PULSE when the caller must pulse HPD now; the next
 * wait then starts at the first such pass after HPD is high again. When the
 * wait after the last allowed pulse ends, it returns HDMI_HPD_RETRY_GIVE_UP
 * once and disarms. Without +5 V the wait starts over with the first
 * interval; the pulses already sent still count. */
static inline hdmi_hpd_retry_action_t
hdmi_hpd_retry_due(hdmi_hpd_retry_t *retry, int64_t now_us, bool cable_5v)
{
    if (!retry->armed) {
        return HDMI_HPD_RETRY_IDLE;
    }
    if (!cable_5v) {
        retry->waiting = false;
        retry->pulses_since_5v = 0;
        return HDMI_HPD_RETRY_IDLE;
    }
    if (!retry->waiting) {
        retry->waiting = true;
        retry->wait_start_us = now_us;
        return HDMI_HPD_RETRY_IDLE;
    }
    if (now_us - retry->wait_start_us <
        hdmi_hpd_retry_wait_us(retry->pulses_since_5v)) {
        return HDMI_HPD_RETRY_IDLE;
    }
    retry->waiting = false;
    if (retry->pulses >= HDMI_HPD_RETRY_MAX_PULSES) {
        retry->armed = false;
        return HDMI_HPD_RETRY_GIVE_UP;
    }
    retry->pulses++;
    retry->pulses_since_5v++;
    return HDMI_HPD_RETRY_PULSE;
}

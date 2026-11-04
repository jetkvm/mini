#!/usr/bin/env python3
"""Run the HDMI recovery pulse schedule against sources that start their
output after a given HPD rise, sleep, never start, or lose +5 V. The host
display policy that raises HPD after a pulse is the production one."""

from pathlib import Path
import re
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
video = (component / "jetkvm_video.c").read_text()
bridge_header = (component.parent /
                 "jetkvm_hdmi_bridge/include/jetkvm_hdmi_bridge.h").read_text()


def function(source, signature):
    start = source.index(signature)
    pos = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


def define(source, name):
    return re.search(rf"^#define {name}\b.*$", source, re.M).group(0)


production = "\n".join([
    define(bridge_header, "TC_HPD_LOW_MIN_MS"),
    define(video, "HOST_DISPLAY_IDLE_GRACE_US"),
    function(video, "static bool p4_host_display_should_advertise("),
    "static int64_t s_host_display_hidden_since_us;",
    function(video, "static int64_t p4_host_display_hidden_us("),
    function(video, "static bool p4_host_display_may_raise("),
    function(video, "static esp_err_t p4_host_display_sync("),
])

preamble = r'''
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "hdmi_hpd_retry.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define TAG "test"
#define ESP_LOGI(tag, fmt, ...) \
    ((void)(tag), (void)snprintf(NULL, 0, fmt, ##__VA_ARGS__))
#define ESP_RETURN_ON_FALSE(cond, err, tag, ...) \
    do { if (!(cond)) return (err); } while (0)
#define ESP_RETURN_ON_ERROR(expr, tag, ...) \
    do { esp_err_t e_ = (expr); if (e_ != ESP_OK) return e_; } while (0)
typedef struct { int unused; } jetkvm_hdmi_bridge_t;
typedef struct { jetkvm_hdmi_bridge_t bridge; } app_ctx_t;
typedef struct { bool browser_session_active; } webrtc_transport_stats_t;
static volatile bool s_host_display_idle_enabled;
static bool session_active;
static int64_t now_us;
static bool hpd = true;
static int rises;
static int64_t rise_at_us[16];
static int64_t esp_timer_get_time(void) { return now_us; }
static bool jetkvm_video_guest_owns_display(void) { return false; }
static esp_err_t webrtc_transport_get_stats(webrtc_transport_stats_t *stats)
{
    stats->browser_session_active = session_active;
    return ESP_OK;
}
static esp_err_t tc_enable_stream(jetkvm_hdmi_bridge_t *ctx, bool enable)
{
    (void)ctx; (void)enable;
    return ESP_OK;
}
static esp_err_t csi_destroy(app_ctx_t *ctx) { (void)ctx; return ESP_OK; }
static esp_err_t tc_set_hpd_output(jetkvm_hdmi_bridge_t *ctx, bool advertised)
{
    (void)ctx;
    if (!hpd && advertised && rises < 16) rise_at_us[rises++] = now_us;
    hpd = advertised;
    return ESP_OK;
}
'''

checks = r'''
#define S(n) ((int64_t)(n) * 1000000)

/* The source: +5 V on the cable, and video while HPD is high once it has
 * seen good_rise HPD rises (0: it sends video at once), until it sleeps. */
static bool cable_5v = true;
static int good_rise;
static bool asleep;
static bool source_sends(void)
{
    return hpd && cable_5v && !asleep && rises >= good_rise;
}

static app_ctx_t ctx;
static bool advertised = true;
static bool pulse_low;
static hdmi_hpd_retry_t retry = {.armed = true};
static int pulses;
static int64_t pulse_at_us[16];
static int give_ups;
static int64_t give_up_at_us;

/* The capture loop's passes: the host display policy, then 1 s detection
 * passes while HPD is high, 50 ms passes while it is low. */
static void run_until(int64_t end_us)
{
    while (now_us < end_us) {
        assert(p4_host_display_sync(&ctx, &advertised) == ESP_OK);
        assert(advertised == hpd);
        if (!advertised) {
            if (!pulse_low) hdmi_hpd_retry_arm(&retry);
            now_us += 50000;
            continue;
        }
        pulse_low = false;
        hdmi_hpd_retry_action_t action = HDMI_HPD_RETRY_IDLE;
        if (source_sends()) {
            hdmi_hpd_retry_signal(&retry);
        } else {
            action = hdmi_hpd_retry_due(&retry, now_us, cable_5v);
        }
        if (action == HDMI_HPD_RETRY_GIVE_UP) {
            give_ups++;
            give_up_at_us = now_us;
        } else if (action == HDMI_HPD_RETRY_PULSE) {
            assert(pulses < 16);
            pulse_at_us[pulses++] = now_us;
            assert(tc_set_hpd_output(&ctx.bridge, false) == ESP_OK);
            s_host_display_hidden_since_us = now_us;
            advertised = false;
            pulse_low = true;
            now_us += 50000;
            continue;
        }
        now_us += 1000000;
    }
}

static void reset(int64_t start_us)
{
    now_us = start_us;
    hpd = true;
    advertised = true;
    pulse_low = false;
    rises = 0;
    pulses = 0;
    give_ups = 0;
    cable_5v = true;
    asleep = false;
    good_rise = 0;
    session_active = false;
    s_host_display_idle_enabled = false;
    hdmi_hpd_retry_arm(&retry);
}

static void assert_near(int64_t actual_us, int64_t expected_us)
{
    assert(actual_us >= expected_us && actual_us <= expected_us + S(1));
}

int main(void)
{
    const int64_t low_us = (int64_t)TC_HPD_LOW_MIN_MS * 1000;

    /* A source that never starts after the Mini's HPD rise at t0: pulses
     * 10 s, 30 s, 60 s, 60 s and 60 s after each rise, each
     * TC_HPD_LOW_MIN_MS long. When the wait after the fifth pulse ends
     * without a signal the retry gives up once, and there is no sixth pulse
     * however long the source stays without video. */
    const int64_t t0 = S(100);
    reset(t0);
    good_rise = 99;
    run_until(t0 + S(3600));
    assert(pulses == 5 && rises == 5);
    assert_near(pulse_at_us[0], t0 + HDMI_HPD_RETRY_FIRST_US);
    for (int i = 0; i < 5; i++) {
        assert(rise_at_us[i] - pulse_at_us[i] >= low_us);
        assert(rise_at_us[i] - pulse_at_us[i] < low_us + 100000);
    }
    assert_near(pulse_at_us[1], rise_at_us[0] + HDMI_HPD_RETRY_SECOND_US);
    assert_near(pulse_at_us[2], rise_at_us[1] + HDMI_HPD_RETRY_LATER_US);
    assert_near(pulse_at_us[3], rise_at_us[2] + HDMI_HPD_RETRY_LATER_US);
    assert_near(pulse_at_us[4], rise_at_us[3] + HDMI_HPD_RETRY_LATER_US);
    assert(give_ups == 1);
    assert_near(give_up_at_us, rise_at_us[4] + HDMI_HPD_RETRY_LATER_US);
    assert(hpd);

    /* After the give-up, the next HPD rise of the Mini (here the idle
     * policy shows the display again for a session) arms the schedule
     * again: five more pulses from 10 s after that rise, then one more
     * give-up. */
    s_host_display_idle_enabled = true;
    run_until(now_us + S(60));
    assert(!hpd && pulses == 5);
    session_active = true;
    run_until(now_us + S(1));
    assert(hpd && rises == 6);
    run_until(now_us + S(3600));
    assert(pulses == 10 && give_ups == 2);
    assert_near(pulse_at_us[5], rise_at_us[5] + HDMI_HPD_RETRY_FIRST_US);
    assert_near(pulse_at_us[6], rise_at_us[6] + HDMI_HPD_RETRY_SECOND_US);
    assert_near(pulse_at_us[9], rise_at_us[9] + HDMI_HPD_RETRY_LATER_US);
    assert_near(give_up_at_us, rise_at_us[10] + HDMI_HPD_RETRY_LATER_US);

    /* A source that starts after the second pulse: no third pulse. */
    reset(t0);
    good_rise = 2;
    run_until(t0 + S(600));
    assert(pulses == 2 && rises == 2);

    /* A source that sent video and then sleeps gets no pulse. */
    reset(t0);
    run_until(t0 + S(5));
    asleep = true;
    run_until(t0 + S(600));
    assert(pulses == 0);

    /* No pulse without +5 V; when it returns the wait starts again with
     * the first interval, also after pulses. The pulses before the loss of
     * +5 V count toward the five. */
    reset(t0);
    good_rise = 99;
    run_until(t0 + S(60));
    assert(pulses == 2);
    cable_5v = false;
    run_until(t0 + S(300));
    assert(pulses == 2);
    cable_5v = true;
    const int64_t plugged_us = now_us;
    run_until(plugged_us + S(15));
    assert(pulses == 3);
    assert_near(pulse_at_us[2], plugged_us + HDMI_HPD_RETRY_FIRST_US);
    run_until(plugged_us + S(3600));
    assert(pulses == 5 && give_ups == 1);

    /* The idle policy hides the display and shows it for a session: that
     * rise arms the retry again from the first interval, after a source
     * that had video and after earlier pulses. */
    reset(t0);
    s_host_display_idle_enabled = true;
    session_active = true;
    run_until(t0 + S(5));
    session_active = false;
    good_rise = 99;
    run_until(t0 + S(60));
    assert(!hpd && pulses == 0);
    session_active = true;
    run_until(t0 + S(61));
    assert(hpd && rises == 1);
    run_until(rise_at_us[0] + S(15));
    assert(pulses == 1);
    assert_near(pulse_at_us[0], rise_at_us[0] + HDMI_HPD_RETRY_FIRST_US);
    run_until(rise_at_us[1] + S(31));
    assert(pulses == 2);
    /* The session ends while HPD is low for the second pulse: HPD rises
     * within the idle grace and falls when the grace ends. */
    session_active = false;
    run_until(now_us + S(10));
    assert(!hpd && rises == 3);
    session_active = true;
    run_until(now_us + S(3));
    assert(hpd && rises == 4);
    run_until(rise_at_us[3] + S(15));
    assert(pulses == 3);
    assert_near(pulse_at_us[2], rise_at_us[3] + HDMI_HPD_RETRY_FIRST_US);

    /* While the policy keeps the display hidden there is no pulse. */
    reset(t0);
    s_host_display_idle_enabled = true;
    good_rise = 99;
    run_until(t0 + S(600));
    assert(!hpd && pulses == 0);

    puts("HDMI recovery pulses follow the Mini's HPD rise only, five at most: PASS");
}
'''

with tempfile.TemporaryDirectory(prefix="video-hdmi-hpd-retry-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + production + checks)
    subprocess.run([
        "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2",
        "-I", str(component), str(path / "test.c"), "-o", str(path / "test"),
    ], check=True)
    subprocess.run([str(path / "test")], check=True)

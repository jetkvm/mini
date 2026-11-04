#!/usr/bin/env python3
"""Run the capture task's video health check against a bridge that delivers
good video, a trickle of frames, or a mode the transport refuses, and that
a reset brings back after a given number of resets or never. The check and
the bridge reset are the production ones."""

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


task = function(video, "static void hdmi_capture_task(")
# Every HPD rise that arms the recovery pulses also restores the resets.
assert task.count("hdmi_hpd_retry_arm(&hpd_retry);") == \
    task.count("hdmi_health_arm(&health);") == 3
# Capture running, a detected mode without capture, and a signal whose
# timings stay out of range are judged.
assert task.count("hdmi_health_check(&ctx, &health, &hpd_retry,") == 3
assert "ctx->h264_mode_refused = ret == ESP_ERR_NOT_SUPPORTED;" in function(
    video, "static void publish_prepared_h264_mode(")
assert "ctx->h264_mode_refused = false;" in function(
    video, "static esp_err_t csi_destroy(")
# A bridge that fails its setup restarts the Mini, as at boot.
assert "ESP_ERROR_CHECK(hdmi_bridge_reset(ctx));" in function(
    video, "static bool hdmi_health_check(")

production = "\n".join([
    re.search(r"^static const uint32_t TC358743_REFCLK_HZ = .*$", video,
              re.M).group(0),
    function(video, "static esp_err_t hdmi_bridge_reset("),
    function(video, "static bool hdmi_health_check("),
])

preamble = r'''
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hdmi_hpd_retry.h"
#include "hdmi_health.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define TAG "test"
static char last_log[256];
static int give_up_logs;
#define LOG_LINE(fmt, ...) \
    ((void)snprintf(last_log, sizeof(last_log), fmt, ##__VA_ARGS__), \
     give_up_logs += strstr(last_log, "no more resets") != NULL)
#define ESP_LOGI(tag, fmt, ...) ((void)(tag), LOG_LINE(fmt, ##__VA_ARGS__))
#define ESP_LOGW(tag, fmt, ...) ((void)(tag), LOG_LINE(fmt, ##__VA_ARGS__))
#define ESP_LOGE(tag, fmt, ...) ((void)(tag), LOG_LINE(fmt, ##__VA_ARGS__))
#define ESP_ERROR_CHECK(expr) \
    do { esp_err_t e_ = (expr); assert(e_ == ESP_OK); (void)e_; } while (0)
#define ESP_RETURN_ON_ERROR(expr, tag, ...) \
    do { esp_err_t e_ = (expr); if (e_ != ESP_OK) return e_; } while (0)
typedef struct { int unused; } jetkvm_hdmi_bridge_t;
typedef struct { uint16_t width, height, fps; } tc358743_timings_t;
typedef int jetkvm_video_edid_preset_t;
typedef struct { const char *label; } p4_edid_descriptor_t;
typedef struct { volatile uint32_t capture_frames; } video_metrics_t;
typedef struct {
    jetkvm_hdmi_bridge_t bridge;
    video_metrics_t metrics;
    bool h264_mode_refused;
} app_ctx_t;

static int64_t now_us;
static int64_t esp_timer_get_time(void) { return now_us; }

/* The bridge: bad until it has had fix_after resets (0: good from the
 * start). Bad either trickles frames or reports a mode the transport
 * refuses. */
static int fix_after;
static int resets;
static int64_t reset_at_us[16];
static bool refuses_when_bad;
static char calls[512];
static const p4_edid_descriptor_t presets[2] = {{"default"}, {"applied"}};
static void call(const char *name)
{
    strncat(calls, name, sizeof(calls) - strlen(calls) - 1);
    strncat(calls, " ", sizeof(calls) - strlen(calls) - 1);
}
static jetkvm_video_edid_preset_t p4_edid_control_applied_preset(void)
{
    return 1;
}
static const p4_edid_descriptor_t *p4_edid_descriptor(
    jetkvm_video_edid_preset_t preset)
{
    return &presets[preset];
}
static esp_err_t tc_enable_stream(jetkvm_hdmi_bridge_t *ctx, bool enable)
{
    (void)ctx;
    call(enable ? "stream-on" : "stream-off");
    return ESP_OK;
}
static esp_err_t csi_destroy(app_ctx_t *ctx)
{
    call("destroy");
    ctx->h264_mode_refused = false;
    return ESP_OK;
}
static void tc_hw_reset(jetkvm_hdmi_bridge_t *ctx)
{
    (void)ctx;
    call("hw-reset");
}
static esp_err_t tc_probe_chip(jetkvm_hdmi_bridge_t *ctx)
{
    (void)ctx;
    call("probe");
    return ESP_OK;
}
static esp_err_t tc_initial_setup(jetkvm_hdmi_bridge_t *ctx,
                                  uint32_t refclk_hz,
                                  const p4_edid_descriptor_t *edid)
{
    (void)ctx;
    assert(refclk_hz == 27000000);
    call(edid->label);
    assert(resets < 16);
    reset_at_us[resets++] = now_us;
    /* The EDID write holds HPD low. */
    now_us += (int64_t)TC_HPD_LOW_MIN_MS * 1000;
    return ESP_OK;
}
'''

checks = r'''
#define S(n) ((int64_t)(n) * 1000000)
#define PASS_US 33333

static app_ctx_t ctx;
static hdmi_health_t health;
static hdmi_hpd_retry_t retry;
static tc358743_timings_t mode = {1920, 1080, 30};
/* Without a signal the capture task pauses the check. */
static bool signal_present = true;
/* Frames a second of a good bridge, and of a bad one that trickles. */
static int good_rate = 28;
static int bad_rate = 5;
static int64_t frame_credit;

static bool bridge_bad(void)
{
    return resets < fix_after;
}

/* The capture loop's passes, one per frame period while a mode is up. A
 * reset tears capture down: the next mode starts its counter at zero. */
static void run_until(int64_t end_us)
{
    while (now_us < end_us) {
        if (!signal_present) {
            hdmi_health_pause(&health);
            now_us += S(1);
            continue;
        }
        const bool refused = bridge_bad() && refuses_when_bad;
        const int rate = bridge_bad() && !refuses_when_bad ? bad_rate :
                         good_rate;
        frame_credit += (int64_t)rate * PASS_US;
        while (frame_credit >= S(1)) {
            ctx.metrics.capture_frames++;
            frame_credit -= S(1);
        }
        ctx.h264_mode_refused = refused;
        if (hdmi_health_check(&ctx, &health, &retry, &mode, !refused)) {
            assert(!retry.waiting && retry.armed && retry.pulses == 0);
            ctx.metrics.capture_frames = 0;
            continue;
        }
        now_us += PASS_US;
    }
}

static void reset(int fix)
{
    now_us = S(100);
    fix_after = fix;
    resets = 0;
    refuses_when_bad = false;
    signal_present = true;
    good_rate = 28;
    mode.fps = 30;
    give_up_logs = 0;
    frame_credit = 0;
    calls[0] = '\0';
    ctx = (app_ctx_t) {0};
    ctx.metrics.capture_frames = 12345;
    hdmi_health_arm(&health);
    retry = (hdmi_hpd_retry_t) {0};
}

int main(void)
{
    const int64_t t0 = S(100);
    const int64_t reset_us = (int64_t)TC_HPD_LOW_MIN_MS * 1000;

    /* Good video for an hour: no reset. */
    reset(0);
    run_until(t0 + S(3600));
    assert(resets == 0 && give_up_logs == 0);

    /* A source at 30 Hz whose frames trickle in at 5 a second: the bridge
     * resets after two bad windows, from stream off to the applied EDID
     * preset, and video comes back with no further reset. */
    reset(1);
    run_until(t0 + S(3600));
    assert(resets == 1 && give_up_logs == 0);
    assert(reset_at_us[0] >= t0 + 2 * HDMI_HEALTH_WINDOW_US);
    assert(reset_at_us[0] < t0 + 2 * HDMI_HEALTH_WINDOW_US + S(1));
    assert(strcmp(calls, "stream-off destroy hw-reset probe applied ") == 0);

    /* The transport refuses the detected mode while frames arrive at the
     * full rate, as with 1280x1080: also reset. */
    reset(1);
    refuses_when_bad = true;
    run_until(t0 + S(3600));
    assert(resets == 1 && give_up_logs == 0);
    assert(reset_at_us[0] < t0 + 2 * HDMI_HEALTH_WINDOW_US + S(1));

    /* A bridge that no reset brings back: HDMI_HEALTH_MAX_RESETS resets,
     * two bad windows apart, then one give-up and nothing more for an
     * hour. */
    reset(99);
    run_until(t0 + S(3600));
    assert(resets == HDMI_HEALTH_MAX_RESETS && give_up_logs == 1);
    for (int i = 1; i < resets; i++) {
        const int64_t gap = reset_at_us[i] - reset_at_us[i - 1];
        assert(gap >= reset_us + 2 * HDMI_HEALTH_WINDOW_US);
        assert(gap < reset_us + 2 * HDMI_HEALTH_WINDOW_US + S(1));
    }
    /* The Mini's next HPD rise for another reason makes resets available
     * again. */
    hdmi_health_arm(&health);
    run_until(now_us + S(3600));
    assert(resets == 2 * HDMI_HEALTH_MAX_RESETS && give_up_logs == 2);

    /* The last allowed reset works: no give-up. A minute of good video
     * then makes all resets available again. */
    reset(HDMI_HEALTH_MAX_RESETS);
    run_until(t0 + S(600));
    assert(resets == HDMI_HEALTH_MAX_RESETS && give_up_logs == 0);
    fix_after = 99;
    run_until(t0 + S(3600));
    assert(resets == 2 * HDMI_HEALTH_MAX_RESETS && give_up_logs == 1);

    /* Less than a minute of good video between two bad episodes keeps the
     * count: no reset is left for the second one. */
    reset(HDMI_HEALTH_MAX_RESETS);
    run_until(t0 + S(30));
    while (resets < HDMI_HEALTH_MAX_RESETS) {
        run_until(now_us + PASS_US);
    }
    run_until(now_us + (HDMI_HEALTH_GOOD_WINDOWS - 1) * HDMI_HEALTH_WINDOW_US);
    fix_after = 99;
    run_until(now_us + S(3600));
    assert(resets == HDMI_HEALTH_MAX_RESETS && give_up_logs == 1);

    /* Mode switches of a working source: a few seconds without frames
     * every half minute cost no reset. Neither does a stretch without a
     * signal, which the capture task leaves to the HPD retry. */
    reset(0);
    for (int i = 0; i < 100; i++) {
        good_rate = 0;
        run_until(now_us + S(4));
        good_rate = 28;
        run_until(now_us + S(26));
    }
    signal_present = false;
    run_until(now_us + S(600));
    signal_present = true;
    run_until(now_us + S(600));
    assert(resets == 0 && give_up_logs == 0);

    /* A host that changes between 1920x1080@30 and 1280x720@60 every 4 s
     * with its signal kept, as in the host resolution round trips of the
     * integration tests: each change costs 1.5 s without frames while the
     * capture is rebuilt. Each rate counts for its own time, so ten
     * minutes of it cost no reset. */
    reset(0);
    for (int i = 0; i < 150; i++) {
        mode.fps = i % 2 ? 60 : 30;
        good_rate = 0;
        run_until(now_us + S(3) / 2);
        good_rate = i % 2 ? 55 : 28;
        run_until(now_us + S(5) / 2);
    }
    assert(resets == 0 && give_up_logs == 0);

    /* Somewhat more than half of the 30 Hz arriving is good; somewhat less
     * is not. */
    reset(0);
    good_rate = 16;
    run_until(t0 + S(3600));
    assert(resets == 0);
    good_rate = 14;
    /* The window in progress may still pass. */
    run_until(now_us + 3 * HDMI_HEALTH_WINDOW_US + S(1));
    assert(resets == 1);

    puts("HDMI bridge resets when its signal gives no usable video, three "
         "at most: PASS");
}
'''

with tempfile.TemporaryDirectory(prefix="video-hdmi-health-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(
        define(bridge_header, "TC_HPD_LOW_MIN_MS") + "\n" + preamble +
        production + checks)
    subprocess.run([
        "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2",
        "-I", str(component), str(path / "test.c"), "-o", str(path / "test"),
    ], check=True)
    subprocess.run([str(path / "test")], check=True)

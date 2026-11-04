#!/usr/bin/env python3
"""Run the production host display policy with a mocked bridge and clock."""

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
static volatile bool s_host_display_idle_enabled = true;
static bool session_active;
static int64_t now_us;
static bool hpd = true;
static int64_t hpd_low_at_us;
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
    if (hpd && !advertised) hpd_low_at_us = now_us;
    hpd = advertised;
    return ESP_OK;
}
'''

checks = r'''
/* One pass of the video task's loop: 50 ms apart while HPD is low. */
static void run_until(app_ctx_t *ctx, bool *advertised, int64_t end_us)
{
    while (now_us < end_us) {
        assert(p4_host_display_sync(ctx, advertised) == ESP_OK);
        assert(*advertised == hpd);
        now_us += 50000;
    }
}

int main(void)
{
    app_ctx_t ctx = {0};
    bool advertised = true;
    const int64_t min_low_us = (int64_t)TC_HPD_LOW_MIN_MS * 1000;
    now_us = 10000000;

    /* The last session closes; HPD falls after the idle grace. */
    run_until(&ctx, &advertised, now_us + HOST_DISPLAY_IDLE_GRACE_US + 100000);
    assert(!hpd);
    const int64_t first_low_us = hpd_low_at_us;

    /* A session 300 ms later keeps HPD low for the minimum, then the next
     * pass raises it without another event. */
    now_us = first_low_us + 300000;
    session_active = true;
    run_until(&ctx, &advertised, first_low_us + min_low_us);
    assert(!hpd);
    run_until(&ctx, &advertised, first_low_us + min_low_us + 50000);
    assert(hpd);

    /* After a long low phase a session raises HPD on the first pass. */
    session_active = false;
    run_until(&ctx, &advertised, now_us + HOST_DISPLAY_IDLE_GRACE_US + 100000);
    assert(!hpd);
    now_us = hpd_low_at_us + 11000000;
    session_active = true;
    assert(p4_host_display_sync(&ctx, &advertised) == ESP_OK);
    assert(hpd && advertised);
    puts("host display HPD stays low for TC_HPD_LOW_MIN_MS before a raise: PASS");
}
'''

with tempfile.TemporaryDirectory(prefix="video-host-display-hpd-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + production + checks)
    subprocess.run([
        "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2",
        str(path / "test.c"), "-o", str(path / "test"),
    ], check=True)
    subprocess.run([str(path / "test")], check=True)

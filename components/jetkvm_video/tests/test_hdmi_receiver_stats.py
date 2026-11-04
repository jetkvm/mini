#!/usr/bin/env python3
"""Run the capture task's two reads of the receiver, and the source of
"hdmiReceiver" in /webrtc/stats, against a register mock of the TC358743:
the report has what the chip said at the task's last read of each kind, the
report itself reads no register, and a failed read leaves the report as it
was."""

from pathlib import Path
import re
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
components = component.parent
video = (component / "jetkvm_video.c").read_text()
bridge = (components / "jetkvm_hdmi_bridge/jetkvm_hdmi_bridge.c").read_text()
bridge_header = (
    components / "jetkvm_hdmi_bridge/include/jetkvm_hdmi_bridge.h").read_text()
transport_header = (
    components / "webrtc_transport/include/webrtc_transport.h").read_text()


def function(source, signature):
    start = source.index(signature)
    pos = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


def between(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def structure(source, name):
    return re.search(
        r"typedef struct \{(?:(?!typedef struct).)*?\} " + name + ";",
        source, re.S)[0]


production = "\n".join([
    structure(bridge_header, "tc358743_timings_t"),
    structure(transport_header, "webrtc_transport_hdmi_receiver_t"),
    next(line for line in video.splitlines()
         if line.startswith("#define HDMI_CONNECTION_CHECK_MS")),
    function(bridge, "esp_err_t tc_get_sys_status("),
    function(bridge, "static bool tc_has_signal("),
    function(bridge, "esp_err_t tc_get_detected_timings("),
    between(video, "static portMUX_TYPE s_hdmi_receiver_mux",
            "typedef struct {\n    uint32_t generation;"),
])

preamble = r'''
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "tc358743_regs.h"
typedef int esp_err_t;
typedef struct { const char *log_tag; } jetkvm_hdmi_bridge_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_RETURN_ON_FALSE(condition, error, tag, ...) \
    do { (void)(tag); if (!(condition)) return (error); } while (0)
#define ESP_RETURN_ON_ERROR(call, tag, ...) \
    do { (void)(tag); esp_err_t e_ = (call); if (e_ != ESP_OK) return e_; } while (0)
#define ESP_LOGW(tag, ...) ((void)(tag))
static const char *esp_err_to_name(esp_err_t error)
{
    return error == ESP_OK ? "ESP_OK" :
           error == ESP_ERR_TIMEOUT ? "ESP_ERR_TIMEOUT" :
           error == ESP_ERR_NOT_FOUND ? "ESP_ERR_NOT_FOUND" :
           error == ESP_ERR_INVALID_RESPONSE ? "ESP_ERR_INVALID_RESPONSE" : "?";
}
static volatile bool s_video_hdmi_connected;
static int64_t now_us;
static int64_t esp_timer_get_time(void) { return now_us; }

/* The chip: one byte per register address, and a count of the reads. */
static uint8_t chip[0x10000];
static unsigned reads;
static esp_err_t read_error;
static esp_err_t tc_read_bytes(jetkvm_hdmi_bridge_t *ctx, uint16_t reg,
                               uint8_t *data, size_t len)
{
    (void)ctx;
    ++reads;
    if (read_error == ESP_OK) memcpy(data, &chip[reg], len);
    return read_error;
}
static esp_err_t tc_read_reg(jetkvm_hdmi_bridge_t *ctx, uint16_t reg,
                             int size, uint32_t *value)
{
    uint8_t byte = 0;
    assert(size == 1);
    esp_err_t error = tc_read_bytes(ctx, reg, &byte, 1);
    if (error == ESP_OK) *value = byte;
    return error;
}
static void chip_put16(uint16_t reg, uint16_t value)
{
    chip[reg] = value & 0xff;
    chip[reg + 1] = value >> 8;
}
/* A source that sends width x height in a frame of total_width x
 * total_height lines, one frame per interval_x100us. The chip counts the
 * lines of the frame twice. */
static void chip_signal(uint16_t width, uint16_t height, uint16_t total_width,
                        uint16_t total_height, uint16_t interval_x100us)
{
    chip[SYS_STATUS] = MASK_S_SYNC | MASK_S_HDMI | MASK_S_PHY_SCDT |
                       MASK_S_PHY_PLL | MASK_S_TMDS | MASK_S_DDC5V;
    chip[HPD_CTL] = MASK_HPD_OUT0;
    chip_put16(DE_WIDTH_H_LO, width);
    chip_put16(DE_WIDTH_V_LO, height);
    chip_put16(H_SIZE_LO, total_width);
    chip_put16(V_SIZE_LO, total_height * 2);
    chip_put16(FV_CNT_LO, interval_x100us);
}
'''

checks = r'''
int main(void)
{
    jetkvm_hdmi_bridge_t bridge = { .log_tag = "test" };
    int64_t next_check_us = 0;
    tc358743_timings_t timings;
    webrtc_transport_hdmi_receiver_t report;

    /* Before the task's first read the report says that nothing was read. */
    now_us = 1000000;
    jetkvm_video_get_hdmi_receiver_stats(&report);
    assert(!report.status_read && !report.detect_ran && reads == 0);

    /* 1920x1080 at 30 frames per second: both reads see it. */
    chip_signal(1920, 1080, 2200, 1125, 333);
    video_update_hdmi_connection(&bridge, &next_check_us);
    assert(video_detect_timings(&bridge, &timings) == ESP_OK);
    const unsigned reads_of_the_task = reads;
    now_us += 120000;
    jetkvm_video_get_hdmi_receiver_stats(&report);
    assert(reads == reads_of_the_task);
    assert(report.status_read && report.sys_status == 0x9f);
    assert(report.cable_5v && report.tmds_clock && report.phy_pll &&
           report.phy_scdt && report.sync);
    assert(report.status_age_ms == 120);
    assert(report.detect_ran && strcmp(report.detect_result, "ESP_OK") == 0);
    assert(report.detected_width == 1920 && report.detected_height == 1080 &&
           report.detected_fps == 30);
    assert(report.detect_age_ms == 120);

    /* The host's display sleeps: the cable's +5 V stays, the TMDS clock and
     * everything that needs it go. The status read is due after 500 ms. */
    chip[SYS_STATUS] = MASK_S_DDC5V;
    now_us = 1000000 + 499999;
    video_update_hdmi_connection(&bridge, &next_check_us);
    jetkvm_video_get_hdmi_receiver_stats(&report);
    assert(report.sys_status == 0x9f && report.status_age_ms == 499);
    now_us = 1000000 + 500000;
    video_update_hdmi_connection(&bridge, &next_check_us);
    assert(video_detect_timings(&bridge, &timings) == ESP_ERR_NOT_FOUND);
    jetkvm_video_get_hdmi_receiver_stats(&report);
    assert(report.sys_status == 0x01 && report.cable_5v);
    assert(!report.tmds_clock && !report.phy_pll && !report.phy_scdt &&
           !report.sync);
    assert(report.status_age_ms == 0);
    assert(strcmp(report.detect_result, "ESP_ERR_NOT_FOUND") == 0);
    assert(report.detected_width == 0 && report.detected_height == 0 &&
           report.detected_fps == 0 && report.detect_age_ms == 0);

    /* A clock that the receiver's PHY locked on, without video sync: the
     * state that asks for a look at the receiver, not at the host. */
    chip[SYS_STATUS] = MASK_S_PHY_SCDT | MASK_S_PHY_PLL | MASK_S_TMDS |
                       MASK_S_DDC5V;
    now_us += 500000;
    video_update_hdmi_connection(&bridge, &next_check_us);
    assert(video_detect_timings(&bridge, &timings) == ESP_ERR_NOT_FOUND);
    jetkvm_video_get_hdmi_receiver_stats(&report);
    assert(report.sys_status == 0x0f && report.tmds_clock && report.phy_pll &&
           report.phy_scdt && !report.sync);

    /* Sync is back with timings that are not settled: the report has the
     * result and the numbers that the chip gave. */
    chip_signal(3840, 1080, 4400, 1125, 333);
    now_us += 500000;
    video_update_hdmi_connection(&bridge, &next_check_us);
    assert(video_detect_timings(&bridge, &timings) == ESP_ERR_INVALID_RESPONSE);
    jetkvm_video_get_hdmi_receiver_stats(&report);
    assert(report.sync);
    assert(strcmp(report.detect_result, "ESP_ERR_INVALID_RESPONSE") == 0);
    assert(report.detected_width == 3840 && report.detected_height == 1080);

    /* A failed I2C read of the status leaves the report's status as it was
     * and lets its age grow; the detection reports its own failure. */
    chip_signal(1280, 720, 1650, 750, 167);
    read_error = ESP_ERR_TIMEOUT;
    now_us += 500000;
    video_update_hdmi_connection(&bridge, &next_check_us);
    assert(video_detect_timings(&bridge, &timings) == ESP_ERR_NOT_FOUND);
    now_us += 250000;
    jetkvm_video_get_hdmi_receiver_stats(&report);
    assert(report.sys_status == 0x9f && report.status_age_ms == 750);
    assert(strcmp(report.detect_result, "ESP_ERR_NOT_FOUND") == 0 &&
           report.detect_age_ms == 250);

    /* The next good reads have the new mode. */
    read_error = ESP_OK;
    now_us += 250000;
    video_update_hdmi_connection(&bridge, &next_check_us);
    assert(video_detect_timings(&bridge, &timings) == ESP_OK);
    jetkvm_video_get_hdmi_receiver_stats(&report);
    assert(report.status_age_ms == 0 && report.detected_width == 1280 &&
           report.detected_height == 720 && report.detected_fps == 60);
    puts("hdmiReceiver source: signal, display asleep, clock without sync, "
         "unsettled timings, I2C error: PASS");
}
'''

# Every timing detection of the capture task goes through the function that
# keeps the result: the bridge's own function is called from it alone.
assert video.count("tc_get_detected_timings(") == 2, \
    "a detection of the capture task does not reach the report"
assert "tc_get_detected_timings(bridge, timings);" in function(
    video, "static esp_err_t video_detect_timings(")

with tempfile.TemporaryDirectory(prefix="video-hdmi-receiver-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + production + checks)
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
        "-I", str(components / "jetkvm_hdmi_bridge"),
        str(path / "test.c"), "-o", str(path / "test"),
    ], check=True)
    subprocess.run([str(path / "test")], check=True)

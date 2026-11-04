#!/usr/bin/env python3
"""Run the production cable detector and polling code with an I2C mock."""

from pathlib import Path
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
video = (component / "jetkvm_video.c").read_text()
bridge = (component.parent / "jetkvm_hdmi_bridge/jetkvm_hdmi_bridge.c").read_text()


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


production = "\n".join([
    next(line for line in video.splitlines()
         if line.startswith("#define HDMI_CONNECTION_CHECK_MS")),
    function(bridge, "esp_err_t tc_get_sys_status("),
    between(video, "static portMUX_TYPE s_hdmi_receiver_mux",
            "static void video_update_hdmi_connection("),
    function(video, "static void video_update_hdmi_connection("),
    function(video, "bool jetkvm_video_is_hdmi_connected("),
])

preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "tc358743_regs.h"
typedef int esp_err_t;
typedef struct { int unused; } jetkvm_hdmi_bridge_t;
typedef struct { int unused; } tc358743_timings_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
static volatile bool s_video_hdmi_connected;
static int64_t now_us;
static unsigned reads;
static uint8_t sys_status;
static esp_err_t read_error;
static int64_t esp_timer_get_time(void) { return now_us; }
static esp_err_t tc_read_reg(jetkvm_hdmi_bridge_t *ctx, uint16_t reg,
                             int size, uint32_t *value)
{
    (void)ctx;
    assert(reg == 0x8520 && size == 1);
    ++reads;
    if (read_error == ESP_OK) *value = sys_status;
    return read_error;
}
'''

checks = r'''
int main(void)
{
    jetkvm_hdmi_bridge_t bridge = {0};
    int64_t next_check_us = 0;
    /* Boot without a viewer: source +5 V is present without video sync. */
    assert(!jetkvm_video_is_hdmi_connected());
    sys_status = 0x01;
    video_update_hdmi_connection(&bridge, &next_check_us);
    assert(jetkvm_video_is_hdmi_connected() && reads == 1);

    /* Video stops and the cable is removed between polls. */
    sys_status = 0;
    now_us = 499999;
    video_update_hdmi_connection(&bridge, &next_check_us);
    assert(jetkvm_video_is_hdmi_connected() && reads == 1);
    now_us = 500000;
    video_update_hdmi_connection(&bridge, &next_check_us);
    assert(!jetkvm_video_is_hdmi_connected() && reads == 2);

    /* Plug in again while capture remains inactive. */
    sys_status = 0x01;
    now_us = 1000000;
    video_update_hdmi_connection(&bridge, &next_check_us);
    assert(jetkvm_video_is_hdmi_connected());

    /* An I2C timeout must not report a cable removal. */
    read_error = ESP_ERR_TIMEOUT;
    now_us = 1500000;
    video_update_hdmi_connection(&bridge, &next_check_us);
    assert(jetkvm_video_is_hdmi_connected());
    uint8_t status = 0x01;
    assert(tc_get_sys_status(&bridge, &status) == ESP_ERR_TIMEOUT);
    assert(status == 0x01);

    /* Stale signal flags without source +5 V do not mean connected. */
    read_error = ESP_OK;
    sys_status = 0xfe;
    now_us = 2000000;
    video_update_hdmi_connection(&bridge, &next_check_us);
    assert(!jetkvm_video_is_hdmi_connected());

    /* An error also preserves a previously disconnected status. */
    read_error = ESP_ERR_TIMEOUT;
    now_us = 2500000;
    video_update_hdmi_connection(&bridge, &next_check_us);
    assert(!jetkvm_video_is_hdmi_connected());
    read_error = ESP_OK;
    sys_status = 0xff;
    now_us = 3000000;
    video_update_hdmi_connection(&bridge, &next_check_us);
    assert(jetkvm_video_is_hdmi_connected());
    puts("HDMI cable detection: boot, no sync, unplug/replug, I2C errors and polling: PASS");
}
'''

with tempfile.TemporaryDirectory(prefix="video-hdmi-connection-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(preamble + production + checks)
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
        "-I", str(component.parent / "jetkvm_hdmi_bridge"),
        str(path / "test.c"), "-o", str(path / "test"),
    ], check=True)
    subprocess.run([str(path / "test")], check=True)

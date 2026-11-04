#!/usr/bin/env python3
"""Run the renderer of "hdmiReceiver" in /webrtc/stats with real cJSON, and
check that the full and the compact view both have it."""

from pathlib import Path
import re
import subprocess
import tempfile


COMPONENT = Path(__file__).resolve().parents[1]
REPO = COMPONENT.parents[1]
RENDERER = (COMPONENT / "webrtc_stats_json.c").read_text()
HEADER = (COMPONENT / "include" / "webrtc_transport.h").read_text()


def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


# Both views report it: the full one and the one that ?bitrate=1 selects,
# which the walk records each second.
full = between(RENDERER, "static esp_err_t stats_json_create(",
               "\nesp_err_t webrtc_stats_json_create(")
compact = RENDERER[RENDERER.index("esp_err_t webrtc_stats_json_create_bitrate("):]
assert full.count("add_hdmi_receiver(root);") == 1
assert compact.count("add_hdmi_receiver(root);") == 1

# The source is in place before the HTTP server, which serves the stats,
# starts: no request reads the pointer while main writes it.
MAIN = (REPO / "main" / "main.c").read_text()
registration = re.search(
    r"webrtc_transport_set_hdmi_receiver_source\(\s*"
    r"jetkvm_video_get_hdmi_receiver_stats\);", MAIN)
assert registration and len(re.findall(
    r"webrtc_transport_set_hdmi_receiver_source\(", MAIN)) == 1
assert registration.start() < MAIN.index("ESP_ERROR_CHECK(webrtc_transport_start());")

structure = re.search(
    r"typedef struct \{(?:(?!typedef struct).)*?\} webrtc_transport_hdmi_receiver_t;",
    HEADER, re.S)[0]


def function(signature):
    """The definition that starts with signature, to its closing brace."""
    start = RENDERER.index(signature)
    pos = RENDERER.index("{", start) + 1
    depth = 1
    while depth:
        depth += (RENDERER[pos] == "{") - (RENDERER[pos] == "}")
        pos += 1
    return RENDERER[start:pos] + "\n"


source_pointer = ("static void (*s_hdmi_receiver)"
                  "(webrtc_transport_hdmi_receiver_t *receiver);\n")
assert source_pointer in RENDERER
production = (
    function("static void add_u64(") +
    source_pointer +
    function("void webrtc_transport_set_hdmi_receiver_source(") +
    function("static void add_hdmi_receiver(")
)

PREAMBLE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
"""

CHECKS = r"""
static webrtc_transport_hdmi_receiver_t reported;
static void source(webrtc_transport_hdmi_receiver_t *receiver)
{
    *receiver = reported;
}
static char *render(void)
{
    cJSON *root = cJSON_CreateObject();
    assert(root);
    add_hdmi_receiver(root);
    char *json = cJSON_PrintUnformatted(root);
    assert(json);
    cJSON_Delete(root);
    return json;
}
int main(void)
{
    /* Firmware without the video component's source. */
    char *json = render();
    assert(strcmp(json, "{\"hdmiReceiver\":null}") == 0);
    free(json);

    /* Before the capture task's first reads: no value that could be taken
     * for a receiver without a signal. */
    webrtc_transport_set_hdmi_receiver_source(source);
    json = render();
    assert(strcmp(json,
        "{\"hdmiReceiver\":{\"sysStatus\":null,\"statusAgeMs\":null,"
        "\"detect\":null,\"detectAgeMs\":null}}") == 0);
    free(json);

    /* A status without a detection yet: the receiver has the cable's +5 V
     * and a clock, and no sync. */
    reported = (webrtc_transport_hdmi_receiver_t) {
        .status_read = true, .sys_status = 0x0f, .cable_5v = true,
        .tmds_clock = true, .phy_pll = true, .phy_scdt = true, .sync = false,
        .status_age_ms = 480,
    };
    json = render();
    assert(strcmp(json,
        "{\"hdmiReceiver\":{\"sysStatus\":15,\"cable5v\":true,"
        "\"tmdsClock\":true,\"phyPll\":true,\"phyScdt\":true,\"sync\":false,"
        "\"statusAgeMs\":480,\"detect\":null,\"detectAgeMs\":null}}") == 0);
    free(json);

    /* A detection that is 60 days old beside a fresh status, as after a long
     * service video: the age does not wrap at 32 bits. */
    reported = (webrtc_transport_hdmi_receiver_t) {
        .status_read = true, .sys_status = 0x01, .cable_5v = true,
        .status_age_ms = 12,
        .detect_ran = true, .detect_result = "ESP_ERR_NOT_FOUND",
        .detect_age_ms = 5184000000ULL,
    };
    json = render();
    assert(strcmp(json,
        "{\"hdmiReceiver\":{\"sysStatus\":1,\"cable5v\":true,"
        "\"tmdsClock\":false,\"phyPll\":false,\"phyScdt\":false,"
        "\"sync\":false,\"statusAgeMs\":12,\"detect\":\"ESP_ERR_NOT_FOUND\","
        "\"detectedWidth\":0,\"detectedHeight\":0,\"detectedFps\":0,"
        "\"detectAgeMs\":5184000000}}") == 0);
    free(json);
    puts("hdmiReceiver in /webrtc/stats: PASS");
    return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="webrtc-stats-hdmi-receiver-") as directory:
    temp = Path(directory)
    (temp / "test.c").write_text(PREAMBLE + structure + "\n" + production + CHECKS)
    cjson = REPO / "managed_components/espressif__cjson/cJSON"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    "-I", str(cjson), str(temp / "test.c"), str(cjson / "cJSON.c"),
                    "-o", str(temp / "test"), "-lm"], check=True)
    subprocess.run([str(temp / "test")], check=True)

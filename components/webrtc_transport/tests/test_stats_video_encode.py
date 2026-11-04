#!/usr/bin/env python3
"""Run the renderer of "videoEncode" in /webrtc/stats with real cJSON, and
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


# Both views report it: the full one and the one that ?bitrate=1 selects.
full = between(RENDERER, "static esp_err_t stats_json_create(",
               "\nesp_err_t webrtc_stats_json_create(")
compact = RENDERER[RENDERER.index("esp_err_t webrtc_stats_json_create_bitrate("):]
assert full.count("add_video_encode(root);") == 1
assert compact.count("add_video_encode(root);") == 1

# The source is in place before the HTTP server, which serves the stats,
# starts: no request reads the pointer while main writes it.
MAIN = (REPO / "main" / "main.c").read_text()
registration = "webrtc_transport_set_video_encode_source(jetkvm_video_get_encode_stats);"
assert MAIN.count(registration) == 1
assert MAIN.index(registration) < MAIN.index("ESP_ERROR_CHECK(webrtc_transport_start());")

structure = re.search(
    r"typedef struct \{(?:(?!typedef struct).)*?\} webrtc_transport_video_encode_t;",
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


source_pointer = "static void (*s_video_encode)(webrtc_transport_video_encode_t *encode);\n"
assert source_pointer in RENDERER
production = (
    function("static void add_u64(") +
    source_pointer +
    function("void webrtc_transport_set_video_encode_source(") +
    function("static void add_video_encode(")
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
static webrtc_transport_video_encode_t reported;
static unsigned source_calls;
static void source(webrtc_transport_video_encode_t *encode)
{
    ++source_calls;
    *encode = reported;
}
static char *render(void)
{
    cJSON *root = cJSON_CreateObject();
    assert(root);
    add_video_encode(root);
    char *json = cJSON_PrintUnformatted(root);
    assert(json);
    cJSON_Delete(root);
    return json;
}
int main(void)
{
    /* Firmware without the video component's source. */
    char *json = render();
    assert(strcmp(json, "{\"videoEncode\":null}") == 0);
    free(json);

    /* Sums past 32 bits (71 minutes of encode time) keep every microsecond. */
    reported = (webrtc_transport_video_encode_t) {
        .p_frames = 4000000000U, .p_encode_us = 131600000000123ULL,
        .p_encode_max_us = 33490,
        .idr_frames = 7, .idr_encode_us = 192011, .idr_encode_max_us = 27743,
        .handoff_us = 4294967296ULL, .handoff_max_us = 14401,
    };
    webrtc_transport_set_video_encode_source(source);
    json = render();
    assert(source_calls == 1);
    assert(strcmp(json,
        "{\"videoEncode\":{\"pFrames\":4000000000,"
        "\"pEncodeUs\":131600000000123,\"pEncodeMaxUs\":33490,"
        "\"idrFrames\":7,\"idrEncodeUs\":192011,\"idrEncodeMaxUs\":27743,"
        "\"handoffUs\":4294967296,\"handoffMaxUs\":14401}}") == 0);
    free(json);

    /* Before the first frame the counters are there, at zero. */
    reported = (webrtc_transport_video_encode_t) {0};
    json = render();
    assert(strcmp(json,
        "{\"videoEncode\":{\"pFrames\":0,\"pEncodeUs\":0,\"pEncodeMaxUs\":0,"
        "\"idrFrames\":0,\"idrEncodeUs\":0,\"idrEncodeMaxUs\":0,"
        "\"handoffUs\":0,\"handoffMaxUs\":0}}") == 0);
    free(json);
    puts("videoEncode in /webrtc/stats: PASS");
    return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="webrtc-stats-video-encode-") as directory:
    temp = Path(directory)
    (temp / "test.c").write_text(PREAMBLE + structure + "\n" + production + CHECKS)
    cjson = REPO / "managed_components/espressif__cjson/cJSON"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    "-I", str(cjson), str(temp / "test.c"), str(cjson / "cJSON.c"),
                    "-o", str(temp / "test"), "-lm"], check=True)
    subprocess.run([str(temp / "test")], check=True)

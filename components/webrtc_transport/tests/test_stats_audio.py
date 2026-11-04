#!/usr/bin/env python3
"""Run the renderer of "audio" in /webrtc/stats with real cJSON, and check
that the full and the compact view both have it."""

from pathlib import Path
import re
import subprocess
import tempfile


COMPONENT = Path(__file__).resolve().parents[1]
REPO = COMPONENT.parents[1]
RENDERER = (COMPONENT / "webrtc_stats_json.c").read_text()
HEADER = (REPO / "components" / "jetkvm_audio" / "include" / "jetkvm_audio.h").read_text()


def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


# Both views report it: the full one and the one that ?bitrate=1 selects,
# which a poll once per second reads.
full = between(RENDERER, "static esp_err_t stats_json_create(",
               "\nesp_err_t webrtc_stats_json_create(")
compact = RENDERER[RENDERER.index("esp_err_t webrtc_stats_json_create_bitrate("):]
assert full.count("add_audio(root);") == 1
assert compact.count("add_audio(root);") == 1

structure = re.search(r"typedef struct \{[^}]*\} jetkvm_audio_stats_t;", HEADER)[0]


def function(signature):
    """The definition that starts with signature, to its closing brace."""
    start = RENDERER.index(signature)
    pos = RENDERER.index("{", start) + 1
    depth = 1
    while depth:
        depth += (RENDERER[pos] == "{") - (RENDERER[pos] == "}")
        pos += 1
    return RENDERER[start:pos] + "\n"


PREAMBLE = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
"""

STUB = r"""
static jetkvm_audio_stats_t reported;
static void jetkvm_audio_get_stats(jetkvm_audio_stats_t *stats) { *stats = reported; }
"""

CHECKS = r"""
static char *render(void)
{
    cJSON *root = cJSON_CreateObject();
    assert(root);
    add_audio(root);
    char *json = cJSON_PrintUnformatted(root);
    assert(json);
    cJSON_Delete(root);
    return json;
}
int main(void)
{
    /* Each counter under its own name, and a count past 31 bits as it is. */
    reported = (jetkvm_audio_stats_t) {
        .usb_ready = true, .streaming = true, .enabled = false,
        .received_packets = 3000000000U, .received_bytes = 72, .pcm_bytes = 192000,
        .encoded_frames = 50, .encode_errors = 2, .dropped_frames = 7,
    };
    char *json = render();
    assert(strcmp(json,
        "{\"audio\":{\"usbReady\":true,\"streaming\":true,\"enabled\":false,"
        "\"receivedPackets\":3000000000,\"receivedBytes\":72,\"pcmBytes\":192000,"
        "\"encodedFrames\":50,\"encodeErrors\":2,\"droppedFrames\":7}}") == 0);
    free(json);
    puts("audio in /webrtc/stats: PASS");
    return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="webrtc-stats-audio-") as directory:
    temp = Path(directory)
    (temp / "test.c").write_text(PREAMBLE + structure + "\n" + STUB + function("static void add_audio(") + CHECKS)
    cjson = REPO / "managed_components/espressif__cjson/cJSON"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    "-I", str(cjson), str(temp / "test.c"), str(cjson / "cJSON.c"),
                    "-o", str(temp / "test"), "-lm"], check=True)
    subprocess.run([str(temp / "test")], check=True)

#!/usr/bin/env python3
"""The TC358743 FIFO level suits every mode the EDID offers.

Builds the real tc358743_fifo_level() on the host and checks, for the modes of
tools/edid/generate_presets.py at the capture candidate's CSI rate, that the
level lies between what the transmitter needs so as not to run the FIFO empty
and the length of the line, that the modes the fixed level was chosen for keep
it, and that the capture path writes the level for the detected timings.
"""

from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
bridge = (ROOT / "components/jetkvm_hdmi_bridge/jetkvm_hdmi_bridge.c").read_text()
regs = (ROOT / "components/jetkvm_hdmi_bridge/tc358743_regs.h").read_text()
video = (ROOT / "components/jetkvm_video/jetkvm_video.c").read_text()


def extract_function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + "\n"


def define(name):
    match = re.search(rf"#define {name}\b.*\n", regs)
    assert match, name
    return match.group(0)


# The definition, not the forward declaration.
streaming = extract_function(video[video.rindex("static esp_err_t tc_start_streaming("):],
                             "static esp_err_t tc_start_streaming(")
assert streaming.index("tc_set_csi(") < streaming.index("tc_set_fifo_level(")

# name, active width, pixel clock in kHz; the candidate sends 2 x 594 Mbps.
MODES = [
    ("1920x1080p30", 1920, 74250, True), ("1280x720p60", 1280, 74250, True),
    ("1280x720p30", 1280, 74250, True), ("1024x768p30", 1024, 30000, True),
    ("1280x800p30", 1280, 39000, True), ("1280x1024p30", 1280, 50000, True),
    ("1440x900p30", 1440, 49250, True), ("1680x1050p30", 1680, 67750, True),
    ("800x600p60", 800, 40000, False), ("640x480p60", 640, 25175, False),
]
cases = "\n".join(
    f'    check("{name}", {width}U, {clock}000ULL, {"true" if fixed else "false"});'
    for name, width, clock, fixed in MODES)

PROGRAM = f"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
{define("TC358743_FIFO_LEVEL")}{define("TC358743_FIFO_LEVEL_MAX")}{define("TC358743_FIFO_MARGIN")}
{extract_function(bridge, "uint32_t tc358743_fifo_level(")}
static void check(const char *name, uint32_t width, uint64_t clock, bool fixed)
{{
    const uint64_t csi = 2ULL * 594000000ULL;
    const uint32_t level = tc358743_fifo_level(width, clock, csi);
    const uint32_t line = width / 2U;
    const uint64_t input = clock * 16U;
    const uint32_t needed = csi > input ? (uint32_t)((line * (csi - input) + csi - 1U) / csi) : 0U;
    if (level < needed || level >= line || level > TC358743_FIFO_LEVEL_MAX ||
        fixed != (level == TC358743_FIFO_LEVEL)) {{
        printf("%s: level %u, needed %u, line %u\\n", name, (unsigned)level, (unsigned)needed,
               (unsigned)line);
        assert(false);
    }}
}}
int main(void)
{{
{cases}
    return 0;
}}
"""

with tempfile.TemporaryDirectory(prefix="tc358743-fifo-") as directory:
    source_path = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    source_path.write_text(PROGRAM)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    str(source_path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("TC358743 FIFO level: between need and line length for every EDID mode: OK")

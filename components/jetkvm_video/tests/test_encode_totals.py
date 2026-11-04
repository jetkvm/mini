#!/usr/bin/env python3
"""Run the encoder's totals since boot: sums per frame type, and the longest
times of the current window and the window before it."""

from pathlib import Path
import subprocess
import tempfile


COMPONENT = Path(__file__).resolve().parents[1]

TEST = r"""
#include <assert.h>
#include <stdio.h>
#include "video_encode_totals.h"

#define W VIDEO_ENCODE_LONGEST_WINDOW_US

static video_encode_totals_t totals;

static void assert_longest(int64_t now_us, uint32_t p_us, uint32_t idr_us,
                           uint32_t handoff_us)
{
    const video_encode_longest_t longest =
        video_encode_totals_longest(&totals, now_us);
    assert(longest.p_encode_us == p_us);
    assert(longest.idr_encode_us == idr_us);
    assert(longest.handoff_us == handoff_us);
}

int main(void)
{
    /* Nothing was encoded since boot. */
    assert_longest(0, 0, 0, 0);
    assert_longest(10 * W, 0, 0, 0);

    /* The first frames come long after boot. Each type has its count, its
     * sum and its longest time; the handoff is over both types. */
    const int64_t start = 5 * W + 123;
    video_encode_totals_add(&totals, start, false, 33000, 100);
    video_encode_totals_add(&totals, start + 33000, true, 27000, 300);
    video_encode_totals_add(&totals, start + 66000, false, 32000, 200);
    assert(totals.p_frames == 2 && totals.p_encode_us == 65000);
    assert(totals.idr_frames == 1 && totals.idr_encode_us == 27000);
    assert(totals.handoff_us == 600);
    assert_longest(start + 66000, 33000, 27000, 300);

    /* The next window follows at once. Its shorter frame does not hide the
     * longest times of the window before, up to the window's last moment. */
    video_encode_totals_add(&totals, start + W, false, 31000, 50);
    assert_longest(start + W, 33000, 27000, 300);
    assert_longest(start + 2 * W - 1, 33000, 27000, 300);

    /* A frame of the third window ends the first: its times are gone, and
     * no keyframe is left in the two windows. */
    video_encode_totals_add(&totals, start + 2 * W, false, 30000, 40);
    assert_longest(start + 2 * W, 31000, 0, 50);

    /* Encoding stops. The last window's times stay for one more window
     * length, then the report has none. The counts and sums stay. */
    assert_longest(start + 3 * W - 1, 31000, 0, 50);
    assert_longest(start + 3 * W, 30000, 0, 40);
    assert_longest(start + 4 * W - 1, 30000, 0, 40);
    assert_longest(start + 4 * W, 0, 0, 0);
    assert(totals.p_frames == 4 && totals.p_encode_us == 126000);
    assert(totals.idr_frames == 1 && totals.handoff_us == 690);

    /* Encoding starts again later: no time of the old windows comes back. */
    video_encode_totals_add(&totals, start + 20 * W + 7, true, 28000, 10);
    assert_longest(start + 20 * W + 7, 0, 28000, 10);
    assert_longest(start + 21 * W + 6, 0, 28000, 10);
    video_encode_totals_add(&totals, start + 21 * W + 7, false, 29000, 20);
    assert_longest(start + 21 * W + 7, 29000, 28000, 20);

    /* The sums do not wrap at 32 bits (about 71 minutes of encode time). */
    video_encode_totals_t wide = {0};
    for (int frame = 0; frame < 3; ++frame) {
        video_encode_totals_add(&wide, frame, false, UINT32_MAX, UINT32_MAX);
    }
    assert(wide.p_encode_us == 3 * (uint64_t)UINT32_MAX);
    assert(wide.handoff_us == 3 * (uint64_t)UINT32_MAX);

    puts("encoder totals since boot: sums per type, longest of two windows: PASS");
    return 0;
}
"""

with tempfile.TemporaryDirectory(prefix="video-encode-totals-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(TEST)
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
        "-I", str(COMPONENT), str(path / "test.c"), "-o", str(path / "test"),
    ], check=True)
    subprocess.run([str(path / "test")], check=True)

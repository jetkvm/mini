#!/usr/bin/env python3
"""Run the bridge fault schedule against bridges that fail one access, never
answer, answer again after a time, or fail a few seconds after each setup,
and count the setups and the hot plugs that the source sees."""

from pathlib import Path
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]

checks = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "hdmi_bridge_fault.h"

#define S(n) ((int64_t)(n) * 1000000)
#define PASS_US 50000
#define FOR_GOOD INT64_MAX

/* The bridge: it answers from answers_from_us, and after each setup it
 * works for up_us, then fails an access. glitch_at_us: one more failed
 * access at that time. */
static int64_t answers_from_us;
static int64_t up_us;
static int64_t glitch_at_us;

static int64_t now_us;
static int64_t set_up_at_us;
static hdmi_bridge_fault_t fault;
static int setups;
static int64_t setup_at_us[512];
/* Setups that worked: each writes the EDID, so the source sees a hot plug. */
static int hot_plugs;
static int64_t hot_plug_at_us[512];

static void start(int64_t answers_from, int64_t up, int64_t glitch_at)
{
    answers_from_us = answers_from;
    up_us = up;
    glitch_at_us = glitch_at;
    now_us = 0;
    set_up_at_us = 0;
    fault = (hdmi_bridge_fault_t) {0};
    setups = hot_plugs = 0;
    /* The setup at boot. */
    if (answers_from_us == 0) {
        fault.up_since_us = 1;
    } else {
        hdmi_bridge_fault_raise(&fault, now_us);
    }
}

static bool access_fails(void)
{
    if (now_us < answers_from_us) {
        return true;
    }
    if (now_us == glitch_at_us) {
        return true;
    }
    return up_us != FOR_GOOD && now_us - set_up_at_us >= up_us;
}

/* The capture loop's passes. */
static void run_until(int64_t end_us)
{
    for (; now_us < end_us; now_us += PASS_US) {
        if (!fault.faulted && access_fails()) {
            assert(hdmi_bridge_fault_raise(&fault, now_us));
        }
        if (!hdmi_bridge_fault_setup_due(&fault, now_us)) {
            continue;
        }
        const bool worked = now_us >= answers_from_us;
        assert(setups < 512);
        setup_at_us[setups++] = now_us;
        if (worked) {
            hot_plug_at_us[hot_plugs++] = now_us;
            set_up_at_us = now_us;
        }
        hdmi_bridge_fault_setup_done(&fault, now_us, worked);
    }
}

int main(void)
{
    /* One failed access of a good bridge: one setup in the same pass, and
     * no other in the next hour. */
    start(0, FOR_GOOD, S(100));
    run_until(S(3700));
    assert(setups == 1 && hot_plugs == 1);
    assert(setup_at_us[0] == S(100));
    assert(!fault.faulted);

    /* A bridge that never answers: the setups thin out to one in 30 s and
     * do not stop. No setup works, so the source sees no hot plug. */
    start(FOR_GOOD, FOR_GOOD, -1);
    run_until(S(3600));
    static const int64_t first[] = {0, 1, 3, 8, 18, 48, 78};
    for (int i = 0; i < 7; i++) {
        assert(setup_at_us[i] == S(first[i]));
    }
    for (int i = 7; i < setups; i++) {
        assert(setup_at_us[i] - setup_at_us[i - 1] == S(30));
    }
    assert(setup_at_us[setups - 1] >= S(3600 - 30));
    assert(hot_plugs == 0 && fault.faulted);

    /* A bridge that answers 20 s after boot: the next setup of the
     * schedule brings it up, and it stays up. */
    start(S(20), FOR_GOOD, -1);
    run_until(S(3600));
    assert(hot_plugs == 1 && hot_plug_at_us[0] == S(48));
    assert(setups == 6 && !fault.faulted);

    /* A bridge that fails 5 s after each setup, as a unit with a weak
     * bridge supply does: it gets video for 5 s in each cycle, and the
     * source sees no more than two hot plugs a minute once the waits have
     * grown. */
    start(0, S(5), -1);
    run_until(S(3600));
    assert(fault.faults > 100);
    for (int i = 0; i < hot_plugs; i++) {
        if (hot_plug_at_us[i] < S(60)) {
            continue;
        }
        assert(hot_plug_at_us[i] - hot_plug_at_us[i - 1] >= S(30));
        assert(hot_plug_at_us[i] - hot_plug_at_us[i - 1] <= S(36));
    }
    assert(hot_plug_at_us[hot_plugs - 1] >= S(3600 - 36));

    /* The same bridge, then healthy: the fault after two good minutes is
     * handled like the first one, with a setup in the same pass. */
    start(0, S(5), -1);
    run_until(S(600));
    up_us = FOR_GOOD;
    run_until(S(1200));
    assert(!fault.faulted);
    const int before = setups;
    glitch_at_us = S(1500);
    run_until(S(1600));
    assert(setups == before + 1 && setup_at_us[before] == S(1500));
    assert(!fault.faulted);

    puts("a bridge fault gets a setup at once, then at growing waits, without end: PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="video-hdmi-bridge-fault-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(checks)
    subprocess.run([
        "cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O2",
        "-I", str(component), str(path / "test.c"), "-o", str(path / "test"),
    ], check=True)
    subprocess.run([str(path / "test")], check=True)

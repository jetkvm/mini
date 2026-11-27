/* When the capture task sets the TC358743 up again after the bridge failed
 * a register access or lost its setup.
 *
 * A bridge that does not answer, or that answers with its reset values, is
 * a fault of the bridge or its supply, not of the Mini as a whole: network,
 * USB and the screen work without it. So the capture task does not restart
 * the Mini. It resets the bridge and sets it up as at boot, and tries again
 * when that fails.
 *
 * Each setup that works writes the EDID, which pulses HPD: the source sees
 * a hot plug. The waits before a setup therefore grow while the bridge
 * keeps failing, and start over when it has worked for
 * HDMI_BRIDGE_FAULT_STABLE_US.
 *
 * No clock of its own: the caller gives the time. Capture task only. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The wait before a setup, by the number of setups since the bridge last
 * stayed up. The first follows the fault at once: one failed access of a
 * good bridge costs one rebuild of the pipeline. */
#define HDMI_BRIDGE_FAULT_WAITS_US \
    { 0, INT64_C(1) * 1000 * 1000, INT64_C(2) * 1000 * 1000, \
      INT64_C(5) * 1000 * 1000, INT64_C(10) * 1000 * 1000, \
      INT64_C(30) * 1000 * 1000 }
/* Time without a fault after which the bridge counts as up again. */
#define HDMI_BRIDGE_FAULT_STABLE_US (INT64_C(120) * 1000 * 1000)

typedef struct {
    /* The bridge needs a setup. */
    bool faulted;
    /* Setups since the bridge last stayed up; picks the wait. */
    uint32_t attempts;
    /* The earliest time of the next setup. */
    int64_t next_setup_us;
    /* The last setup that worked; 0 before the first. */
    int64_t up_since_us;
    /* Faults since boot, for the log. */
    uint32_t faults;
} hdmi_bridge_fault_t;

static inline int64_t hdmi_bridge_fault_wait_us(uint32_t attempts)
{
    static const int64_t waits[] = HDMI_BRIDGE_FAULT_WAITS_US;
    const uint32_t last = sizeof(waits) / sizeof(waits[0]) - 1;
    return waits[attempts < last ? attempts : last];
}

/* A register access failed, or the bridge has lost its setup. Returns true
 * for the first report of a fault; more reports before the next setup
 * change nothing. */
static inline bool hdmi_bridge_fault_raise(hdmi_bridge_fault_t *fault,
                                           int64_t now_us)
{
    if (fault->faulted) {
        return false;
    }
    if (fault->up_since_us != 0 &&
        now_us - fault->up_since_us >= HDMI_BRIDGE_FAULT_STABLE_US) {
        fault->attempts = 0;
    }
    fault->faulted = true;
    fault->faults++;
    fault->next_setup_us = now_us + hdmi_bridge_fault_wait_us(fault->attempts);
    return true;
}

/* True when the caller must reset the bridge and set it up now. */
static inline bool hdmi_bridge_fault_setup_due(const hdmi_bridge_fault_t *fault,
                                               int64_t now_us)
{
    return fault->faulted && now_us >= fault->next_setup_us;
}

/* The result of the setup that hdmi_bridge_fault_setup_due() asked for. */
static inline void hdmi_bridge_fault_setup_done(hdmi_bridge_fault_t *fault,
                                                int64_t now_us, bool worked)
{
    fault->attempts++;
    if (worked) {
        fault->faulted = false;
        fault->up_since_us = now_us;
    } else {
        fault->next_setup_us =
            now_us + hdmi_bridge_fault_wait_us(fault->attempts);
    }
}

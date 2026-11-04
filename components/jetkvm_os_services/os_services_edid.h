#pragma once

#include <stdbool.h>
#include <stdint.h>

#define OSVC_EDID_SIZE 256u

/* Modes the guest display can advertise. Nothing samples HDMI while the
 * guest display is presented, so the modes may exceed what the Mini
 * captures. */
typedef enum {
    OSVC_GUEST_RES_720P60 = 0,
    OSVC_GUEST_RES_1080P60,
    OSVC_GUEST_RES_1440P60,
    OSVC_GUEST_RES_2160P60,
} osvc_guest_resolution_t;

/* Builds the guest display EDID (base block plus CTA-861 extension) for one
 * preferred mode. The vendor ID is 0x28B4, which the sender uses to find the
 * KVM display; `serial` decides whether the guest OS knows the display
 * again (the same serial) or takes it for a new monitor (a new serial). Pure: no allocation or logging.
 * Returns false for an unknown resolution. */
bool osvc_edid_build(osvc_guest_resolution_t resolution, uint32_t serial,
                     uint8_t out[OSVC_EDID_SIZE]);

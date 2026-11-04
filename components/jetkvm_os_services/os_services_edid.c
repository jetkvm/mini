/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The guest display's EDID: a VESA E-EDID 1.3 base block followed by one
 * CTA-861 extension block (revision 3).
 *
 * The sender's computer sees this display on the KVM's HDMI port while OS
 * Services video replaces its own output. Nothing samples the HDMI signal
 * in that state; the EDID only has to make the computer's OS create a
 * display at the requested size. Every OS adopts the preferred timing of a
 * monitor it has not seen, so the requested mode is the first detailed
 * timing. The session chooses the serial number: the same at every
 * advertisement of one KVM and size for a sender that sets the display's
 * mode, and a new one each time for any other sender. The other modes stay
 * listed so the user may still pick one.
 *
 * Byte layout follows VESA E-EDID Release A, Rev. 1 (EDID 1.3) and
 * CTA-861-G (extension block, video and vendor-specific data blocks).
 */

#include "os_services_edid.h"

#include <stddef.h>
#include <string.h>

#define BLOCK_LEN 128U
#define DESCRIPTOR_LEN 18U
#define TEXT_LEN 13U

/* Base block offsets (EDID 1.3 section 3). */
#define BASE_VENDOR 8U
#define BASE_PRODUCT 10U
#define BASE_SERIAL 12U
#define BASE_WEEK 16U
#define BASE_YEAR 17U
#define BASE_VERSION 18U
#define BASE_INPUT 20U
#define BASE_SIZE_CM 21U
#define BASE_GAMMA 23U
#define BASE_FEATURES 24U
#define BASE_CHROMA 25U
#define BASE_STANDARD_TIMINGS 38U
#define BASE_DESCRIPTORS 54U
#define BASE_EXTENSIONS 126U

/* "JET" in the packed three-letter PNP ID: the sender looks for the KVM's
 * display by these bytes on every OS. */
#define VENDOR_ID 0x28B4U
/* Manufactured in week 36 of 2026 (the year counts from 1990). */
#define MANUFACTURE_WEEK 36U
#define MANUFACTURE_YEAR (2026U - 1990U)
/* Digital input, no colour depth or interface declared (1.3 has none). */
#define INPUT_DIGITAL 0x80U
/* (gamma x 100) - 100 for gamma 2.2. */
#define GAMMA_2_2 120U
/* Feature support: DPMS suspend (bit 6), display type 2 (bits 4:3), sRGB
 * as the default colour space (bit 2), preferred timing in the first
 * detailed descriptor (bit 1); no GTF, so no OS derives extra modes from
 * the range limits. */
#define FEATURES 0x56U

/* Display descriptor tags (EDID 1.3 section 3.10.3). */
#define TAG_SERIAL_TEXT 0xFFU
#define TAG_RANGE_LIMITS 0xFDU
#define TAG_PRODUCT_NAME 0xFCU

/* Physical size for about 110 pixels per inch at each mode, so Windows
 * suggests 100% scaling and macOS keeps a 1:1 desktop up to 1440p. At
 * 2160p macOS picks a 2x desktop whatever the physical size says. */
#define PIXELS_PER_INCH 110U

/* CTA-861 data block tags (CTA-861-G table 54). */
#define CTA_TAG 0x02U
#define CTA_REVISION 3U
#define CTA_HEADER_LEN 4U
#define CTA_VIDEO_BLOCK 2U
#define CTA_VENDOR_BLOCK 3U
#define CTA_EXTENDED_BLOCK 7U
#define CTA_YCBCR420_VIDEO 0x0EU
#define CTA_NATIVE_SVD 0x80U

typedef struct {
    uint16_t pixel_clock_10khz;
    uint16_t h_active;
    uint16_t h_blank;
    uint16_t h_front;
    uint16_t h_sync;
    uint16_t v_active;
    uint16_t v_blank;
    uint16_t v_front;
    uint16_t v_sync;
    /* A product code of its own per mode, so settings an OS keeps per
     * monitor model (Windows display scaling) stay with their mode. */
    uint16_t product;
    /* CTA-861 Video Identification Code; 0 for a mode CTA-861 lacks. */
    uint8_t vic;
    /* Offered only in the YCbCr 4:2:0 Video Data Block: the RGB mode
     * would need a 594 MHz HDMI link. */
    bool ycbcr420_only;
} guest_timing_t;

static const guest_timing_t s_timings[] = {
    /* CTA-861 VIC 4: 74.25 MHz, 1650 x 750 total. */
    [OSVC_GUEST_RES_720P60] = {
        .pixel_clock_10khz = 7425U,
        .h_active = 1280U, .h_blank = 370U, .h_front = 110U, .h_sync = 40U,
        .v_active = 720U, .v_blank = 30U, .v_front = 5U, .v_sync = 5U,
        .product = 0x4011U,
        .vic = 4U,
    },
    /* CTA-861 VIC 16: 148.5 MHz, 2200 x 1125 total. */
    [OSVC_GUEST_RES_1080P60] = {
        .pixel_clock_10khz = 14850U,
        .h_active = 1920U, .h_blank = 280U, .h_front = 88U, .h_sync = 44U,
        .v_active = 1080U, .v_blank = 45U, .v_front = 4U, .v_sync = 5U,
        .product = 0x4012U,
        .vic = 16U,
    },
    /* VESA CVT reduced blanking: 241.5 MHz, 2720 x 1481 total. */
    [OSVC_GUEST_RES_1440P60] = {
        .pixel_clock_10khz = 24150U,
        .h_active = 2560U, .h_blank = 160U, .h_front = 48U, .h_sync = 32U,
        .v_active = 1440U, .v_blank = 41U, .v_front = 3U, .v_sync = 5U,
        .product = 0x4013U,
        .vic = 0U,
    },
    /* CTA-861 VIC 97: 594 MHz, 4400 x 2250 total. */
    [OSVC_GUEST_RES_2160P60] = {
        .pixel_clock_10khz = 59400U,
        .h_active = 3840U, .h_blank = 560U, .h_front = 176U, .h_sync = 88U,
        .v_active = 2160U, .v_blank = 90U, .v_front = 8U, .v_sync = 10U,
        .product = 0x4014U,
        .vic = 97U,
        .ycbcr420_only = true,
    },
};

#define TIMING_COUNT (sizeof(s_timings) / sizeof(s_timings[0]))

/* sRGB primaries and D65 white point in 1/10000 CIE units (IEC 61966-2-1),
 * in the order EDID stores them: red x, y, green x, y, blue x, y, white
 * x, y. */
static const uint16_t s_chromaticity[8] = {
    6400U, 3300U, 3000U, 6000U, 1500U, 600U, 3127U, 3290U,
};

/* HDMI Vendor-Specific Data Block: IEEE OUI 00-0C-03, source physical
 * address 1.0.0.0, no optional features, and Max_TMDS_Clock 600 MHz (in
 * 5 MHz units). Without it Linux treats the sink as DVI and drops every
 * mode above 165 MHz, the 1440p one among them. */
static const uint8_t s_hdmi_vsdb_payload[] = {
    0x03U, 0x0CU, 0x00U, 0x10U, 0x00U, 0x00U, 600U / 5U,
};

static uint8_t block_checksum(const uint8_t *block)
{
    uint8_t sum = 0U;
    for (size_t i = 0U; i < BLOCK_LEN - 1U; ++i) {
        sum = (uint8_t)(sum + block[i]);
    }
    return (uint8_t)(0U - sum);
}

static uint16_t size_mm(uint16_t pixels)
{
    /* pixels / ppi * 25.4 mm, rounded to the nearest millimetre. */
    const uint32_t scaled = (uint32_t)pixels * 254U;
    const uint32_t divisor = PIXELS_PER_INCH * 10U;
    return (uint16_t)((scaled + divisor / 2U) / divisor);
}

/* A chromaticity coordinate as EDID's 10-bit binary fraction. */
static uint16_t chroma_10bit(uint16_t value_10000)
{
    return (uint16_t)(((uint32_t)value_10000 * 1024U + 5000U) / 10000U);
}

static void put_chromaticity(uint8_t out[10])
{
    uint16_t bits[8];
    for (size_t i = 0U; i < 8U; ++i) {
        bits[i] = chroma_10bit(s_chromaticity[i]);
    }
    /* Two bytes of low-order bit pairs, then the high eight bits of each
     * coordinate. */
    out[0] = (uint8_t)(((bits[0] & 3U) << 6) | ((bits[1] & 3U) << 4) |
                       ((bits[2] & 3U) << 2) | (bits[3] & 3U));
    out[1] = (uint8_t)(((bits[4] & 3U) << 6) | ((bits[5] & 3U) << 4) |
                       ((bits[6] & 3U) << 2) | (bits[7] & 3U));
    for (size_t i = 0U; i < 8U; ++i) {
        out[2U + i] = (uint8_t)(bits[i] >> 2);
    }
}

static void put_detailed_timing(uint8_t d[DESCRIPTOR_LEN],
                                const guest_timing_t *t, uint16_t width_mm,
                                uint16_t height_mm)
{
    memset(d, 0, DESCRIPTOR_LEN);
    d[0] = (uint8_t)(t->pixel_clock_10khz & 0xFFU);
    d[1] = (uint8_t)(t->pixel_clock_10khz >> 8);
    d[2] = (uint8_t)(t->h_active & 0xFFU);
    d[3] = (uint8_t)(t->h_blank & 0xFFU);
    d[4] = (uint8_t)(((t->h_active >> 8) << 4) | (t->h_blank >> 8));
    d[5] = (uint8_t)(t->v_active & 0xFFU);
    d[6] = (uint8_t)(t->v_blank & 0xFFU);
    d[7] = (uint8_t)(((t->v_active >> 8) << 4) | (t->v_blank >> 8));
    d[8] = (uint8_t)(t->h_front & 0xFFU);
    d[9] = (uint8_t)(t->h_sync & 0xFFU);
    d[10] = (uint8_t)(((t->v_front & 0x0FU) << 4) | (t->v_sync & 0x0FU));
    d[11] = (uint8_t)(((t->h_front >> 8) << 6) | ((t->h_sync >> 8) << 4) |
                      ((t->v_front >> 4) << 2) | (t->v_sync >> 4));
    d[12] = (uint8_t)(width_mm & 0xFFU);
    d[13] = (uint8_t)(height_mm & 0xFFU);
    d[14] = (uint8_t)(((width_mm >> 8) << 4) | (height_mm >> 8));
    /* Bytes 15-16: no border. Byte 17: progressive, no stereo, digital
     * separate sync (bits 4:3), positive vertical and horizontal sync. */
    d[17] = 0x18U | 0x04U | 0x02U;
}

/* A text descriptor: up to 13 characters, then a line feed and spaces. */
static void put_text(uint8_t d[DESCRIPTOR_LEN], uint8_t tag, const char *text,
                     size_t length)
{
    memset(d, 0, DESCRIPTOR_LEN);
    d[3] = tag;
    if (length > TEXT_LEN) {
        length = TEXT_LEN;
    }
    memcpy(d + 5, text, length);
    for (size_t i = 5U + length; i < DESCRIPTOR_LEN; ++i) {
        d[i] = i == 5U + length ? 0x0AU : 0x20U;
    }
}

static void put_range_limits(uint8_t d[DESCRIPTOR_LEN])
{
    memset(d, 0, DESCRIPTOR_LEN);
    d[3] = TAG_RANGE_LIMITS;
    d[5] = 23U;        /* minimum vertical rate, Hz */
    d[6] = 120U;       /* maximum vertical rate, Hz */
    d[7] = 30U;        /* minimum horizontal rate, kHz */
    d[8] = 140U;       /* maximum horizontal rate, kHz */
    d[9] = 600U / 10U; /* maximum pixel clock, 10 MHz units */
    /* Byte 10 = 0: no secondary timing formula; the rest is padding. */
    d[11] = 0x0AU;
    memset(d + 12, 0x20, DESCRIPTOR_LEN - 12U);
}

static void build_base(const guest_timing_t *t, uint32_t serial,
                       uint8_t base[BLOCK_LEN])
{
    static const char hex[] = "0123456789ABCDEF";
    const uint16_t width_mm = size_mm(t->h_active);
    const uint16_t height_mm = size_mm(t->v_active);

    memset(base, 0, BLOCK_LEN);
    base[0] = 0x00U;
    memset(base + 1, 0xFF, 6U);
    base[7] = 0x00U;
    base[BASE_VENDOR] = (uint8_t)(VENDOR_ID >> 8);
    base[BASE_VENDOR + 1U] = (uint8_t)(VENDOR_ID & 0xFFU);
    base[BASE_PRODUCT] = (uint8_t)(t->product & 0xFFU);
    base[BASE_PRODUCT + 1U] = (uint8_t)(t->product >> 8);
    for (unsigned i = 0U; i < 4U; ++i) {
        base[BASE_SERIAL + i] = (uint8_t)(serial >> (8U * i));
    }
    base[BASE_WEEK] = MANUFACTURE_WEEK;
    base[BASE_YEAR] = MANUFACTURE_YEAR;
    /* 1.3, not 1.4: a Windows GPU driver has been seen refusing the same
     * modes above 165 MHz from a 1.4 EDID. */
    base[BASE_VERSION] = 1U;
    base[BASE_VERSION + 1U] = 3U;
    base[BASE_INPUT] = INPUT_DIGITAL;
    base[BASE_SIZE_CM] = (uint8_t)(width_mm / 10U);
    base[BASE_SIZE_CM + 1U] = (uint8_t)(height_mm / 10U);
    base[BASE_GAMMA] = GAMMA_2_2;
    base[BASE_FEATURES] = FEATURES;
    put_chromaticity(base + BASE_CHROMA);
    /* Bytes 35-37: no established timings. Eight unused standard timing
     * slots. */
    memset(base + BASE_STANDARD_TIMINGS, 0x01, 16U);

    uint8_t *descriptor = base + BASE_DESCRIPTORS;
    put_detailed_timing(descriptor, t, width_mm, height_mm);
    descriptor += DESCRIPTOR_LEN;
    put_text(descriptor, TAG_PRODUCT_NAME, "JetKVM Guest", 12U);
    descriptor += DESCRIPTOR_LEN;
    put_range_limits(descriptor);
    descriptor += DESCRIPTOR_LEN;
    /* The serial number as text too, for OSes that show it. */
    char text[12] = {'J', 'K', 'V', 'G'};
    for (unsigned i = 0U; i < 8U; ++i) {
        text[4U + i] = hex[(serial >> (28U - 4U * i)) & 0x0FU];
    }
    put_text(descriptor, TAG_SERIAL_TEXT, text, sizeof(text));

    base[BASE_EXTENSIONS] = 1U;
    base[BLOCK_LEN - 1U] = block_checksum(base);
}

static void build_cta(const guest_timing_t *preferred,
                      uint8_t cta[BLOCK_LEN])
{
    memset(cta, 0, BLOCK_LEN);
    cta[0] = CTA_TAG;
    cta[1] = CTA_REVISION;
    /* Byte 3 = 0: no underscan, audio or YCbCr support declared, and no
     * native detailed timings counted. */
    size_t offset = CTA_HEADER_LEN;

    /* Video Data Block: every RGB mode with a VIC, the preferred one marked
     * native. */
    const size_t video_block = offset++;
    for (size_t i = 0U; i < TIMING_COUNT; ++i) {
        if (s_timings[i].vic == 0U || s_timings[i].ycbcr420_only) {
            continue;
        }
        cta[offset++] = (uint8_t)(s_timings[i].vic |
                                  (&s_timings[i] == preferred ?
                                       CTA_NATIVE_SVD : 0U));
    }
    cta[video_block] =
        (uint8_t)((CTA_VIDEO_BLOCK << 5) | (offset - video_block - 1U));

    /* YCbCr 4:2:0 Video Data Block: the modes offered only as 4:2:0. */
    const size_t ycbcr420_block = offset++;
    cta[offset++] = CTA_YCBCR420_VIDEO;
    for (size_t i = 0U; i < TIMING_COUNT; ++i) {
        if (s_timings[i].ycbcr420_only) {
            cta[offset++] = s_timings[i].vic;
        }
    }
    cta[ycbcr420_block] = (uint8_t)((CTA_EXTENDED_BLOCK << 5) |
                                    (offset - ycbcr420_block - 1U));

    cta[offset++] = (uint8_t)((CTA_VENDOR_BLOCK << 5) |
                              sizeof(s_hdmi_vsdb_payload));
    memcpy(cta + offset, s_hdmi_vsdb_payload, sizeof(s_hdmi_vsdb_payload));
    offset += sizeof(s_hdmi_vsdb_payload);

    /* Detailed timings start after the data blocks: the modes without a
     * VIC, except the preferred one, which the base block carries. */
    cta[2] = (uint8_t)offset;
    const uint16_t width_mm = size_mm(preferred->h_active);
    const uint16_t height_mm = size_mm(preferred->v_active);
    for (size_t i = 0U; i < TIMING_COUNT; ++i) {
        if (s_timings[i].vic != 0U || &s_timings[i] == preferred) {
            continue;
        }
        /* The image size is the panel's, the same for every timing. */
        put_detailed_timing(cta + offset, &s_timings[i], width_mm, height_mm);
        offset += DESCRIPTOR_LEN;
    }
    cta[BLOCK_LEN - 1U] = block_checksum(cta);
}

bool osvc_edid_build(osvc_guest_resolution_t resolution, uint32_t serial,
                     uint8_t out[OSVC_EDID_SIZE])
{
    if ((size_t)resolution >= TIMING_COUNT) {
        return false;
    }
    const guest_timing_t *preferred = &s_timings[resolution];
    build_base(preferred, serial, out);
    build_cta(preferred, out + BLOCK_LEN);
    return true;
}

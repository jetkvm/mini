/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The guest display EDID against golden EDID bytes for the same mode and
 * serial (os_services_edid_golden.h), and against the EDID and CTA-861
 * rules read back independently of the builder: block checksums, the vendor
 * ID and serial number, the preferred timing, and the full set of modes
 * offered.
 *
 * 3840x2160 is CTA-861 VIC 97 (2160p60) and is offered only in a YCbCr
 * 4:2:0 Video Data Block, which keeps the HDMI TMDS rate at 297 MHz without
 * scrambling.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "os_services_edid.h"
#include "os_services_edid_golden.h"

#define BLOCK 128U

typedef struct {
    osvc_guest_resolution_t resolution;
    const uint8_t *golden;
    uint16_t width;
    uint16_t height;
    uint32_t pixel_clock_khz;
    uint16_t h_total;
    uint16_t v_total;
    uint8_t vic; /* 0: not a CTA-861 mode */
    bool ycbcr420_only;
} guest_mode_t;

static const guest_mode_t k_modes[] = {
    {OSVC_GUEST_RES_720P60, k_golden_edid_720p60, 1280U, 720U, 74250U, 1650U,
     750U, 4U, false},
    {OSVC_GUEST_RES_1080P60, k_golden_edid_1080p60, 1920U, 1080U, 148500U,
     2200U, 1125U, 16U, false},
    {OSVC_GUEST_RES_1440P60, k_golden_edid_1440p60, 2560U, 1440U, 241500U,
     2720U, 1481U, 0U, false},
    {OSVC_GUEST_RES_2160P60, k_golden_edid_2160p60, 3840U, 2160U, 594000U,
     4400U, 2250U, 97U, true},
};
#define MODE_COUNT (sizeof(k_modes) / sizeof(k_modes[0]))

static void dump_difference(const char *what, const uint8_t *got,
                            const uint8_t *want)
{
    for (size_t i = 0U; i < 256U; ++i) {
        if (got[i] != want[i]) {
            fprintf(stderr, "%s: byte %zu is 0x%02X, want 0x%02X\n", what, i,
                    got[i], want[i]);
            return;
        }
    }
}

static uint8_t block_sum(const uint8_t *block)
{
    uint8_t sum = 0U;
    for (size_t i = 0U; i < BLOCK; ++i) {
        sum = (uint8_t)(sum + block[i]);
    }
    return sum;
}

static void build(osvc_guest_resolution_t resolution, uint32_t serial,
                  uint8_t out[OSVC_EDID_SIZE])
{
    memset(out, 0xA5, OSVC_EDID_SIZE);
    assert(osvc_edid_build(resolution, serial, out));
}

static void test_matches_the_golden_edids(void)
{
    for (size_t m = 0U; m < MODE_COUNT; ++m) {
        uint8_t got[OSVC_EDID_SIZE];
        build(k_modes[m].resolution, GOLDEN_EDID_SERIAL, got);

        if (memcmp(got, k_modes[m].golden, OSVC_EDID_SIZE) != 0) {
            dump_difference("EDID", got, k_modes[m].golden);
            abort();
        }
    }
}

static void test_checksums_vendor_and_header(void)
{
    static const uint32_t serials[] = {1U, 0x80000000U, 0xFFFFFFFFU,
                                       0x0BADF00DU};
    for (size_t m = 0U; m < MODE_COUNT; ++m) {
        for (size_t s = 0U; s < sizeof(serials) / sizeof(serials[0]); ++s) {
            uint8_t edid[OSVC_EDID_SIZE];
            build(k_modes[m].resolution, serials[s], edid);
            static const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF,
                                              0xFF, 0xFF, 0xFF, 0x00};
            assert(memcmp(edid, header, sizeof(header)) == 0);
            assert(block_sum(edid) == 0U);
            assert(block_sum(edid + BLOCK) == 0U);
            /* One extension, and it is CTA-861 revision 3. */
            assert(edid[126] == 1U);
            assert(edid[BLOCK] == 0x02U && edid[BLOCK + 1U] == 3U);
            /* EDID 1.3, digital input. */
            assert(edid[18] == 1U && edid[19] == 3U && (edid[20] & 0x80U));

            /* Vendor 0x28B4, big-endian, is "JET" in 5-bit letters. */
            assert(edid[8] == 0x28U && edid[9] == 0xB4U);
            const uint16_t id = (uint16_t)((edid[8] << 8) | edid[9]);
            const char vendor[4] = {(char)('A' - 1 + ((id >> 10) & 0x1F)),
                                    (char)('A' - 1 + ((id >> 5) & 0x1F)),
                                    (char)('A' - 1 + (id & 0x1F)), '\0'};
            assert(strcmp(vendor, "JET") == 0);
        }
    }
}

/* The display descriptor with `tag`, or NULL. */
static const uint8_t *find_descriptor(const uint8_t *edid, uint8_t tag)
{
    for (size_t offset = 54U; offset < 126U; offset += 18U) {
        const uint8_t *d = edid + offset;
        if (d[0] == 0U && d[1] == 0U && d[2] == 0U && d[3] == tag) {
            return d;
        }
    }
    return NULL;
}

static void test_serial_placement(void)
{
    uint8_t a[OSVC_EDID_SIZE];
    uint8_t b[OSVC_EDID_SIZE];
    build(OSVC_GUEST_RES_1080P60, 0xC0FFEE42U, a);
    build(OSVC_GUEST_RES_1080P60, 0x00000007U, b);

    /* Little-endian at bytes 12-15. */
    assert(a[12] == 0x42U && a[13] == 0xEEU && a[14] == 0xFFU &&
           a[15] == 0xC0U);
    assert(b[12] == 0x07U && b[13] == 0U && b[14] == 0U && b[15] == 0U);

    /* And as text in the serial number descriptor. */
    const uint8_t *text = find_descriptor(a, 0xFFU);
    assert(text != NULL);
    assert(memcmp(text + 5, "JKVGC0FFEE42\n", 13U) == 0);
    text = find_descriptor(b, 0xFFU);
    assert(text != NULL && memcmp(text + 5, "JKVG00000007\n", 13U) == 0);

    /* Nothing else depends on the serial: the two differ only in those
     * bytes and the base block checksum. */
    const size_t text_start = (size_t)(text - b) + 5U;
    for (size_t i = 0U; i < OSVC_EDID_SIZE; ++i) {
        const bool serial_byte =
            (i >= 12U && i <= 15U) ||
            (i >= text_start + 4U && i < text_start + 12U) || i == 127U;
        if (!serial_byte && a[i] != b[i]) {
            fprintf(stderr, "byte %zu depends on the serial\n", i);
            abort();
        }
    }

    /* The name descriptor. */
    const uint8_t *name = find_descriptor(a, 0xFCU);
    assert(name != NULL && memcmp(name + 5, "JetKVM Guest\n", 13U) == 0);
}

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t pixel_clock_khz;
    uint16_t h_total;
    uint16_t v_total;
} timing_t;

static timing_t decode_dtd(const uint8_t *d)
{
    timing_t t;
    t.pixel_clock_khz = (uint32_t)(d[0] | (d[1] << 8)) * 10U;
    t.width = (uint16_t)(d[2] | ((d[4] & 0xF0U) << 4));
    t.h_total = (uint16_t)(t.width + (d[3] | ((d[4] & 0x0FU) << 8)));
    t.height = (uint16_t)(d[5] | ((d[7] & 0xF0U) << 4));
    t.v_total = (uint16_t)(t.height + (d[6] | ((d[7] & 0x0FU) << 8)));
    return t;
}

static bool timing_is(const timing_t *t, const guest_mode_t *mode)
{
    return t->width == mode->width && t->height == mode->height &&
           t->pixel_clock_khz == mode->pixel_clock_khz &&
           t->h_total == mode->h_total && t->v_total == mode->v_total;
}

static void test_preferred_timing_and_mode_set(void)
{
    for (size_t m = 0U; m < MODE_COUNT; ++m) {
        const guest_mode_t *mode = &k_modes[m];
        uint8_t edid[OSVC_EDID_SIZE];
        build(mode->resolution, 0x13572468U, edid);

        /* The feature byte says the first descriptor is the preferred
         * timing, and that descriptor is the requested mode at 60 Hz. */
        assert(edid[24] & 0x02U);
        assert(!(edid[24] & 0x01U)); /* no GTF-derived modes */
        assert(edid[54] != 0U || edid[55] != 0U);
        const timing_t preferred = decode_dtd(edid + 54);
        assert(timing_is(&preferred, mode));
        const uint64_t millihertz =
            (uint64_t)preferred.pixel_clock_khz * 1000000U /
            ((uint64_t)preferred.h_total * preferred.v_total);
        assert(millihertz >= 59900U && millihertz <= 60100U);
        /* Positive sync, digital separate. */
        assert(edid[54 + 17] == 0x1EU);
        /* The image size there matches the base block's centimetres and
         * is near 110 pixels per inch. */
        const uint8_t *size = edid + 54 + 12;
        const unsigned width_mm = size[0] | ((size[2] & 0xF0U) << 4);
        const unsigned height_mm = size[1] | ((size[2] & 0x0FU) << 8);
        assert(edid[21] == width_mm / 10U && edid[22] == height_mm / 10U);
        assert(mode->width * 254U / width_mm / 10U >= 109U &&
               mode->width * 254U / width_mm / 10U <= 110U);

        /* Walk the CTA data blocks: one Video Data Block, one YCbCr 4:2:0
         * Video Data Block with the 4:2:0-only modes, the HDMI VSDB, nothing
         * else. A 4:2:0-only mode in the plain Video Data Block would claim
         * RGB, which needs a 594 MHz link at 2160p60. */
        const uint8_t *cta = edid + BLOCK;
        bool seen[MODE_COUNT] = {false};
        seen[m] = true;
        unsigned native = 0U;
        bool hdmi = false;
        bool ycbcr420 = false;
        for (size_t offset = 4U; offset < cta[2];) {
            const uint8_t tag = (uint8_t)(cta[offset] >> 5);
            const size_t length = cta[offset] & 0x1FU;
            assert(offset + 1U + length <= cta[2]);
            if (tag == 2U) {
                for (size_t i = 1U; i <= length; ++i) {
                    const uint8_t vic = cta[offset + i] & 0x7FU;
                    bool known = false;
                    for (size_t k = 0U; k < MODE_COUNT; ++k) {
                        if (k_modes[k].vic != 0U && k_modes[k].vic == vic &&
                            !k_modes[k].ycbcr420_only) {
                            seen[k] = true;
                            known = true;
                        }
                    }
                    assert(known);
                    if (cta[offset + i] & 0x80U) {
                        ++native;
                        assert(vic == mode->vic);
                    }
                }
            } else if (tag == 7U && length >= 1U &&
                       cta[offset + 1U] == 0x0EU) {
                for (size_t i = 2U; i <= length; ++i) {
                    bool known = false;
                    for (size_t k = 0U; k < MODE_COUNT; ++k) {
                        if (k_modes[k].ycbcr420_only &&
                            k_modes[k].vic == cta[offset + i]) {
                            seen[k] = true;
                            known = true;
                        }
                    }
                    assert(known);
                }
                ycbcr420 = true;
            } else if (tag == 3U) {
                assert(length == 7U);
                assert(cta[offset + 1U] == 0x03U && cta[offset + 2U] == 0x0CU &&
                       cta[offset + 3U] == 0x00U);
                assert(cta[offset + 4U] == 0x10U && cta[offset + 5U] == 0U);
                hdmi = true;
            } else {
                fprintf(stderr, "unexpected CTA data block tag %u\n", tag);
                abort();
            }
            offset += 1U + length;
        }
        assert(hdmi && ycbcr420);
        assert(native ==
               (mode->vic != 0U && !mode->ycbcr420_only ? 1U : 0U));
        /* Detailed timings after the data blocks list the rest. */
        for (size_t offset = cta[2]; offset + 18U <= BLOCK - 1U &&
                                     (cta[offset] | cta[offset + 1U]) != 0U;
             offset += 18U) {
            const timing_t t = decode_dtd(cta + offset);
            bool known = false;
            for (size_t k = 0U; k < MODE_COUNT; ++k) {
                if (timing_is(&t, &k_modes[k])) {
                    seen[k] = true;
                    known = true;
                    assert(k != m); /* the preferred one is not repeated */
                }
            }
            assert(known);
        }
        for (size_t k = 0U; k < MODE_COUNT; ++k) {
            assert(seen[k]);
        }
    }
}

static void test_rejects_unknown_resolution(void)
{
    static const uint32_t bad[] = {4U, 5U, 0x7FFFFFFFU, 0xFFFFFFFFU};
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        uint8_t edid[OSVC_EDID_SIZE];
        memset(edid, 0x5A, sizeof(edid));
        assert(!osvc_edid_build((osvc_guest_resolution_t)bad[i], 1U, edid));
        for (size_t j = 0U; j < sizeof(edid); ++j) {
            assert(edid[j] == 0x5AU);
        }
    }
}

int main(void)
{
    test_matches_the_golden_edids();
    test_checksums_vendor_and_header();
    test_serial_placement();
    test_preferred_timing_and_mode_set();
    test_rejects_unknown_resolution();
    puts("os_services EDID tests passed");
    return 0;
}

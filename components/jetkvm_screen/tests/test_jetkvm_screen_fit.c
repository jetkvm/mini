#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "jetkvm_screen_fit.h"

static void test_native_square_mapping(void)
{
    assert(jetkvm_screen_fit_source_x(0) == 0U);
    assert(jetkvm_screen_fit_source_x(JETKVM_SCREEN_PANEL_LANDSCAPE_WIDTH - 1U) ==
           JETKVM_SCREEN_PANEL_LANDSCAPE_WIDTH - 1U);

    jetkvm_screen_fit_span_t span = {0};
    assert(jetkvm_screen_fit_map_span(0, JETKVM_SCREEN_SOURCE_WIDTH - 1U, &span));
    assert(span.x1 == 0);
    assert(span.x2 == JETKVM_SCREEN_PANEL_LANDSCAPE_WIDTH - 1U);
    assert(jetkvm_screen_fit_map_span(-20, 39, &span));
    assert(span.x1 == 0 && span.x2 == 39);
    assert(jetkvm_screen_fit_map_span(239, 279, &span));
    assert(span.x1 == 239 && span.x2 == 239);
    assert(!jetkvm_screen_fit_map_span(240, 299, &span));
}

static uint16_t byte_swap(uint16_t value)
{
    return (uint16_t)((value << 8U) | (value >> 8U));
}

static void test_partial_copy_preserves_pixels_and_converts_transfer_endian(void)
{
    uint16_t source[JETKVM_SCREEN_SOURCE_WIDTH];
    uint16_t destination[JETKVM_SCREEN_PANEL_LANDSCAPE_WIDTH] = {0};
    for (uint16_t x = 0; x < JETKVM_SCREEN_SOURCE_WIDTH; ++x) {
        source[x] = (uint16_t)(0x4000U + x);
    }

    jetkvm_screen_fit_span_t span = {0};
    assert(jetkvm_screen_fit_map_span(0, JETKVM_SCREEN_SOURCE_WIDTH - 1U, &span));
    jetkvm_screen_fit_copy_rgb565_be(source, 0, JETKVM_SCREEN_SOURCE_WIDTH - 1U,
                                     1, destination, span);
    for (uint16_t x = 0; x < JETKVM_SCREEN_PANEL_LANDSCAPE_WIDTH; ++x) {
        assert(destination[x] ==
               byte_swap((uint16_t)(0x4000U + jetkvm_screen_fit_source_x(x))));
    }

    uint16_t partial_source[11];
    uint16_t partial_destination[11] = {0};
    memset(partial_source, 0, sizeof(partial_source));
    for (uint16_t x = 0; x < 11; ++x) {
        partial_source[x] = (uint16_t)(0x5000U + x);
    }
    assert(jetkvm_screen_fit_map_span(0, 10, &span));
    assert(span.x1 == 0 && span.x2 == 10);
    jetkvm_screen_fit_copy_rgb565_be(partial_source, 0, 10, 1,
                                     partial_destination, span);
    for (uint16_t x = 0; x < 11; ++x) {
        assert(partial_destination[x] ==
               byte_swap((uint16_t)(0x5000U + x)));
    }
}

static void test_rgb565_primary_wire_bytes(void)
{
    const uint16_t source[] = {0xF800U, 0x07E0U, 0x001FU};
    uint16_t destination[3] = {0};
    uint8_t wire_bytes[sizeof(destination)] = {0};
    jetkvm_screen_fit_span_t span = {0};

    /* ESP32-P4 and the host contract encode uint16_t least-significant byte first. */
    const uint16_t endian_probe = 1U;
    assert(*(const uint8_t *)&endian_probe == 1U);
    assert(jetkvm_screen_fit_map_span(0, 2, &span));
    jetkvm_screen_fit_copy_rgb565_be(source, 0, 2, 1, destination, span);
    memcpy(wire_bytes, destination, sizeof(wire_bytes));

    const uint8_t expected[] = {0xF8, 0x00, 0x07, 0xE0, 0x00, 0x1F};
    assert(memcmp(wire_bytes, expected, sizeof(expected)) == 0);
}

static void test_st7789p3_square_panel_contract(void)
{
    assert(JETKVM_SCREEN_PANEL_NATIVE_WIDTH == 240U);
    assert(JETKVM_SCREEN_PANEL_NATIVE_HEIGHT == 240U);
    assert(JETKVM_SCREEN_PANEL_LANDSCAPE_WIDTH == 240U);
    assert(JETKVM_SCREEN_PANEL_LANDSCAPE_HEIGHT == 240U);
    assert(JETKVM_SCREEN_SOURCE_WIDTH == 240U);
    assert(JETKVM_SCREEN_SOURCE_HEIGHT == 240U);
}

static void test_full_width_strip_fits_render_capacity(void)
{
    jetkvm_screen_fit_span_t span = {0};
    assert(jetkvm_screen_fit_map_span(0, JETKVM_SCREEN_SOURCE_WIDTH - 1U, &span));
    size_t viewport_width = (size_t)span.x2 - span.x1 + 1U;
    assert(viewport_width == 240U);
    assert(viewport_width * JETKVM_SCREEN_RENDER_BUFFER_LINES ==
           JETKVM_SCREEN_FLUSH_BUFFER_PIXELS);
}

static void test_invalid_spans_are_rejected(void)
{
    jetkvm_screen_fit_span_t span = {0};
    assert(!jetkvm_screen_fit_map_span(4, 3, &span));
    assert(!jetkvm_screen_fit_map_span(-20, -1, &span));
    assert(!jetkvm_screen_fit_map_span(JETKVM_SCREEN_SOURCE_WIDTH,
                                       JETKVM_SCREEN_SOURCE_WIDTH + 10U, &span));
    assert(!jetkvm_screen_fit_map_span(0, 10, NULL));
}

int main(void)
{
    test_native_square_mapping();
    test_partial_copy_preserves_pixels_and_converts_transfer_endian();
    test_rgb565_primary_wire_bytes();
    test_st7789p3_square_panel_contract();
    test_full_width_strip_fits_render_capacity();
    test_invalid_spans_are_rejected();
    puts("jetkvm_screen fit tests passed");
    return 0;
}

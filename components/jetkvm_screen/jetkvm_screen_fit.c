#include "jetkvm_screen_fit.h"

#include <stddef.h>

uint16_t jetkvm_screen_fit_source_x(uint16_t panel_x)
{
    if (panel_x >= JETKVM_SCREEN_PANEL_LANDSCAPE_WIDTH) {
        panel_x = JETKVM_SCREEN_PANEL_LANDSCAPE_WIDTH - 1U;
    }

    return panel_x;
}

bool jetkvm_screen_fit_map_span(int32_t source_x1,
                                int32_t source_x2,
                                jetkvm_screen_fit_span_t *panel_span)
{
    if (panel_span == NULL || source_x2 < 0 ||
        source_x1 >= (int32_t)JETKVM_SCREEN_SOURCE_WIDTH || source_x1 > source_x2) {
        return false;
    }

    if (source_x1 < 0) {
        source_x1 = 0;
    }
    if (source_x2 >= (int32_t)JETKVM_SCREEN_SOURCE_WIDTH) {
        source_x2 = (int32_t)JETKVM_SCREEN_SOURCE_WIDTH - 1;
    }

    panel_span->x1 = (uint16_t)source_x1;
    panel_span->x2 = (uint16_t)source_x2;
    return true;
}

void jetkvm_screen_fit_copy_rgb565_be(const uint16_t *source,
                                      uint16_t source_x1,
                                      uint16_t source_x2,
                                      uint16_t source_rows,
                                      uint16_t *destination,
                                      jetkvm_screen_fit_span_t panel_span)
{
    if (source == NULL || destination == NULL || source_x1 > source_x2 ||
        source_x2 >= JETKVM_SCREEN_SOURCE_WIDTH || panel_span.x1 > panel_span.x2 ||
        panel_span.x2 >= JETKVM_SCREEN_PANEL_LANDSCAPE_WIDTH) {
        return;
    }

    const size_t source_stride = (size_t)source_x2 - source_x1 + 1U;
    const size_t destination_stride = (size_t)panel_span.x2 - panel_span.x1 + 1U;
    const uint16_t visible_source_x1 = jetkvm_screen_fit_source_x(panel_span.x1);
    const size_t source_offset = visible_source_x1 - source_x1;
    for (uint16_t row = 0; row < source_rows; ++row) {
        const uint16_t *source_row = source + (size_t)row * source_stride + source_offset;
        uint16_t *destination_row = destination + (size_t)row * destination_stride;
        for (size_t x = 0; x < destination_stride; ++x) {
            uint16_t pixel = source_row[x];
            destination_row[x] = (uint16_t)((pixel << 8U) | (pixel >> 8U));
        }
    }
}

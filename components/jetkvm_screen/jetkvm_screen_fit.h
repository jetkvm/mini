#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JETKVM_SCREEN_SOURCE_WIDTH 240U
#define JETKVM_SCREEN_SOURCE_HEIGHT 240U
#define JETKVM_SCREEN_PANEL_NATIVE_WIDTH 240U
#define JETKVM_SCREEN_PANEL_NATIVE_HEIGHT 240U
#define JETKVM_SCREEN_PANEL_LANDSCAPE_WIDTH 240U
#define JETKVM_SCREEN_PANEL_LANDSCAPE_HEIGHT 240U
#define JETKVM_SCREEN_RENDER_BUFFER_LINES 20U
#define JETKVM_SCREEN_FLUSH_BUFFER_PIXELS \
    (JETKVM_SCREEN_SOURCE_WIDTH * JETKVM_SCREEN_RENDER_BUFFER_LINES)

typedef struct {
    uint16_t x1;
    uint16_t x2;
} jetkvm_screen_fit_span_t;

/* The square LVGL canvas maps one-to-one onto the square physical panel. */
uint16_t jetkvm_screen_fit_source_x(uint16_t panel_x);

/* Clips an inclusive source span to the physical panel. */
bool jetkvm_screen_fit_map_span(int32_t source_x1,
                                int32_t source_x2,
                                jetkvm_screen_fit_span_t *panel_span);

/*
 * Copies the visible viewport and byte-swaps native LVGL RGB565 for the
 * ST7789's big-endian transfer order.
 */
void jetkvm_screen_fit_copy_rgb565_be(const uint16_t *source,
                                      uint16_t source_x1,
                                      uint16_t source_x2,
                                      uint16_t source_rows,
                                      uint16_t *destination,
                                      jetkvm_screen_fit_span_t panel_span);

#ifdef __cplusplus
}
#endif

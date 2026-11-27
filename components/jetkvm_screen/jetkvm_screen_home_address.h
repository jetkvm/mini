#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The IPv4 address of the home screen. Its line is 216 px wide, and 194 px
 * when the button strip is on the left or right (jetkvm_screen_buttons.c).
 * The address is drawn in the layout's font when it fits its line, else in
 * the smaller ui_font_home_address_small, in which every address fits
 * 194 px. All calls run on the screen task with LVGL owned by the caller.
 */
extern const lv_font_t ui_font_home_address_small;

/* Shows `text` as the address. */
void jetkvm_screen_home_address_set(const char *text);

/* Selects the font again. Call after the width of the line changed. */
void jetkvm_screen_home_address_fit(void);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <lvgl.h>

#include "jetkvm_screen_fit.h"

/*
 * The Mini's local UI on an LVGL display in memory: the generated screens,
 * the Mini's own pages and the production button code with its strip
 * (jetkvm_screen_buttons.c), which reads two pins that this file holds.
 */

/* The canvas after lv_refr_now(), RGB565. */
extern uint16_t host_ui_framebuffer[JETKVM_SCREEN_SOURCE_WIDTH * JETKVM_SCREEN_SOURCE_HEIGHT];

/* Creates the display and the screens, as a build without the buttons has
 * them. False when LVGL gave no display. */
bool host_ui_start(void);

/* Starts the button code, as the screen task does after ui_init(). */
void host_ui_start_buttons(void);

/* One tap of a button, sampled every 10 ms as the screen loop samples it:
 * 0 is SW3 and 1 is SW2. */
void host_ui_tap(unsigned button);

/* One press of a button that is held for `milliseconds` and then released. */
void host_ui_hold(unsigned button, uint32_t milliseconds);

/* The parts of such a press, for a state that changes while it is held. */
void host_ui_press(unsigned button);
void host_ui_keep(uint32_t milliseconds);
void host_ui_release(unsigned button);

/* Writes the IPv4 address that is widest in `font`, with its kerning, among
 * all 2^32 addresses. */
void host_ui_widest_address(const lv_font_t *font, char address[16]);

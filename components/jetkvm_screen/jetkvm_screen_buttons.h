#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Drives the local LVGL UI from the two push buttons beside the panel's right
 * edge, SW3 (top) and SW2 (bottom). The top button goes back; the bottom
 * button steps down or scrolls on a tap and accepts on a hold, where the
 * confirmation screens keep their existing countdown. A strip beside the
 * buttons shows an icon per button. All calls run on the screen task with LVGL
 * owned by the caller.
 */
#if CONFIG_JETKVM_SCREEN_BUTTONS

/* Call once after ui_init(). A button edge notifies `task` with `notify_bits`
 * so the screen loop wakes at once instead of at its idle cadence. */
void jetkvm_screen_buttons_init(TaskHandle_t task, uint32_t notify_bits);

/* Samples both buttons, debounces them and dispatches presses. While the
 * HDMI `preview` covers the UI, a press only wakes the display. Returns true
 * while an edge is settling or a button is held so the loop keeps ticking. */
bool jetkvm_screen_buttons_poll(bool preview);

/* Wi-Fi setup started from the menu: on its page the buttons do not open the
 * menu, and while `stoppable` a held top button ends setup. */
void jetkvm_screen_buttons_set_wifi_setup(bool active, bool stoppable);

/* The picture turned `degrees` clockwise on the panel: the strip moves to the
 * canvas edge beside the buttons and the screens keep clear of it. */
void jetkvm_screen_buttons_set_rotation(uint16_t degrees);

#else

static inline void jetkvm_screen_buttons_init(TaskHandle_t task, uint32_t notify_bits)
{
    (void)task;
    (void)notify_bits;
}

static inline bool jetkvm_screen_buttons_poll(bool preview)
{
    (void)preview;
    return false;
}

static inline void jetkvm_screen_buttons_set_wifi_setup(bool active, bool stoppable)
{
    (void)active;
    (void)stoppable;
}

static inline void jetkvm_screen_buttons_set_rotation(uint16_t degrees)
{
    (void)degrees;
}

#endif

#ifdef __cplusplus
}
#endif

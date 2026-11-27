#pragma once
#include "esp_err.h"
#include "jetkvm_control.h"

typedef struct {
    esp_err_t (*rotation)(uint16_t degrees);
    esp_err_t (*brightness)(uint32_t value);
    esp_err_t (*active)(bool enabled);
} local_display_driver_t;
typedef struct {
    bool ready, active;
    /* The timers hold the backlight at 0 although its set brightness is above 0. */
    bool dark;
    uint16_t rotation;
    uint32_t brightness;
    esp_err_t error;
} local_display_state_t;
esp_err_t local_display_settings_init(uint16_t rotation, const local_display_driver_t *driver,
                                      bool restore_settings);
esp_err_t local_display_prepare(void);
esp_err_t local_display_ready(void);
esp_err_t local_display_active(bool enabled);
void local_display_tick(void);
/* A button press: wakes the display, also in a window of its schedule. */
void local_display_wake(void);
/* A network, HDMI or session state change: wakes the display, but not in a
 * window of its schedule. */
void local_display_notice(void);
/* Last completed state; never waits for a settings transaction. */
bool local_display_state(local_display_state_t *state);
uint32_t local_display_brightness(const jetkvm_backlight_settings_t *settings, uint64_t idle_seconds);

/* A press in a window lights the display for this long when neither idle
 * timer is set. */
#define LOCAL_DISPLAY_WINDOW_PRESS_SECONDS 30U
/* True in a window: it is on, and the minute of the local day at
 * utc_seconds, by the POSIX TZ rule of the schedule's zone, is from its start
 * to before its end. A window that is off, or a rule that is NULL or not
 * readable, gives false. */
bool local_display_in_window(const jetkvm_display_window_t *window, const char *rule, int64_t utc_seconds);
/* The brightness by the idle timers and the two windows; the darkest of
 * them wins. In the off window the display is off, and in the dim window at
 * most at half its set brightness. A press lifts the windows' limit until
 * the first idle timer that is set would act, or for
 * LOCAL_DISPLAY_WINDOW_PRESS_SECONDS without one. press_idle_seconds is
 * UINT64_MAX when there was no press. */
uint32_t local_display_scheduled_brightness(const jetkvm_backlight_settings_t *settings,
                                            bool in_dim_window, bool in_off_window,
                                            uint64_t idle_seconds, uint64_t press_idle_seconds);

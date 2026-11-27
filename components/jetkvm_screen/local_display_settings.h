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
/* A button press: wakes the display, also in the quiet hours. */
void local_display_wake(void);
/* A network, HDMI or session state change: wakes the display, but not in the
 * quiet hours. */
void local_display_notice(void);
/* Last completed state; never waits for a settings transaction. */
bool local_display_state(local_display_state_t *state);
uint32_t local_display_brightness(const jetkvm_backlight_settings_t *settings, uint64_t idle_seconds);

/* A press in the quiet hours lights the display for this long when neither
 * idle timer is set. */
#define LOCAL_DISPLAY_QUIET_PRESS_SECONDS 30U
/* True in the quiet hours of a schedule: it is on, and the minute of the
 * local day at utc_seconds, by the POSIX TZ rule of its zone, is from the
 * start to before the end. A schedule that is off, or a rule that is NULL or
 * not readable, has no quiet hours. */
bool local_display_quiet(const jetkvm_display_schedule_t *schedule, const char *rule, int64_t utc_seconds);
/* The brightness with the idle timers and the quiet hours. In the quiet hours
 * the display is at most off, or half its set brightness for a dim schedule.
 * A press lifts that limit until the first idle timer that is set would act,
 * or for LOCAL_DISPLAY_QUIET_PRESS_SECONDS without one. press_idle_seconds is
 * UINT64_MAX when there was no press. */
uint32_t local_display_scheduled_brightness(const jetkvm_backlight_settings_t *settings,
                                            const jetkvm_display_schedule_t *schedule, bool quiet,
                                            uint64_t idle_seconds, uint64_t press_idle_seconds);

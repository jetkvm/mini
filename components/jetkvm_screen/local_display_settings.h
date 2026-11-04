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
void local_display_wake(void);
/* Last completed state; never waits for a settings transaction. */
bool local_display_state(local_display_state_t *state);
uint32_t local_display_brightness(const jetkvm_backlight_settings_t *settings, uint64_t idle_seconds);

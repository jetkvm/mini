/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "jetkvm_control.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JETKVM_HID_PROFILES_SCHEMA_VERSION 1U

typedef struct {
    bool initialized;
    bool recovered_defaults;
    esp_err_t load_error;
    esp_err_t persistence_error;
    uint32_t revision;
} jetkvm_hid_profiles_status_t;

/** Populate the Go-compatible empty macro list and standard UTC jiggler. */
void jetkvm_hid_profiles_defaults(
    jetkvm_control_keyboard_macro_list_t *macros, bool *jiggler_enabled,
    jetkvm_control_jiggler_config_t *jiggler_config);

bool jetkvm_hid_profiles_macros_valid(
    const jetkvm_control_keyboard_macro_list_t *macros);
bool jetkvm_hid_profiles_jiggler_config_valid(
    const jetkvm_control_jiggler_config_t *config);

/** Validate and evaluate the six-field Cron syntax used by the Go service. */
bool jetkvm_hid_profiles_cron_valid(const char *expression);
bool jetkvm_hid_profiles_cron_matches(const char *expression,
                                      const struct tm *utc);

/** NVS codec. Outputs always contain safe defaults after a failed load. */
esp_err_t jetkvm_hid_profiles_load(
    jetkvm_control_keyboard_macro_list_t *macros, bool *jiggler_enabled,
    jetkvm_control_jiggler_config_t *jiggler_config);
esp_err_t jetkvm_hid_profiles_save(
    const jetkvm_control_keyboard_macro_list_t *macros,
    bool jiggler_enabled,
    const jetkvm_control_jiggler_config_t *jiggler_config);

/** Load and publish the persistent profile snapshot. */
esp_err_t jetkvm_hid_profiles_init(void);
bool jetkvm_hid_profiles_is_initialized(void);

esp_err_t jetkvm_hid_profiles_get_macros(
    jetkvm_control_keyboard_macro_list_t *macros);
/** Validate and persist before atomically publishing a replacement list. */
esp_err_t jetkvm_hid_profiles_set_macros(
    const jetkvm_control_keyboard_macro_list_t *macros);

esp_err_t jetkvm_hid_profiles_get_jiggler_state(bool *enabled);
esp_err_t jetkvm_hid_profiles_set_jiggler_state(bool enabled);
esp_err_t jetkvm_hid_profiles_get_jiggler_config(
    jetkvm_control_jiggler_config_t *config);
esp_err_t jetkvm_hid_profiles_set_jiggler_config(
    const jetkvm_control_jiggler_config_t *config);

/** Coherent scheduler snapshot; revision changes on every published update. */
esp_err_t jetkvm_hid_profiles_get_jiggler_snapshot(
    bool *enabled, jetkvm_control_jiggler_config_t *config,
    uint32_t *revision);

esp_err_t jetkvm_hid_profiles_get_status(
    jetkvm_hid_profiles_status_t *status);

/** Start the device-local jiggler scheduler after the target HID owner. */
esp_err_t jetkvm_hid_profiles_start_jiggler(void);

#ifdef __cplusplus
}
#endif

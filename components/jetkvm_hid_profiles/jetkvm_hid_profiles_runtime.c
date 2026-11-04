/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_hid_profiles.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "jetkvm_hidprof";

typedef struct {
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_storage;
    jetkvm_control_keyboard_macro_list_t *macros;
    bool jiggler_enabled;
    jetkvm_control_jiggler_config_t jiggler_config;
    jetkvm_hid_profiles_status_t status;
    esp_err_t init_result;
} hid_profiles_runtime_t;

static hid_profiles_runtime_t s_runtime;

static bool runtime_ready(void)
{
    return s_runtime.status.initialized && s_runtime.lock != NULL &&
           s_runtime.macros != NULL;
}

static void bump_revision_locked(void)
{
    s_runtime.status.revision++;
    if (s_runtime.status.revision == 0U) {
        s_runtime.status.revision++;
    }
}

esp_err_t jetkvm_hid_profiles_init(void)
{
    if (runtime_ready()) {
        return s_runtime.init_result;
    }
    memset(&s_runtime, 0, sizeof(s_runtime));
    s_runtime.macros = heap_caps_calloc(
        1U, sizeof(*s_runtime.macros), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_runtime.macros == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_runtime.lock = xSemaphoreCreateMutexStatic(&s_runtime.lock_storage);
    if (s_runtime.lock == NULL) {
        heap_caps_free(s_runtime.macros);
        s_runtime.macros = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t load_error = jetkvm_hid_profiles_load(
        s_runtime.macros, &s_runtime.jiggler_enabled,
        &s_runtime.jiggler_config);
    if (load_error != ESP_OK) {
        ESP_LOGW(TAG, "invalid HID profiles; using safe defaults: %s",
                 esp_err_to_name(load_error));
    }
    s_runtime.status = (jetkvm_hid_profiles_status_t) {
        .initialized = true,
        .recovered_defaults = load_error != ESP_OK,
        .load_error = load_error,
        .persistence_error = ESP_OK,
        .revision = 1U,
    };
    s_runtime.init_result = load_error;
    ESP_LOGI(TAG, "HID macro cache uses %u bytes of PSRAM",
             (unsigned)sizeof(*s_runtime.macros));
    return load_error;
}

bool jetkvm_hid_profiles_is_initialized(void)
{
    return runtime_ready();
}

esp_err_t jetkvm_hid_profiles_get_macros(
    jetkvm_control_keyboard_macro_list_t *macros)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (macros == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    *macros = *s_runtime.macros;
    xSemaphoreGive(s_runtime.lock);
    return ESP_OK;
}

esp_err_t jetkvm_hid_profiles_set_macros(
    const jetkvm_control_keyboard_macro_list_t *macros)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!jetkvm_hid_profiles_macros_valid(macros)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    esp_err_t error = jetkvm_hid_profiles_save(
        macros, s_runtime.jiggler_enabled, &s_runtime.jiggler_config);
    if (error == ESP_OK) {
        *s_runtime.macros = *macros;
        s_runtime.status.persistence_error = ESP_OK;
        bump_revision_locked();
    } else {
        s_runtime.status.persistence_error = error;
    }
    xSemaphoreGive(s_runtime.lock);
    return error;
}

esp_err_t jetkvm_hid_profiles_get_jiggler_state(bool *enabled)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enabled == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    *enabled = s_runtime.jiggler_enabled;
    xSemaphoreGive(s_runtime.lock);
    return ESP_OK;
}

esp_err_t jetkvm_hid_profiles_set_jiggler_state(bool enabled)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    if (s_runtime.jiggler_enabled == enabled) {
        xSemaphoreGive(s_runtime.lock);
        return ESP_OK;
    }
    esp_err_t error = jetkvm_hid_profiles_save(
        s_runtime.macros, enabled, &s_runtime.jiggler_config);
    if (error == ESP_OK) {
        s_runtime.jiggler_enabled = enabled;
        s_runtime.status.persistence_error = ESP_OK;
        bump_revision_locked();
    } else {
        s_runtime.status.persistence_error = error;
    }
    xSemaphoreGive(s_runtime.lock);
    return error;
}

esp_err_t jetkvm_hid_profiles_get_jiggler_config(
    jetkvm_control_jiggler_config_t *config)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    *config = s_runtime.jiggler_config;
    xSemaphoreGive(s_runtime.lock);
    return ESP_OK;
}

esp_err_t jetkvm_hid_profiles_set_jiggler_config(
    const jetkvm_control_jiggler_config_t *config)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!jetkvm_hid_profiles_jiggler_config_valid(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    esp_err_t error = jetkvm_hid_profiles_save(
        s_runtime.macros, s_runtime.jiggler_enabled, config);
    if (error == ESP_OK) {
        s_runtime.jiggler_config = *config;
        s_runtime.status.persistence_error = ESP_OK;
        bump_revision_locked();
    } else {
        s_runtime.status.persistence_error = error;
    }
    xSemaphoreGive(s_runtime.lock);
    return error;
}

esp_err_t jetkvm_hid_profiles_get_jiggler_snapshot(
    bool *enabled, jetkvm_control_jiggler_config_t *config,
    uint32_t *revision)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (enabled == NULL || config == NULL || revision == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    *enabled = s_runtime.jiggler_enabled;
    *config = s_runtime.jiggler_config;
    *revision = s_runtime.status.revision;
    xSemaphoreGive(s_runtime.lock);
    return ESP_OK;
}

esp_err_t jetkvm_hid_profiles_get_status(
    jetkvm_hid_profiles_status_t *status)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    *status = s_runtime.status;
    xSemaphoreGive(s_runtime.lock);
    return ESP_OK;
}

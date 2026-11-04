/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_settings.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "jetkvm_settings";

typedef struct {
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_storage;
    jetkvm_settings_t settings;
    jetkvm_settings_status_t status;
    esp_err_t init_result;
} settings_runtime_t;

static settings_runtime_t s_runtime;

static bool runtime_ready(void)
{
    return s_runtime.status.initialized && s_runtime.lock;
}

esp_err_t jetkvm_settings_init(void)
{
    if (runtime_ready()) {
        return s_runtime.init_result;
    }

    memset(&s_runtime, 0, sizeof(s_runtime));
    s_runtime.lock = xSemaphoreCreateMutexStatic(&s_runtime.lock_storage);
    if (!s_runtime.lock) {
        return ESP_ERR_NO_MEM;
    }

    bool migrated = false;
    esp_err_t load_error = jetkvm_settings_load(&s_runtime.settings,
                                                &migrated);
    esp_err_t persistence_error = ESP_OK;
    if (load_error != ESP_OK) {
        /* load() already left a validated default snapshot in place. */
        ESP_LOGW(TAG,
                 "invalid settings record; using en-US defaults: %s",
                 esp_err_to_name(load_error));
    } else if (migrated) {
        persistence_error = jetkvm_settings_save(&s_runtime.settings);
        if (persistence_error != ESP_OK) {
            ESP_LOGW(TAG,
                     "settings migrated in memory but commit failed: %s",
                     esp_err_to_name(persistence_error));
        }
    }

    s_runtime.status = (jetkvm_settings_status_t) {
        .initialized = true,
        .recovered_defaults = load_error != ESP_OK,
        .settings_migrated = migrated,
        .load_error = load_error,
        .persistence_error = persistence_error,
    };
    s_runtime.init_result = load_error != ESP_OK ? load_error :
                            persistence_error;
    return s_runtime.init_result;
}

bool jetkvm_settings_is_initialized(void)
{
    return runtime_ready();
}

esp_err_t jetkvm_settings_get_keyboard_layout(char *output,
                                              size_t output_capacity)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!output || output_capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    size_t required = strlen(s_runtime.settings.keyboard_layout) + 1U;
    esp_err_t err = output_capacity >= required ? ESP_OK :
                                                  ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) {
        memcpy(output, s_runtime.settings.keyboard_layout, required);
    }
    xSemaphoreGive(s_runtime.lock);
    return err;
}

esp_err_t jetkvm_settings_set_keyboard_layout(const char *layout)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    jetkvm_settings_t validated = s_runtime.settings;
    esp_err_t err = jetkvm_settings_assign_keyboard_layout(
        &validated, layout, NULL);
    if (err != ESP_OK) {
        xSemaphoreGive(s_runtime.lock);
        return err;
    }
    err = jetkvm_settings_save(&validated);
    if (err == ESP_OK) {
        s_runtime.settings = validated;
        s_runtime.status.persistence_error = ESP_OK;
    } else {
        s_runtime.status.persistence_error = err;
    }
    xSemaphoreGive(s_runtime.lock);
    return err;
}

esp_err_t jetkvm_settings_get_video_quality(double *factor)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!factor) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    *factor = (double)s_runtime.settings.video_quality_permille / 1000.0;
    xSemaphoreGive(s_runtime.lock);
    return ESP_OK;
}

esp_err_t jetkvm_settings_set_video_quality(double factor)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    jetkvm_settings_t validated = s_runtime.settings;
    esp_err_t err = jetkvm_settings_assign_video_quality(&validated, factor);
    if (err == ESP_OK &&
        validated.video_quality_permille ==
            s_runtime.settings.video_quality_permille) {
        xSemaphoreGive(s_runtime.lock);
        return ESP_OK;
    }
    if (err == ESP_OK) {
        err = jetkvm_settings_save(&validated);
    }
    if (err == ESP_OK) {
        s_runtime.settings = validated;
        s_runtime.status.persistence_error = ESP_OK;
    } else if (err != ESP_ERR_INVALID_ARG) {
        s_runtime.status.persistence_error = err;
    }
    xSemaphoreGive(s_runtime.lock);
    return err;
}

esp_err_t jetkvm_settings_get_video_codec_preference(
    char *output, size_t output_capacity)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!output || output_capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    size_t required = strlen(s_runtime.settings.video_codec_preference) + 1U;
    esp_err_t err = output_capacity >= required ? ESP_OK :
                                                  ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) {
        memcpy(output, s_runtime.settings.video_codec_preference, required);
    }
    xSemaphoreGive(s_runtime.lock);
    return err;
}

esp_err_t jetkvm_settings_set_video_codec_preference(const char *codec)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    jetkvm_settings_t validated = s_runtime.settings;
    esp_err_t err = jetkvm_settings_assign_video_codec_preference(
        &validated, codec);
    if (err == ESP_OK && strcmp(validated.video_codec_preference,
                                s_runtime.settings.video_codec_preference) == 0) {
        xSemaphoreGive(s_runtime.lock);
        return ESP_OK;
    }
    if (err == ESP_OK) {
        err = jetkvm_settings_save(&validated);
    }
    if (err == ESP_OK) {
        s_runtime.settings = validated;
        s_runtime.status.persistence_error = ESP_OK;
    } else if (err != ESP_ERR_INVALID_ARG) {
        s_runtime.status.persistence_error = err;
    }
    xSemaphoreGive(s_runtime.lock);
    return err;
}

esp_err_t jetkvm_settings_get_video_edid_preset(
    jetkvm_video_edid_preset_t *preset)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!preset) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    *preset = (jetkvm_video_edid_preset_t)
        s_runtime.settings.video_edid_preset;
    xSemaphoreGive(s_runtime.lock);
    return ESP_OK;
}

esp_err_t jetkvm_settings_set_video_edid_preset(
    jetkvm_video_edid_preset_t preset)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    jetkvm_settings_t validated = s_runtime.settings;
    esp_err_t err = jetkvm_settings_assign_video_edid_preset(
        &validated, preset);
    if (err == ESP_OK && validated.video_edid_preset ==
                         s_runtime.settings.video_edid_preset) {
        xSemaphoreGive(s_runtime.lock);
        return ESP_OK;
    }
    if (err == ESP_OK) {
        err = jetkvm_settings_save(&validated);
    }
    if (err == ESP_OK) {
        s_runtime.settings = validated;
        s_runtime.status.persistence_error = ESP_OK;
    } else if (err != ESP_ERR_INVALID_ARG) {
        s_runtime.status.persistence_error = err;
    }
    xSemaphoreGive(s_runtime.lock);
    return err;
}

esp_err_t jetkvm_settings_get_host_display_idle(bool *enabled)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!enabled) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    *enabled = s_runtime.settings.host_display_idle_enabled != 0U;
    xSemaphoreGive(s_runtime.lock);
    return ESP_OK;
}

esp_err_t jetkvm_settings_set_host_display_idle(bool enabled)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    jetkvm_settings_t validated = s_runtime.settings;
    esp_err_t err = jetkvm_settings_assign_host_display_idle(
        &validated, enabled);
    if (err == ESP_OK && validated.host_display_idle_enabled ==
                         s_runtime.settings.host_display_idle_enabled) {
        xSemaphoreGive(s_runtime.lock);
        return ESP_OK;
    }
    if (err == ESP_OK) {
        err = jetkvm_settings_save(&validated);
    }
    if (err == ESP_OK) {
        s_runtime.settings = validated;
        s_runtime.status.persistence_error = ESP_OK;
    } else if (err != ESP_ERR_INVALID_ARG) {
        s_runtime.status.persistence_error = err;
    }
    xSemaphoreGive(s_runtime.lock);
    return err;
}

esp_err_t jetkvm_settings_get_video_sleep_after(int32_t *duration_seconds)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!duration_seconds) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    *duration_seconds = s_runtime.settings.video_sleep_after_seconds;
    xSemaphoreGive(s_runtime.lock);
    return ESP_OK;
}

esp_err_t jetkvm_settings_set_video_sleep_after(int32_t duration_seconds)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    jetkvm_settings_t validated = s_runtime.settings;
    esp_err_t err = jetkvm_settings_assign_video_sleep_after(
        &validated, duration_seconds);
    if (err == ESP_OK && validated.video_sleep_after_seconds ==
                         s_runtime.settings.video_sleep_after_seconds) {
        xSemaphoreGive(s_runtime.lock);
        return ESP_OK;
    }
    if (err == ESP_OK) {
        err = jetkvm_settings_save(&validated);
    }
    if (err == ESP_OK) {
        s_runtime.settings = validated;
        s_runtime.status.persistence_error = ESP_OK;
    } else if (err != ESP_ERR_INVALID_ARG) {
        s_runtime.status.persistence_error = err;
    }
    xSemaphoreGive(s_runtime.lock);
    return err;
}

esp_err_t jetkvm_settings_get_status(jetkvm_settings_status_t *status)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_runtime.lock, portMAX_DELAY);
    *status = s_runtime.status;
    xSemaphoreGive(s_runtime.lock);
    return ESP_OK;
}

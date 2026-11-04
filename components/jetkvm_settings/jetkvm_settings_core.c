/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_settings.h"

#include <string.h>

static size_t bounded_length(const char *value, size_t capacity)
{
    if (!value) {
        return capacity;
    }
    size_t length = 0U;
    while (length < capacity && value[length] != '\0') {
        ++length;
    }
    return length;
}

static bool ascii_alphanumeric(unsigned char value)
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

void jetkvm_settings_defaults(jetkvm_settings_t *settings)
{
    if (!settings) {
        return;
    }
    memset(settings, 0, sizeof(*settings));
    settings->schema_version = JETKVM_SETTINGS_SCHEMA_VERSION;
    settings->video_quality_permille =
        JETKVM_DEFAULT_VIDEO_QUALITY_PERMILLE;
    memcpy(settings->keyboard_layout, JETKVM_DEFAULT_KEYBOARD_LAYOUT,
           sizeof(JETKVM_DEFAULT_KEYBOARD_LAYOUT));
    memcpy(settings->video_codec_preference,
           JETKVM_DEFAULT_VIDEO_CODEC_PREFERENCE,
           sizeof(JETKVM_DEFAULT_VIDEO_CODEC_PREFERENCE));
    settings->video_edid_preset = JETKVM_DEFAULT_VIDEO_EDID_PRESET;
    settings->host_display_idle_enabled = 0U;
    settings->video_sleep_after_seconds =
        JETKVM_DEFAULT_VIDEO_SLEEP_AFTER_SECONDS;
}

esp_err_t jetkvm_settings_assign_video_sleep_after(
    jetkvm_settings_t *settings, int32_t duration_seconds)
{
    if (!settings || duration_seconds < -1) {
        return ESP_ERR_INVALID_ARG;
    }
    settings->video_sleep_after_seconds = duration_seconds;
    settings->schema_version = JETKVM_SETTINGS_SCHEMA_VERSION;
    return ESP_OK;
}

esp_err_t jetkvm_settings_assign_host_display_idle(
    jetkvm_settings_t *settings, bool enabled)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    settings->host_display_idle_enabled = enabled ? 1U : 0U;
    settings->schema_version = JETKVM_SETTINGS_SCHEMA_VERSION;
    return ESP_OK;
}

esp_err_t jetkvm_settings_assign_video_edid_preset(
    jetkvm_settings_t *settings, jetkvm_video_edid_preset_t preset)
{
    if (!settings ||
        (preset != JETKVM_VIDEO_EDID_PRESET_DEFAULT &&
         preset != JETKVM_VIDEO_EDID_PRESET_DELL_D2721H)) {
        return ESP_ERR_INVALID_ARG;
    }
    settings->video_edid_preset = (uint32_t)preset;
    settings->schema_version = JETKVM_SETTINGS_SCHEMA_VERSION;
    return ESP_OK;
}

esp_err_t jetkvm_settings_assign_video_codec_preference(
    jetkvm_settings_t *settings, const char *codec)
{
    if (!settings || !codec ||
        (strcmp(codec, "auto") != 0 && strcmp(codec, "h264") != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(settings->video_codec_preference, 0,
           sizeof(settings->video_codec_preference));
    memcpy(settings->video_codec_preference, codec, strlen(codec) + 1U);
    settings->schema_version = JETKVM_SETTINGS_SCHEMA_VERSION;
    return ESP_OK;
}

esp_err_t jetkvm_settings_assign_video_quality(
    jetkvm_settings_t *settings, double factor)
{
    if (!settings || factor != factor ||
        (factor != 0.0 && (factor < 0.1 || factor > 1.0))) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t permille = (uint32_t)(factor * 1000.0 + 0.5);
    if (!jetkvm_settings_video_quality_permille_valid(permille)) {
        return ESP_ERR_INVALID_ARG;
    }
    settings->video_quality_permille = permille;
    settings->schema_version = JETKVM_SETTINGS_SCHEMA_VERSION;
    return ESP_OK;
}

bool jetkvm_settings_video_quality_permille_valid(uint32_t permille)
{
    return permille == JETKVM_AUTO_VIDEO_QUALITY_PERMILLE ||
           (permille >= JETKVM_MIN_VIDEO_QUALITY_PERMILLE &&
            permille <= JETKVM_MAX_VIDEO_QUALITY_PERMILLE);
}

bool jetkvm_settings_keyboard_layout_valid(const char *layout)
{
    size_t length = bounded_length(layout, JETKVM_KEYBOARD_LAYOUT_CAPACITY);
    if (length < 2U || length >= JETKVM_KEYBOARD_LAYOUT_CAPACITY ||
        !ascii_alphanumeric((unsigned char)layout[0]) ||
        !ascii_alphanumeric((unsigned char)layout[length - 1U])) {
        return false;
    }

    bool previous_separator = false;
    for (size_t i = 0U; i < length; ++i) {
        unsigned char value = (unsigned char)layout[i];
        if (ascii_alphanumeric(value)) {
            previous_separator = false;
        } else if (value == '-' && !previous_separator) {
            previous_separator = true;
        } else {
            return false;
        }
    }
    return true;
}

esp_err_t jetkvm_settings_assign_keyboard_layout(
    jetkvm_settings_t *settings, const char *layout, bool *legacy_migrated)
{
    if (!settings || !layout) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *canonical = layout;
    bool migrated = strcmp(layout, JETKVM_LEGACY_KEYBOARD_LAYOUT) == 0;
    if (migrated) {
        canonical = JETKVM_DEFAULT_KEYBOARD_LAYOUT;
    } else if (!jetkvm_settings_keyboard_layout_valid(layout)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t length = strlen(canonical);
    memset(settings->keyboard_layout, 0, sizeof(settings->keyboard_layout));
    memcpy(settings->keyboard_layout, canonical, length + 1U);
    settings->schema_version = JETKVM_SETTINGS_SCHEMA_VERSION;
    if (legacy_migrated) {
        *legacy_migrated = migrated;
    }
    return ESP_OK;
}

bool jetkvm_settings_validate(const jetkvm_settings_t *settings)
{
    return settings &&
           settings->schema_version == JETKVM_SETTINGS_SCHEMA_VERSION &&
           jetkvm_settings_keyboard_layout_valid(settings->keyboard_layout) &&
           jetkvm_settings_video_quality_permille_valid(
               settings->video_quality_permille) &&
           (strcmp(settings->video_codec_preference, "auto") == 0 ||
            strcmp(settings->video_codec_preference, "h264") == 0) &&
           (settings->video_edid_preset ==
                JETKVM_VIDEO_EDID_PRESET_DEFAULT ||
            settings->video_edid_preset ==
                JETKVM_VIDEO_EDID_PRESET_DELL_D2721H) &&
           settings->host_display_idle_enabled <= 1U &&
           settings->video_sleep_after_seconds >= -1;
}

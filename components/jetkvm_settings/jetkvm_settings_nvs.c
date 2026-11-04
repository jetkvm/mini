/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_settings.h"

#include <stddef.h>
#include <string.h>

#include "nvs.h"

#define SETTINGS_NVS_NAMESPACE "jkvm_settings"
#define SETTINGS_NVS_RECORD_KEY "settings"
#define SETTINGS_NVS_RECORD_MAGIC 0x54534b4aU /* "JKST" little endian */

typedef struct {
    uint32_t magic;
    uint32_t schema_version;
    char keyboard_layout[JETKVM_KEYBOARD_LAYOUT_CAPACITY];
    uint32_t video_quality_permille;
    char video_codec_preference[JETKVM_VIDEO_CODEC_PREFERENCE_CAPACITY];
    uint32_t video_edid_preset;
    uint32_t host_display_idle_enabled;
    int32_t video_sleep_after_seconds;
    uint32_t crc32;
} settings_nvs_record_t;

typedef struct {
    uint32_t magic;
    uint32_t schema_version;
    char keyboard_layout[JETKVM_KEYBOARD_LAYOUT_CAPACITY];
    uint32_t video_quality_permille;
    char video_codec_preference[JETKVM_VIDEO_CODEC_PREFERENCE_CAPACITY];
    uint32_t video_edid_preset;
    uint32_t host_display_idle_enabled;
    uint32_t crc32;
} settings_nvs_record_v5_t;

typedef struct {
    uint32_t magic;
    uint32_t schema_version;
    char keyboard_layout[JETKVM_KEYBOARD_LAYOUT_CAPACITY];
    uint32_t video_quality_permille;
    char video_codec_preference[JETKVM_VIDEO_CODEC_PREFERENCE_CAPACITY];
    uint32_t video_edid_preset;
    uint32_t crc32;
} settings_nvs_record_v4_t;

typedef struct {
    uint32_t magic;
    uint32_t schema_version;
    char keyboard_layout[JETKVM_KEYBOARD_LAYOUT_CAPACITY];
    uint32_t video_quality_permille;
    char video_codec_preference[JETKVM_VIDEO_CODEC_PREFERENCE_CAPACITY];
    uint32_t crc32;
} settings_nvs_record_v3_t;

typedef struct {
    uint32_t magic;
    uint32_t schema_version;
    char keyboard_layout[JETKVM_KEYBOARD_LAYOUT_CAPACITY];
    uint32_t video_quality_permille;
    uint32_t crc32;
} settings_nvs_record_v2_t;

typedef struct {
    uint32_t magic;
    uint32_t schema_version;
    char keyboard_layout[JETKVM_KEYBOARD_LAYOUT_CAPACITY];
    uint32_t crc32;
} settings_nvs_record_v1_t;

_Static_assert(offsetof(settings_nvs_record_t, crc32) + sizeof(uint32_t) ==
                   sizeof(settings_nvs_record_t),
               "settings record CRC must be the final field");

static uint32_t record_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0U; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

static esp_err_t assign_record_layout(jetkvm_settings_t *settings,
                                      const char *record_layout,
                                      bool *migration_required)
{
    char layout[JETKVM_KEYBOARD_LAYOUT_CAPACITY];
    memcpy(layout, record_layout, sizeof(layout));
    if (!memchr(layout, '\0', sizeof(layout))) {
        return ESP_ERR_INVALID_STATE;
    }
    bool layout_migrated = false;
    esp_err_t err = jetkvm_settings_assign_keyboard_layout(
        settings, layout, &layout_migrated);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (layout_migrated && migration_required) {
        *migration_required = true;
    }
    return ESP_OK;
}

static esp_err_t record_to_settings(const settings_nvs_record_t *record,
                                    jetkvm_settings_t *settings,
                                    bool *migration_required)
{
    if (record->magic != SETTINGS_NVS_RECORD_MAGIC) {
        return ESP_ERR_INVALID_STATE;
    }
    if (record->schema_version != JETKVM_SETTINGS_SCHEMA_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    uint32_t expected = record_crc32(record,
                                     offsetof(settings_nvs_record_t, crc32));
    if (record->crc32 != expected) {
        return ESP_ERR_INVALID_CRC;
    }

    esp_err_t err = assign_record_layout(
        settings, record->keyboard_layout, migration_required);
    if (err != ESP_OK ||
        !jetkvm_settings_video_quality_permille_valid(
            record->video_quality_permille)) {
        return ESP_ERR_INVALID_STATE;
    }
    settings->video_quality_permille = record->video_quality_permille;
    err = jetkvm_settings_assign_video_codec_preference(
        settings, record->video_codec_preference);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    err = jetkvm_settings_assign_video_edid_preset(
        settings, (jetkvm_video_edid_preset_t)record->video_edid_preset);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (record->host_display_idle_enabled > 1U) {
        return ESP_ERR_INVALID_STATE;
    }
    err = jetkvm_settings_assign_host_display_idle(
        settings, record->host_display_idle_enabled != 0U);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    return jetkvm_settings_assign_video_sleep_after(
        settings, record->video_sleep_after_seconds);
}

static esp_err_t record_v5_to_settings(const settings_nvs_record_v5_t *record,
                                       jetkvm_settings_t *settings,
                                       bool *migration_required)
{
    if (record->magic != SETTINGS_NVS_RECORD_MAGIC ||
        record->schema_version != 5U) {
        return ESP_ERR_INVALID_VERSION;
    }
    uint32_t expected = record_crc32(
        record, offsetof(settings_nvs_record_v5_t, crc32));
    if (record->crc32 != expected) {
        return ESP_ERR_INVALID_CRC;
    }
    esp_err_t err = assign_record_layout(
        settings, record->keyboard_layout, migration_required);
    if (err != ESP_OK ||
        !jetkvm_settings_video_quality_permille_valid(
            record->video_quality_permille)) {
        return ESP_ERR_INVALID_STATE;
    }
    settings->video_quality_permille = record->video_quality_permille;
    err = jetkvm_settings_assign_video_codec_preference(
        settings, record->video_codec_preference);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    err = jetkvm_settings_assign_video_edid_preset(
        settings, (jetkvm_video_edid_preset_t)record->video_edid_preset);
    if (err != ESP_OK || record->host_display_idle_enabled > 1U) {
        return ESP_ERR_INVALID_STATE;
    }
    err = jetkvm_settings_assign_host_display_idle(
        settings, record->host_display_idle_enabled != 0U);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (migration_required) {
        *migration_required = true;
    }
    return ESP_OK;
}

static esp_err_t record_v4_to_settings(const settings_nvs_record_v4_t *record,
                                       jetkvm_settings_t *settings,
                                       bool *migration_required)
{
    if (record->magic != SETTINGS_NVS_RECORD_MAGIC ||
        record->schema_version != 4U) {
        return ESP_ERR_INVALID_VERSION;
    }
    uint32_t expected = record_crc32(
        record, offsetof(settings_nvs_record_v4_t, crc32));
    if (record->crc32 != expected) {
        return ESP_ERR_INVALID_CRC;
    }
    esp_err_t err = assign_record_layout(
        settings, record->keyboard_layout, migration_required);
    if (err != ESP_OK ||
        !jetkvm_settings_video_quality_permille_valid(
            record->video_quality_permille)) {
        return ESP_ERR_INVALID_STATE;
    }
    settings->video_quality_permille = record->video_quality_permille;
    err = jetkvm_settings_assign_video_codec_preference(
        settings, record->video_codec_preference);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    err = jetkvm_settings_assign_video_edid_preset(
        settings, (jetkvm_video_edid_preset_t)record->video_edid_preset);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (migration_required) {
        *migration_required = true;
    }
    return ESP_OK;
}

static esp_err_t record_v3_to_settings(const settings_nvs_record_v3_t *record,
                                       jetkvm_settings_t *settings,
                                       bool *migration_required)
{
    if (record->magic != SETTINGS_NVS_RECORD_MAGIC ||
        record->schema_version != 3U) {
        return ESP_ERR_INVALID_VERSION;
    }
    uint32_t expected = record_crc32(
        record, offsetof(settings_nvs_record_v3_t, crc32));
    if (record->crc32 != expected) {
        return ESP_ERR_INVALID_CRC;
    }
    esp_err_t err = assign_record_layout(
        settings, record->keyboard_layout, migration_required);
    if (err != ESP_OK ||
        !jetkvm_settings_video_quality_permille_valid(
            record->video_quality_permille)) {
        return ESP_ERR_INVALID_STATE;
    }
    settings->video_quality_permille = record->video_quality_permille;
    err = jetkvm_settings_assign_video_codec_preference(
        settings, record->video_codec_preference);
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (migration_required) {
        *migration_required = true;
    }
    return ESP_OK;
}

static esp_err_t record_v2_to_settings(const settings_nvs_record_v2_t *record,
                                       jetkvm_settings_t *settings,
                                       bool *migration_required)
{
    if (record->magic != SETTINGS_NVS_RECORD_MAGIC ||
        record->schema_version != 2U) {
        return ESP_ERR_INVALID_VERSION;
    }
    uint32_t expected = record_crc32(
        record, offsetof(settings_nvs_record_v2_t, crc32));
    if (record->crc32 != expected) {
        return ESP_ERR_INVALID_CRC;
    }
    esp_err_t err = assign_record_layout(
        settings, record->keyboard_layout, migration_required);
    if (err != ESP_OK ||
        !jetkvm_settings_video_quality_permille_valid(
            record->video_quality_permille)) {
        return ESP_ERR_INVALID_STATE;
    }
    settings->video_quality_permille = record->video_quality_permille;
    if (migration_required) {
        *migration_required = true;
    }
    return ESP_OK;
}

static esp_err_t record_v1_to_settings(const settings_nvs_record_v1_t *record,
                                       jetkvm_settings_t *settings,
                                       bool *migration_required)
{
    if (record->magic != SETTINGS_NVS_RECORD_MAGIC ||
        record->schema_version != 1U) {
        return ESP_ERR_INVALID_VERSION;
    }
    uint32_t expected = record_crc32(
        record, offsetof(settings_nvs_record_v1_t, crc32));
    if (record->crc32 != expected) {
        return ESP_ERR_INVALID_CRC;
    }
    esp_err_t err = assign_record_layout(
        settings, record->keyboard_layout, migration_required);
    if (err == ESP_OK && migration_required) {
        *migration_required = true;
    }
    return err;
}

esp_err_t jetkvm_settings_load(jetkvm_settings_t *settings,
                               bool *migration_required)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    jetkvm_settings_defaults(settings);
    if (migration_required) {
        *migration_required = false;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t record_size = 0U;
    err = nvs_get_blob(handle, SETTINGS_NVS_RECORD_KEY, NULL, &record_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    union {
        settings_nvs_record_t current;
        settings_nvs_record_v5_t v5;
        settings_nvs_record_v4_t v4;
        settings_nvs_record_v3_t v3;
        settings_nvs_record_v2_t v2;
        settings_nvs_record_v1_t v1;
    } record;
    if (record_size != sizeof(record.current) &&
        record_size != sizeof(record.v5) &&
        record_size != sizeof(record.v4) &&
        record_size != sizeof(record.v3) &&
        record_size != sizeof(record.v2) &&
        record_size != sizeof(record.v1)) {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }
    err = nvs_get_blob(handle, SETTINGS_NVS_RECORD_KEY, &record, &record_size);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (record_size == sizeof(record.current)) {
        err = record_to_settings(&record.current, settings,
                                 migration_required);
    } else if (record_size == sizeof(record.v5)) {
        err = record_v5_to_settings(&record.v5, settings,
                                    migration_required);
    } else if (record_size == sizeof(record.v4)) {
        err = record_v4_to_settings(&record.v4, settings,
                                    migration_required);
    } else if (record_size == sizeof(record.v3)) {
        err = record_v3_to_settings(&record.v3, settings,
                                    migration_required);
    } else if (record_size == sizeof(record.v2)) {
        err = record_v2_to_settings(&record.v2, settings,
                                    migration_required);
    } else {
        err = record_v1_to_settings(&record.v1, settings,
                                    migration_required);
    }
    if (err != ESP_OK) {
        jetkvm_settings_defaults(settings);
        if (migration_required) {
            *migration_required = false;
        }
    }
    return err;
}

esp_err_t jetkvm_settings_save(const jetkvm_settings_t *settings)
{
    if (!jetkvm_settings_validate(settings)) {
        return ESP_ERR_INVALID_ARG;
    }

    settings_nvs_record_t record = {
        .magic = SETTINGS_NVS_RECORD_MAGIC,
        .schema_version = JETKVM_SETTINGS_SCHEMA_VERSION,
    };
    memcpy(record.keyboard_layout, settings->keyboard_layout,
           sizeof(record.keyboard_layout));
    record.video_quality_permille = settings->video_quality_permille;
    memcpy(record.video_codec_preference, settings->video_codec_preference,
           sizeof(record.video_codec_preference));
    record.video_edid_preset = settings->video_edid_preset;
    record.host_display_idle_enabled =
        settings->host_display_idle_enabled;
    record.video_sleep_after_seconds =
        settings->video_sleep_after_seconds;
    record.crc32 = record_crc32(&record,
                                offsetof(settings_nvs_record_t, crc32));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, SETTINGS_NVS_RECORD_KEY,
                       &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

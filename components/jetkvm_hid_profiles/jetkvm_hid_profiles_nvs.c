/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_hid_profiles.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nvs.h"

#define HID_PROFILES_NAMESPACE "jkvm_hidprof"
#define HID_PROFILES_KEY "profiles"
#define HID_PROFILES_MAGIC 0x50484b4aU /* "JKHP" little endian */

typedef struct {
    uint32_t magic;
    uint32_t schema_version;
    uint32_t macro_count;
    jetkvm_control_keyboard_macro_t
        macros[JETKVM_CONTROL_KEYBOARD_MACRO_MAX_COUNT];
    uint8_t jiggler_enabled;
    uint8_t reserved[3];
    jetkvm_control_jiggler_config_t jiggler_config;
    uint32_t crc32;
} hid_profiles_record_t;

_Static_assert(offsetof(hid_profiles_record_t, crc32) + sizeof(uint32_t) ==
                   sizeof(hid_profiles_record_t),
               "HID profile record CRC must be the final field");

static uint32_t record_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
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

static bool record_valid(const hid_profiles_record_t *record)
{
    if (record == NULL || record->magic != HID_PROFILES_MAGIC ||
        record->schema_version != JETKVM_HID_PROFILES_SCHEMA_VERSION ||
        record->macro_count > JETKVM_CONTROL_KEYBOARD_MACRO_MAX_COUNT ||
        record->jiggler_enabled > 1U ||
        record->crc32 !=
            record_crc32(record, offsetof(hid_profiles_record_t, crc32))) {
        return false;
    }
    jetkvm_control_keyboard_macro_list_t *view = calloc(1U, sizeof(*view));
    if (view == NULL) {
        return false;
    }
    view->count = record->macro_count;
    memcpy(view->macros, record->macros,
           record->macro_count * sizeof(record->macros[0]));
    bool valid = jetkvm_hid_profiles_macros_valid(view) &&
                 jetkvm_hid_profiles_jiggler_config_valid(
                     &record->jiggler_config);
    free(view);
    return valid;
}

static esp_err_t open_profiles(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open(HID_PROFILES_NAMESPACE, mode, handle);
}

static esp_err_t load_from_location(
    jetkvm_control_keyboard_macro_list_t *macros,
    bool *jiggler_enabled,
    jetkvm_control_jiggler_config_t *jiggler_config)
{
    nvs_handle_t handle;
    esp_err_t error = open_profiles(NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
    }
    size_t size = 0U;
    error = nvs_get_blob(handle, HID_PROFILES_KEY, NULL, &size);
    if (error != ESP_OK) {
        nvs_close(handle);
        return error;
    }
    if (size != sizeof(hid_profiles_record_t)) {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }
    hid_profiles_record_t *record = calloc(1U, sizeof(*record));
    if (record == NULL) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }
    error = nvs_get_blob(handle, HID_PROFILES_KEY, record, &size);
    nvs_close(handle);
    if (error == ESP_OK && !record_valid(record)) {
        error = record->magic == HID_PROFILES_MAGIC &&
                        record->schema_version !=
                            JETKVM_HID_PROFILES_SCHEMA_VERSION
                    ? ESP_ERR_INVALID_VERSION
                    : ESP_ERR_INVALID_CRC;
    }
    if (error == ESP_OK) {
        macros->count = record->macro_count;
        memcpy(macros->macros, record->macros,
               macros->count * sizeof(macros->macros[0]));
        *jiggler_enabled = record->jiggler_enabled != 0U;
        *jiggler_config = record->jiggler_config;
    }
    free(record);
    return error;
}

esp_err_t jetkvm_hid_profiles_load(
    jetkvm_control_keyboard_macro_list_t *macros, bool *jiggler_enabled,
    jetkvm_control_jiggler_config_t *jiggler_config)
{
    if (macros == NULL || jiggler_enabled == NULL ||
        jiggler_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    jetkvm_hid_profiles_defaults(macros, jiggler_enabled, jiggler_config);

    esp_err_t error = load_from_location(macros, jiggler_enabled,
                                         jiggler_config);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return error;
}

esp_err_t jetkvm_hid_profiles_save(
    const jetkvm_control_keyboard_macro_list_t *macros,
    bool jiggler_enabled,
    const jetkvm_control_jiggler_config_t *jiggler_config)
{
    if (!jetkvm_hid_profiles_macros_valid(macros) ||
        !jetkvm_hid_profiles_jiggler_config_valid(jiggler_config)) {
        return ESP_ERR_INVALID_ARG;
    }
    hid_profiles_record_t *record = calloc(1U, sizeof(*record));
    if (record == NULL) {
        return ESP_ERR_NO_MEM;
    }
    record->magic = HID_PROFILES_MAGIC;
    record->schema_version = JETKVM_HID_PROFILES_SCHEMA_VERSION;
    record->macro_count = (uint32_t)macros->count;
    memcpy(record->macros, macros->macros,
           macros->count * sizeof(macros->macros[0]));
    record->jiggler_enabled = jiggler_enabled ? 1U : 0U;
    record->jiggler_config = *jiggler_config;
    record->crc32 = record_crc32(
        record, offsetof(hid_profiles_record_t, crc32));

    nvs_handle_t handle;
    esp_err_t error = open_profiles(NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, HID_PROFILES_KEY, record,
                             sizeof(*record));
        if (error == ESP_OK) {
            error = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    free(record);
    return error;
}

/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_usb_config.h"

#include <stddef.h>
#include <string.h>

#include "nvs.h"

#define USB_CONFIG_NVS_NAMESPACE "jkvm_usb"
#define USB_CONFIG_NVS_SLOT_A_KEY "cfg_a"
#define USB_CONFIG_NVS_SLOT_B_KEY "cfg_b"
#define USB_CONFIG_RECORD_MAGIC 0x43554b4aU /* "JKUC" little endian */

typedef struct {
    uint32_t magic;
    uint16_t record_version;
    uint16_t record_size;
    uint32_t generation;
    uint32_t config_schema_version;
    uint16_t vendor_id;
    uint16_t product_id;
    char serial_number[JETKVM_USB_STRING_CAPACITY];
    char manufacturer[JETKVM_USB_STRING_CAPACITY];
    char product[JETKVM_USB_STRING_CAPACITY];
    uint8_t keyboard;
    uint8_t absolute_mouse;
    uint8_t relative_mouse;
    uint8_t mass_storage;
    uint8_t serial_console;
    uint8_t audio;
    uint8_t reserved[2];
    uint32_t crc32;
} usb_config_nvs_record_t;

_Static_assert(sizeof(usb_config_nvs_record_t) == 128U,
               "USB configuration record layout changed");
_Static_assert(offsetof(usb_config_nvs_record_t, generation) == 8U &&
                   offsetof(usb_config_nvs_record_t, config_schema_version) ==
                       12U &&
                   offsetof(usb_config_nvs_record_t, serial_number) == 20U &&
                   offsetof(usb_config_nvs_record_t, keyboard) == 116U &&
                   offsetof(usb_config_nvs_record_t, crc32) == 124U,
               "USB configuration persistence ABI changed");
_Static_assert(offsetof(usb_config_nvs_record_t, crc32) + sizeof(uint32_t) ==
                   sizeof(usb_config_nvs_record_t),
               "USB configuration record CRC must be the final field");

typedef struct {
    bool valid;
    esp_err_t error;
    usb_config_nvs_record_t record;
    jetkvm_usb_config_t config;
} slot_result_t;

static uint32_t record_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0U; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

static bool record_flag_valid(uint8_t value)
{
    return value == 0U || value == 1U;
}

static void record_to_config(const usb_config_nvs_record_t *record,
                             jetkvm_usb_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->schema_version = record->config_schema_version;
    config->vendor_id = record->vendor_id;
    config->product_id = record->product_id;
    memcpy(config->serial_number, record->serial_number,
           sizeof(config->serial_number));
    memcpy(config->manufacturer, record->manufacturer,
           sizeof(config->manufacturer));
    memcpy(config->product, record->product, sizeof(config->product));
    config->keyboard = record->keyboard == 1U;
    config->absolute_mouse = record->absolute_mouse == 1U;
    config->relative_mouse = record->relative_mouse == 1U;
    config->mass_storage = record->mass_storage == 1U;
    config->serial_console = record->serial_console == 1U;
    config->audio = record->audio == 1U;
}

static esp_err_t decode_record(const usb_config_nvs_record_t *record,
                               jetkvm_usb_config_t *config)
{
    if (record->magic != USB_CONFIG_RECORD_MAGIC) {
        return ESP_ERR_INVALID_STATE;
    }
    if (record->record_version != JETKVM_USB_CONFIG_RECORD_VERSION ||
        record->config_schema_version != JETKVM_USB_CONFIG_SCHEMA_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    if (record->record_size != sizeof(*record)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (record->generation == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t expected = record_crc32(record,
                                     offsetof(usb_config_nvs_record_t, crc32));
    if (record->crc32 != expected) {
        return ESP_ERR_INVALID_CRC;
    }
    if (record->reserved[0] != 0U || record->reserved[1] != 0U ||
        !record_flag_valid(record->keyboard) ||
        !record_flag_valid(record->absolute_mouse) ||
        !record_flag_valid(record->relative_mouse) ||
        !record_flag_valid(record->mass_storage) ||
        !record_flag_valid(record->serial_console) ||
        !record_flag_valid(record->audio)) {
        return ESP_ERR_INVALID_STATE;
    }

    jetkvm_usb_config_t decoded;
    record_to_config(record, &decoded);
    if (!jetkvm_usb_config_validate(&decoded)) {
        return ESP_ERR_INVALID_STATE;
    }
    *config = decoded;
    return ESP_OK;
}

static void copy_record_string(
    char destination[JETKVM_USB_STRING_CAPACITY], const char *source)
{
    size_t length = strlen(source);
    memcpy(destination, source, length + 1U);
}

static void config_to_record(const jetkvm_usb_config_t *config,
                             uint32_t generation,
                             usb_config_nvs_record_t *record)
{
    memset(record, 0, sizeof(*record));
    record->magic = USB_CONFIG_RECORD_MAGIC;
    record->record_version = JETKVM_USB_CONFIG_RECORD_VERSION;
    record->record_size = (uint16_t)sizeof(*record);
    record->generation = generation;
    record->config_schema_version = JETKVM_USB_CONFIG_SCHEMA_VERSION;
    record->vendor_id = config->vendor_id;
    record->product_id = config->product_id;
    copy_record_string(record->serial_number, config->serial_number);
    copy_record_string(record->manufacturer, config->manufacturer);
    copy_record_string(record->product, config->product);
    record->keyboard = config->keyboard ? 1U : 0U;
    record->absolute_mouse = config->absolute_mouse ? 1U : 0U;
    record->relative_mouse = config->relative_mouse ? 1U : 0U;
    record->mass_storage = config->mass_storage ? 1U : 0U;
    record->serial_console = config->serial_console ? 1U : 0U;
    record->audio = config->audio ? 1U : 0U;
    record->crc32 = record_crc32(record,
                                 offsetof(usb_config_nvs_record_t, crc32));
}

static slot_result_t read_slot(nvs_handle_t handle, const char *key)
{
    slot_result_t result = {
        .valid = false,
        .error = ESP_ERR_NVS_NOT_FOUND,
    };
    size_t record_size = 0U;
    esp_err_t err = nvs_get_blob(handle, key, NULL, &record_size);
    if (err != ESP_OK) {
        result.error = err;
        return result;
    }
    if (record_size != sizeof(result.record)) {
        result.error = ESP_ERR_INVALID_SIZE;
        return result;
    }

    err = nvs_get_blob(handle, key, &result.record, &record_size);
    if (err != ESP_OK) {
        result.error = err;
        return result;
    }
    if (record_size != sizeof(result.record)) {
        result.error = ESP_ERR_INVALID_SIZE;
        return result;
    }
    err = decode_record(&result.record, &result.config);
    if (err != ESP_OK) {
        result.error = err;
        return result;
    }
    result.valid = true;
    result.error = ESP_OK;
    return result;
}

static void state_defaults(jetkvm_usb_config_store_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->active_slot = JETKVM_USB_CONFIG_SLOT_NONE;
    state->defaults_used = true;
    state->slot_a_error = ESP_ERR_NVS_NOT_FOUND;
    state->slot_b_error = ESP_ERR_NVS_NOT_FOUND;
}

static bool slot_rejected(esp_err_t error)
{
    return error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t jetkvm_usb_config_load(
    jetkvm_usb_config_t *config,
    jetkvm_usb_config_store_state_t *state)
{
    if (!config || !state) {
        return ESP_ERR_INVALID_ARG;
    }
    jetkvm_usb_config_defaults(config);
    state_defaults(state);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(USB_CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    slot_result_t slot_a = read_slot(handle, USB_CONFIG_NVS_SLOT_A_KEY);
    slot_result_t slot_b = read_slot(handle, USB_CONFIG_NVS_SLOT_B_KEY);
    nvs_close(handle);

    state->slot_a_error = slot_a.error;
    state->slot_b_error = slot_b.error;
    state->degraded = slot_rejected(slot_a.error) ||
                      slot_rejected(slot_b.error);

    if (!slot_a.valid && !slot_b.valid) {
        return ESP_OK;
    }

    const slot_result_t *selected = &slot_a;
    jetkvm_usb_config_slot_t selected_slot = JETKVM_USB_CONFIG_SLOT_A;
    if (!slot_a.valid ||
        (slot_b.valid && jetkvm_usb_config_generation_is_newer(
                             slot_b.record.generation,
                             slot_a.record.generation))) {
        selected = &slot_b;
        selected_slot = JETKVM_USB_CONFIG_SLOT_B;
    }

    *config = selected->config;
    (void)jetkvm_usb_config_migrate_default_identity(config);
    state->active_slot = selected_slot;
    state->generation = selected->record.generation;
    state->defaults_used = false;
    return ESP_OK;
}

static bool store_state_valid(const jetkvm_usb_config_store_state_t *state)
{
    if (!state) {
        return false;
    }
    if (state->active_slot == JETKVM_USB_CONFIG_SLOT_NONE) {
        return state->generation == 0U;
    }
    return (state->active_slot == JETKVM_USB_CONFIG_SLOT_A ||
            state->active_slot == JETKVM_USB_CONFIG_SLOT_B) &&
           state->generation != 0U;
}

esp_err_t jetkvm_usb_config_save(
    const jetkvm_usb_config_t *config,
    jetkvm_usb_config_store_state_t *state)
{
    if (!jetkvm_usb_config_validate(config) || !store_state_valid(state)) {
        return ESP_ERR_INVALID_ARG;
    }

    jetkvm_usb_config_slot_t target_slot =
        state->active_slot == JETKVM_USB_CONFIG_SLOT_A ?
            JETKVM_USB_CONFIG_SLOT_B : JETKVM_USB_CONFIG_SLOT_A;
    const char *target_key = target_slot == JETKVM_USB_CONFIG_SLOT_A ?
                                 USB_CONFIG_NVS_SLOT_A_KEY :
                                 USB_CONFIG_NVS_SLOT_B_KEY;
    uint32_t next_generation =
        jetkvm_usb_config_generation_next(state->generation);

    usb_config_nvs_record_t record;
    config_to_record(config, next_generation, &record);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(USB_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, target_key, &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    slot_result_t verified = read_slot(handle, target_key);
    nvs_close(handle);
    if (!verified.valid) {
        return verified.error;
    }
    if (verified.record.generation != next_generation ||
        !jetkvm_usb_config_equal(&verified.config, config)) {
        return ESP_ERR_INVALID_STATE;
    }

    jetkvm_usb_config_store_state_t updated = *state;
    updated.active_slot = target_slot;
    updated.generation = next_generation;
    updated.defaults_used = false;
    if (target_slot == JETKVM_USB_CONFIG_SLOT_A) {
        updated.slot_a_error = ESP_OK;
    } else {
        updated.slot_b_error = ESP_OK;
    }
    updated.degraded = slot_rejected(updated.slot_a_error) ||
                       slot_rejected(updated.slot_b_error);
    *state = updated;
    return ESP_OK;
}

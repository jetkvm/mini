/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_mqtt_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nvs.h"

#define MQTT_NAMESPACE "jkvm_mqtt"
#define MQTT_KEY "config"
#define MQTT_RECORD_MAGIC 0x54514d4aU
#define MQTT_RECORD_VERSION 1U

typedef struct {
    uint32_t magic;
    uint32_t version;
    jetkvm_mqtt_config_t config;
    uint32_t crc32;
} mqtt_record_t;

static uint32_t crc32(const void *data, size_t size)
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

esp_err_t jetkvm_mqtt_store_load(jetkvm_mqtt_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    jetkvm_mqtt_config_defaults(config);
    mqtt_record_t record = {0};
    size_t size = sizeof(record);
    nvs_handle_t handle;
    esp_err_t error = nvs_open(MQTT_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_OK) {
        error = nvs_get_blob(handle, MQTT_KEY, &record, &size);
        nvs_close(handle);
    }
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        jetkvm_mqtt_secure_zero(&record, sizeof(record));
        return ESP_OK;
    }
    if (error != ESP_OK) {
        jetkvm_mqtt_secure_zero(&record, sizeof(record));
        return error;
    }
    if (size != sizeof(record)) {
        jetkvm_mqtt_secure_zero(&record, sizeof(record));
        return ESP_ERR_INVALID_SIZE;
    }
    if (record.magic != MQTT_RECORD_MAGIC ||
        record.version != MQTT_RECORD_VERSION ||
        record.crc32 != crc32(&record, offsetof(mqtt_record_t, crc32))) {
        jetkvm_mqtt_secure_zero(&record, sizeof(record));
        return ESP_ERR_INVALID_CRC;
    }
    if (!jetkvm_mqtt_config_validate(&record.config, NULL, 0U)) {
        jetkvm_mqtt_secure_zero(&record, sizeof(record));
        return ESP_ERR_INVALID_STATE;
    }
    *config = record.config;
    jetkvm_mqtt_secure_zero(&record, sizeof(record));
    return ESP_OK;
}

esp_err_t jetkvm_mqtt_store_save(const jetkvm_mqtt_config_t *config)
{
    if (!jetkvm_mqtt_config_validate(config, NULL, 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    mqtt_record_t record = {
        .magic = MQTT_RECORD_MAGIC,
        .version = MQTT_RECORD_VERSION,
        .config = *config,
    };
    record.crc32 = crc32(&record, offsetof(mqtt_record_t, crc32));
    nvs_handle_t handle;
    esp_err_t error = nvs_open(MQTT_NAMESPACE, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        error = nvs_set_blob(handle, MQTT_KEY, &record, sizeof(record));
        if (error == ESP_OK) {
            error = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    jetkvm_mqtt_secure_zero(&record, sizeof(record));
    return error;
}

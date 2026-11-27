/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_mqtt_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "nvs.h"

#define MQTT_NAMESPACE "jkvm_mqtt"
#define MQTT_KEY "config"
#define MQTT_RECORD_MAGIC 0x54514d4aU
#define MQTT_RECORD_VERSION 2U
#define MQTT_RECORD_VERSION_1 1U

typedef struct {
    uint32_t magic;
    uint32_t version;
    jetkvm_mqtt_config_t config;
    uint32_t crc32;
} mqtt_record_t;

/* The settings as firmware before enable_keyboard_macros stored them. */
typedef struct {
    bool enabled;
    char broker[JETKVM_MQTT_BROKER_CAPACITY];
    int32_t port;
    char username[JETKVM_MQTT_USERNAME_CAPACITY];
    char password[JETKVM_MQTT_PASSWORD_CAPACITY];
    char base_topic[JETKVM_MQTT_BASE_TOPIC_CAPACITY];
    bool use_tls;
    bool tls_insecure;
    bool enable_ha_discovery;
    bool enable_actions;
    int32_t debounce_ms;
} mqtt_config_v1_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    mqtt_config_v1_t config;
    uint32_t crc32;
} mqtt_record_v1_t;

typedef union {
    mqtt_record_t current;
    mqtt_record_v1_t v1;
} mqtt_stored_record_t;

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

/* A version 1 record loads with keyboard macros over MQTT off, as Go loads
 * a config file saved before the field existed. */
static bool record_v1_to_config(const mqtt_record_v1_t *record,
                                jetkvm_mqtt_config_t *config)
{
    if (record->magic != MQTT_RECORD_MAGIC ||
        record->version != MQTT_RECORD_VERSION_1 ||
        record->crc32 != crc32(record, offsetof(mqtt_record_v1_t, crc32))) {
        return false;
    }
    const mqtt_config_v1_t *old = &record->config;
    memset(config, 0, sizeof(*config));
    config->enabled = old->enabled;
    memcpy(config->broker, old->broker, sizeof(config->broker));
    config->port = old->port;
    memcpy(config->username, old->username, sizeof(config->username));
    memcpy(config->password, old->password, sizeof(config->password));
    memcpy(config->base_topic, old->base_topic, sizeof(config->base_topic));
    config->use_tls = old->use_tls;
    config->tls_insecure = old->tls_insecure;
    config->enable_ha_discovery = old->enable_ha_discovery;
    config->enable_actions = old->enable_actions;
    config->enable_keyboard_macros = false;
    config->debounce_ms = old->debounce_ms;
    return true;
}

esp_err_t jetkvm_mqtt_store_load(jetkvm_mqtt_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    jetkvm_mqtt_config_defaults(config);
    mqtt_stored_record_t record;
    memset(&record, 0, sizeof(record));
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
    jetkvm_mqtt_config_t loaded;
    if (size == sizeof(record.current) &&
        record.current.version == MQTT_RECORD_VERSION) {
        if (record.current.magic != MQTT_RECORD_MAGIC ||
            record.current.crc32 !=
                crc32(&record.current, offsetof(mqtt_record_t, crc32))) {
            jetkvm_mqtt_secure_zero(&record, sizeof(record));
            return ESP_ERR_INVALID_CRC;
        }
        loaded = record.current.config;
    } else if (size == sizeof(record.v1) &&
               record.v1.version == MQTT_RECORD_VERSION_1) {
        if (!record_v1_to_config(&record.v1, &loaded)) {
            jetkvm_mqtt_secure_zero(&record, sizeof(record));
            return ESP_ERR_INVALID_CRC;
        }
    } else {
        jetkvm_mqtt_secure_zero(&record, sizeof(record));
        return ESP_ERR_INVALID_SIZE;
    }
    jetkvm_mqtt_secure_zero(&record, sizeof(record));
    if (!jetkvm_mqtt_config_validate(&loaded, NULL, 0U)) {
        jetkvm_mqtt_secure_zero(&loaded, sizeof(loaded));
        return ESP_ERR_INVALID_STATE;
    }
    *config = loaded;
    jetkvm_mqtt_secure_zero(&loaded, sizeof(loaded));
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

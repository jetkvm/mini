/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_cloud_config.h"

#include <stddef.h>
#include <string.h>

#include "jetkvm_memory_policy.h"
#include "nvs.h"

#define CLOUD_NVS_NAMESPACE "jkvm_cloud"
#define CLOUD_NVS_CONFIG_KEY "config"
#define CLOUD_NVS_RECORD_MAGIC 0x46434b4aU /* "JKCF" in little endian */

typedef struct {
    uint32_t magic;
    uint32_t schema_version;
    char cloud_url[JETKVM_CLOUD_URL_CAPACITY];
    char cloud_app_url[JETKVM_CLOUD_APP_URL_CAPACITY];
    char cloud_token[JETKVM_CLOUD_TOKEN_CAPACITY];
    char google_identity[JETKVM_CLOUD_IDENTITY_CAPACITY];
    uint32_t crc32;
} cloud_nvs_record_t;

_Static_assert(offsetof(cloud_nvs_record_t, crc32) + sizeof(uint32_t) ==
                   sizeof(cloud_nvs_record_t),
               "cloud record CRC must be the final field");

static uint32_t record_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

static void secure_zero(void *value, size_t size)
{
    volatile unsigned char *bytes = value;
    while (size-- > 0U) {
        *bytes++ = 0U;
    }
}

static cloud_nvs_record_t *record_alloc(void)
{
    cloud_nvs_record_t *record =
        jetkvm_memory_policy_control_alloc(sizeof(*record));
    if (record) {
        memset(record, 0, sizeof(*record));
    }
    return record;
}

static void record_free(cloud_nvs_record_t *record)
{
    if (record) {
        secure_zero(record, sizeof(*record));
        jetkvm_memory_policy_control_free(record);
    }
}

static bool record_valid(const cloud_nvs_record_t *record)
{
    return record && record->magic == CLOUD_NVS_RECORD_MAGIC &&
           record->schema_version == JETKVM_CLOUD_CONFIG_SCHEMA_VERSION &&
           record->crc32 == record_crc32(record,
                                         offsetof(cloud_nvs_record_t, crc32));
}

static void record_to_config(const cloud_nvs_record_t *record,
                             jetkvm_cloud_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->schema_version = record->schema_version;
    memcpy(config->cloud_url, record->cloud_url,
           sizeof(config->cloud_url));
    memcpy(config->cloud_app_url, record->cloud_app_url,
           sizeof(config->cloud_app_url));
    memcpy(config->cloud_token, record->cloud_token,
           sizeof(config->cloud_token));
    memcpy(config->google_identity, record->google_identity,
           sizeof(config->google_identity));
}

static void config_to_record(const jetkvm_cloud_config_t *config,
                             cloud_nvs_record_t *record)
{
    memset(record, 0, sizeof(*record));
    record->magic = CLOUD_NVS_RECORD_MAGIC;
    record->schema_version = JETKVM_CLOUD_CONFIG_SCHEMA_VERSION;
    memcpy(record->cloud_url, config->cloud_url,
           sizeof(record->cloud_url));
    memcpy(record->cloud_app_url, config->cloud_app_url,
           sizeof(record->cloud_app_url));
    memcpy(record->cloud_token, config->cloud_token,
           sizeof(record->cloud_token));
    memcpy(record->google_identity, config->google_identity,
           sizeof(record->google_identity));
    record->crc32 = record_crc32(record,
                                 offsetof(cloud_nvs_record_t, crc32));
}

esp_err_t jetkvm_cloud_config_load(jetkvm_cloud_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    jetkvm_cloud_config_defaults(config);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLOUD_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t record_size = 0U;
    err = nvs_get_blob(handle, CLOUD_NVS_CONFIG_KEY, NULL, &record_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    if (record_size != sizeof(cloud_nvs_record_t)) {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    cloud_nvs_record_t *record = record_alloc();
    if (!record) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_blob(handle, CLOUD_NVS_CONFIG_KEY, record, &record_size);
    nvs_close(handle);
    if (err != ESP_OK) {
        record_free(record);
        return err;
    }
    if (record->magic != CLOUD_NVS_RECORD_MAGIC ||
        record->schema_version != JETKVM_CLOUD_CONFIG_SCHEMA_VERSION) {
        record_free(record);
        return ESP_ERR_INVALID_VERSION;
    }
    if (!record_valid(record)) {
        record_free(record);
        return ESP_ERR_INVALID_CRC;
    }
    record_to_config(record, config);
    record_free(record);
    return jetkvm_cloud_config_validate(config) ? ESP_OK :
                                                  ESP_ERR_INVALID_STATE;
}

esp_err_t jetkvm_cloud_config_save(const jetkvm_cloud_config_t *config)
{
    if (!jetkvm_cloud_config_validate(config)) {
        return ESP_ERR_INVALID_ARG;
    }
    cloud_nvs_record_t *record = record_alloc();
    if (!record) {
        return ESP_ERR_NO_MEM;
    }
    config_to_record(config, record);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLOUD_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        record_free(record);
        return err;
    }
    err = nvs_set_blob(handle, CLOUD_NVS_CONFIG_KEY,
                       record, sizeof(*record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    record_free(record);
    return err;
}

esp_err_t jetkvm_cloud_config_clear_registration_persisted(void)
{
    jetkvm_cloud_config_t *config =
        jetkvm_memory_policy_control_alloc(sizeof(*config));
    if (!config) {
        return ESP_ERR_NO_MEM;
    }
    memset(config, 0, sizeof(*config));
    esp_err_t err = jetkvm_cloud_config_load(config);
    if (err != ESP_OK) {
        secure_zero(config, sizeof(*config));
        jetkvm_memory_policy_control_free(config);
        return err;
    }
    jetkvm_cloud_config_clear_registration(config);
    err = jetkvm_cloud_config_save(config);
    secure_zero(config, sizeof(*config));
    jetkvm_memory_policy_control_free(config);
    return err;
}

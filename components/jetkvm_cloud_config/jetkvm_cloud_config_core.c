/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_cloud_config.h"

#include <string.h>

static size_t bounded_length(const char *value, size_t capacity)
{
    if (!value) {
        return capacity;
    }
    size_t length = 0;
    while (length < capacity && value[length] != '\0') {
        ++length;
    }
    return length;
}

static bool visible_ascii(const char *value, size_t capacity, bool allow_empty)
{
    size_t length = bounded_length(value, capacity);
    if (length >= capacity || (!allow_empty && length == 0U)) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x21U || c > 0x7eU) {
            return false;
        }
    }
    return true;
}

static bool valid_base_url(const char *url, size_t capacity)
{
    size_t length = bounded_length(url, capacity);
    if (length >= capacity || length == 0U) {
        return false;
    }
    size_t authority = 0U;
    if (length > 8U && memcmp(url, "https://", 8U) == 0) {
        authority = 8U;
    } else if (length > 7U && memcmp(url, "http://", 7U) == 0) {
        authority = 7U;
    } else {
        return false;
    }

    size_t authority_end = authority;
    while (authority_end < length && url[authority_end] != '/') {
        unsigned char c = (unsigned char)url[authority_end];
        if (c < 0x21U || c > 0x7eU || c == '?' || c == '#' || c == '\\' ||
            c == '"') {
            return false;
        }
        ++authority_end;
    }
    if (authority_end == authority) {
        return false;
    }
    for (size_t i = authority; i < authority_end; ++i) {
        if (url[i] == '@') {
            return false;
        }
    }
    for (size_t i = authority_end; i < length; ++i) {
        unsigned char c = (unsigned char)url[i];
        if (c < 0x21U || c > 0x7eU || c == '?' || c == '#' || c == '\\' ||
            c == '"') {
            return false;
        }
    }
    return true;
}

static esp_err_t copy_value(char *destination, size_t capacity,
                            const char *source, bool allow_empty)
{
    if (!destination || !source || capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t length = bounded_length(source, capacity);
    if (length >= capacity || (!allow_empty && length == 0U)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(destination, source, length + 1U);
    return ESP_OK;
}

void jetkvm_cloud_config_defaults(jetkvm_cloud_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->schema_version = JETKVM_CLOUD_CONFIG_SCHEMA_VERSION;
    memcpy(config->cloud_url, JETKVM_DEFAULT_CLOUD_URL,
           sizeof(JETKVM_DEFAULT_CLOUD_URL));
    memcpy(config->cloud_app_url, JETKVM_DEFAULT_CLOUD_APP_URL,
           sizeof(JETKVM_DEFAULT_CLOUD_APP_URL));
}

bool jetkvm_cloud_config_validate(const jetkvm_cloud_config_t *config)
{
    if (!config ||
        config->schema_version != JETKVM_CLOUD_CONFIG_SCHEMA_VERSION ||
        !valid_base_url(config->cloud_url, sizeof(config->cloud_url)) ||
        !valid_base_url(config->cloud_app_url,
                        sizeof(config->cloud_app_url)) ||
        !visible_ascii(config->cloud_token, sizeof(config->cloud_token), true) ||
        !visible_ascii(config->google_identity,
                       sizeof(config->google_identity), true)) {
        return false;
    }
    bool has_token = config->cloud_token[0] != '\0';
    bool has_identity = config->google_identity[0] != '\0';
    return has_token == has_identity;
}

bool jetkvm_cloud_config_is_registered(const jetkvm_cloud_config_t *config)
{
    return jetkvm_cloud_config_validate(config) &&
           config->cloud_token[0] != '\0' &&
           config->google_identity[0] != '\0';
}

esp_err_t jetkvm_cloud_config_set_urls(
    jetkvm_cloud_config_t *config,
    const char *cloud_url, const char *cloud_app_url)
{
    if (!config || !valid_base_url(cloud_url, JETKVM_CLOUD_URL_CAPACITY) ||
        !valid_base_url(cloud_app_url, JETKVM_CLOUD_APP_URL_CAPACITY)) {
        return ESP_ERR_INVALID_ARG;
    }
    char api_copy[JETKVM_CLOUD_URL_CAPACITY];
    char app_copy[JETKVM_CLOUD_APP_URL_CAPACITY];
    esp_err_t err = copy_value(api_copy, sizeof(api_copy), cloud_url, false);
    if (err != ESP_OK) {
        return err;
    }
    err = copy_value(app_copy, sizeof(app_copy), cloud_app_url, false);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(config->cloud_url, api_copy, sizeof(api_copy));
    memcpy(config->cloud_app_url, app_copy, sizeof(app_copy));
    return ESP_OK;
}

esp_err_t jetkvm_cloud_config_set_registration(
    jetkvm_cloud_config_t *config,
    const char *cloud_token, const char *google_identity)
{
    if (!config ||
        !visible_ascii(cloud_token, JETKVM_CLOUD_TOKEN_CAPACITY, false) ||
        !visible_ascii(google_identity,
                       JETKVM_CLOUD_IDENTITY_CAPACITY, false)) {
        return ESP_ERR_INVALID_ARG;
    }
    char token_copy[JETKVM_CLOUD_TOKEN_CAPACITY];
    char identity_copy[JETKVM_CLOUD_IDENTITY_CAPACITY];
    esp_err_t err = copy_value(token_copy, sizeof(token_copy),
                               cloud_token, false);
    if (err != ESP_OK) {
        return err;
    }
    err = copy_value(identity_copy, sizeof(identity_copy),
                     google_identity, false);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(config->cloud_token, token_copy, sizeof(token_copy));
    memcpy(config->google_identity, identity_copy, sizeof(identity_copy));
    return ESP_OK;
}

void jetkvm_cloud_config_clear_registration(jetkvm_cloud_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config->cloud_token, 0, sizeof(config->cloud_token));
    memset(config->google_identity, 0, sizeof(config->google_identity));
}

/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_mqtt.h"

#include <stdio.h>
#include <string.h>

static bool terminated(const char *value, size_t capacity)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

static void explain(char *output, size_t capacity, const char *message)
{
    if (output == NULL || capacity == 0U) {
        return;
    }
    (void)snprintf(output, capacity, "%s", message ? message : "invalid MQTT settings");
}

void jetkvm_mqtt_config_defaults(jetkvm_mqtt_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->port = JETKVM_MQTT_DEFAULT_PORT;
    (void)snprintf(config->base_topic, sizeof(config->base_topic), "%s",
                   JETKVM_MQTT_DEFAULT_BASE_TOPIC);
    config->enable_ha_discovery = true;
    config->enable_actions = true;
    config->debounce_ms = JETKVM_MQTT_DEFAULT_DEBOUNCE_MS;
}

bool jetkvm_mqtt_config_validate(
    const jetkvm_mqtt_config_t *config,
    char *error, size_t error_capacity)
{
    if (error != NULL && error_capacity > 0U) {
        error[0] = '\0';
    }
    if (config == NULL) {
        explain(error, error_capacity, "MQTT settings are required");
        return false;
    }
    if (!terminated(config->broker, sizeof(config->broker)) ||
        !terminated(config->username, sizeof(config->username)) ||
        !terminated(config->password, sizeof(config->password)) ||
        !terminated(config->base_topic, sizeof(config->base_topic))) {
        explain(error, error_capacity, "MQTT setting exceeds the supported length");
        return false;
    }
    if (config->enabled && config->broker[0] == '\0') {
        explain(error, error_capacity,
                "broker address is required when MQTT is enabled");
        return false;
    }
    if (config->port < 1 || config->port > 65535) {
        explain(error, error_capacity, "port must be between 1 and 65535");
        return false;
    }
    if (config->base_topic[0] == '\0') {
        explain(error, error_capacity, "base topic must not be empty");
        return false;
    }
    if (strpbrk(config->base_topic, "+#") != NULL) {
        explain(error, error_capacity,
                "base topic must not contain MQTT wildcards (+ or #)");
        return false;
    }
    if (strchr(config->base_topic, ' ') != NULL) {
        explain(error, error_capacity, "base topic must not contain spaces");
        return false;
    }
    if (config->debounce_ms < 0) {
        explain(error, error_capacity, "debounce must not be negative");
        return false;
    }
    return true;
}

bool jetkvm_mqtt_config_normalize(
    jetkvm_mqtt_config_t *candidate,
    const jetkvm_mqtt_config_t *existing,
    char *error, size_t error_capacity)
{
    if (candidate == NULL) {
        explain(error, error_capacity, "MQTT settings are required");
        return false;
    }
    if (!terminated(candidate->base_topic, sizeof(candidate->base_topic)) ||
        !terminated(candidate->password, sizeof(candidate->password))) {
        explain(error, error_capacity, "MQTT setting exceeds the supported length");
        return false;
    }
    if (candidate->base_topic[0] == '\0') {
        (void)snprintf(candidate->base_topic, sizeof(candidate->base_topic),
                       "%s", JETKVM_MQTT_DEFAULT_BASE_TOPIC);
    }
    if (candidate->debounce_ms < 0) {
        candidate->debounce_ms = 0;
    }
    if (strcmp(candidate->password, JETKVM_MQTT_PASSWORD_MASK) == 0 &&
        existing != NULL &&
        terminated(existing->password, sizeof(existing->password))) {
        memcpy(candidate->password, existing->password,
               sizeof(candidate->password));
        candidate->password[sizeof(candidate->password) - 1U] = '\0';
    }
    return jetkvm_mqtt_config_validate(candidate, error, error_capacity);
}

bool jetkvm_mqtt_build_base_topic(
    const jetkvm_mqtt_config_t *config, const char *device_id,
    char *output, size_t output_capacity)
{
    if (config == NULL || device_id == NULL || device_id[0] == '\0' ||
        output == NULL || output_capacity == 0U ||
        !terminated(config->base_topic, sizeof(config->base_topic)) ||
        config->base_topic[0] == '\0') {
        return false;
    }
    int written = strstr(config->base_topic, device_id) != NULL ?
        snprintf(output, output_capacity, "%s", config->base_topic) :
        snprintf(output, output_capacity, "%s/%s", config->base_topic,
                 device_id);
    return written >= 0 && (size_t)written < output_capacity;
}

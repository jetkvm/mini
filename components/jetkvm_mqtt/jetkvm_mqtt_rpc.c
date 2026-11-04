/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include "jetkvm_control.h"
#include "jetkvm_mqtt.h"

_Static_assert(JETKVM_CONTROL_MQTT_BROKER_CAPACITY ==
                   JETKVM_MQTT_BROKER_CAPACITY,
               "MQTT broker capacities must match");
_Static_assert(JETKVM_CONTROL_MQTT_USERNAME_CAPACITY ==
                   JETKVM_MQTT_USERNAME_CAPACITY,
               "MQTT username capacities must match");
_Static_assert(JETKVM_CONTROL_MQTT_PASSWORD_CAPACITY ==
                   JETKVM_MQTT_PASSWORD_CAPACITY,
               "MQTT password capacities must match");
_Static_assert(JETKVM_CONTROL_MQTT_BASE_TOPIC_CAPACITY ==
                   JETKVM_MQTT_BASE_TOPIC_CAPACITY,
               "MQTT topic capacities must match");

static void mqtt_native_to_control(
    const jetkvm_mqtt_config_t *native,
    jetkvm_control_mqtt_settings_t *control)
{
    memset(control, 0, sizeof(*control));
    control->enabled = native->enabled;
    memcpy(control->broker, native->broker, sizeof(control->broker));
    control->port = native->port;
    memcpy(control->username, native->username, sizeof(control->username));
    memcpy(control->password, native->password, sizeof(control->password));
    memcpy(control->base_topic, native->base_topic,
           sizeof(control->base_topic));
    control->use_tls = native->use_tls;
    control->tls_insecure = native->tls_insecure;
    control->enable_ha_discovery = native->enable_ha_discovery;
    control->enable_actions = native->enable_actions;
    control->debounce_ms = native->debounce_ms;
}

static void mqtt_control_to_native(
    const jetkvm_control_mqtt_settings_t *control,
    jetkvm_mqtt_config_t *native)
{
    memset(native, 0, sizeof(*native));
    native->enabled = control->enabled;
    memcpy(native->broker, control->broker, sizeof(native->broker));
    native->port = control->port;
    memcpy(native->username, control->username, sizeof(native->username));
    memcpy(native->password, control->password, sizeof(native->password));
    memcpy(native->base_topic, control->base_topic,
           sizeof(native->base_topic));
    native->use_tls = control->use_tls;
    native->tls_insecure = control->tls_insecure;
    native->enable_ha_discovery = control->enable_ha_discovery;
    native->enable_actions = control->enable_actions;
    native->debounce_ms = control->debounce_ms;
}

static bool mqtt_rpc_get_settings(
    jetkvm_control_mqtt_settings_t *settings, void *ctx)
{
    (void)ctx;
    jetkvm_mqtt_config_t config;
    if (jetkvm_mqtt_get_config(&config, true) != ESP_OK) {
        return false;
    }
    mqtt_native_to_control(&config, settings);
    memset(&config, 0, sizeof(config));
    return true;
}

static jetkvm_mqtt_config_change_result_t mqtt_rpc_set_settings(
    const jetkvm_control_mqtt_settings_t *settings,
    char *error, size_t error_capacity, void *ctx)
{
    (void)ctx;
    jetkvm_mqtt_config_t config;
    mqtt_control_to_native(settings, &config);
    esp_err_t result = jetkvm_mqtt_set_config(
        &config, error, error_capacity);
    memset(&config, 0, sizeof(config));
    if (result == ESP_OK) {
        return JETKVM_MQTT_CONFIG_CHANGE_ACCEPTED;
    }
    if (result == ESP_ERR_INVALID_ARG) {
        return JETKVM_MQTT_CONFIG_CHANGE_INVALID;
    }
    if (result == ESP_ERR_INVALID_STATE) {
        return JETKVM_MQTT_CONFIG_CHANGE_UNAVAILABLE;
    }
    return JETKVM_MQTT_CONFIG_CHANGE_FAILED;
}

static bool mqtt_rpc_get_status(jetkvm_control_mqtt_status_t *status,
                                void *ctx)
{
    (void)ctx;
    jetkvm_mqtt_status_t native = {0};
    if (jetkvm_mqtt_get_status(&native) != ESP_OK) {
        return false;
    }
    memset(status, 0, sizeof(*status));
    status->connected = native.connected;
    memcpy(status->error, native.error, sizeof(status->error));
    memset(&native, 0, sizeof(native));
    return true;
}

static bool mqtt_rpc_test_connection(
    const jetkvm_control_mqtt_settings_t *settings,
    jetkvm_control_mqtt_test_result_t *result, void *ctx)
{
    (void)ctx;
    jetkvm_mqtt_config_t config;
    jetkvm_mqtt_test_result_t native = {0};
    mqtt_control_to_native(settings, &config);
    esp_err_t error = jetkvm_mqtt_test_connection(&config, &native);
    memset(&config, 0, sizeof(config));
    if (error != ESP_OK) {
        memset(&native, 0, sizeof(native));
        return false;
    }
    memset(result, 0, sizeof(*result));
    result->success = native.success;
    memcpy(result->error, native.error, sizeof(result->error));
    memset(&native, 0, sizeof(native));
    return true;
}

void jetkvm_mqtt_fill_control_context(jetkvm_mqtt_context_t *context)
{
    *context = (jetkvm_mqtt_context_t){
        .get_settings = mqtt_rpc_get_settings,
        .set_settings = mqtt_rpc_set_settings,
        .get_status = mqtt_rpc_get_status,
        .test_connection = mqtt_rpc_test_connection,
    };
}

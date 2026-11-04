/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "control_protocol_internal.h"

#include <string.h>

const jetkvm_mqtt_context_t *mqtt_context(
    const jetkvm_control_context_t *context)
{
    if (context &&
        (context->mqtt.get_settings || context->mqtt.set_settings ||
         context->mqtt.get_status || context->mqtt.test_connection)) {
        return &context->mqtt;
    }
    return &s_default_mqtt;
}

static bool mqtt_object_has_members(const cJSON *object, size_t expected)
{
    if (!cJSON_IsObject(object)) {
        return false;
    }
    size_t count = 0U;
    for (const cJSON *item = object->child; item != NULL; item = item->next) {
        count++;
    }
    return count == expected;
}

static bool parse_mqtt_settings(const cJSON *params,
                                jetkvm_control_mqtt_settings_t *settings)
{
    if (!cJSON_IsObject(params) || settings == NULL ||
        !mqtt_object_has_members(params, 1U)) {
        return false;
    }
    const cJSON *object =
        cJSON_GetObjectItemCaseSensitive(params, "settings");
    if (!mqtt_object_has_members(object, 11U)) {
        return false;
    }
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(object, "enabled");
    const cJSON *broker = cJSON_GetObjectItemCaseSensitive(object, "broker");
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(object, "port");
    const cJSON *username =
        cJSON_GetObjectItemCaseSensitive(object, "username");
    const cJSON *password =
        cJSON_GetObjectItemCaseSensitive(object, "password");
    const cJSON *base_topic =
        cJSON_GetObjectItemCaseSensitive(object, "base_topic");
    const cJSON *use_tls =
        cJSON_GetObjectItemCaseSensitive(object, "use_tls");
    const cJSON *tls_insecure =
        cJSON_GetObjectItemCaseSensitive(object, "tls_insecure");
    const cJSON *enable_ha_discovery =
        cJSON_GetObjectItemCaseSensitive(object, "enable_ha_discovery");
    const cJSON *enable_actions =
        cJSON_GetObjectItemCaseSensitive(object, "enable_actions");
    const cJSON *debounce_ms =
        cJSON_GetObjectItemCaseSensitive(object, "debounce_ms");
    int64_t port_value = 0;
    int64_t debounce_value = 0;
    memset(settings, 0, sizeof(*settings));
    if (!cJSON_IsBool(enabled) ||
        !copy_json_string(broker, settings->broker,
                          sizeof(settings->broker), true) ||
        !json_integer_in_range(port, INT32_MIN, INT32_MAX, &port_value) ||
        !copy_json_string(username, settings->username,
                          sizeof(settings->username), true) ||
        !copy_json_string(password, settings->password,
                          sizeof(settings->password), true) ||
        !copy_json_string(base_topic, settings->base_topic,
                          sizeof(settings->base_topic), true) ||
        !cJSON_IsBool(use_tls) || !cJSON_IsBool(tls_insecure) ||
        !cJSON_IsBool(enable_ha_discovery) ||
        !cJSON_IsBool(enable_actions) ||
        !json_integer_in_range(debounce_ms, INT32_MIN, INT32_MAX,
                               &debounce_value)) {
        return false;
    }
    settings->enabled = cJSON_IsTrue(enabled);
    settings->port = (int32_t)port_value;
    settings->use_tls = cJSON_IsTrue(use_tls);
    settings->tls_insecure = cJSON_IsTrue(tls_insecure);
    settings->enable_ha_discovery = cJSON_IsTrue(enable_ha_discovery);
    settings->enable_actions = cJSON_IsTrue(enable_actions);
    settings->debounce_ms = (int32_t)debounce_value;
    return true;
}

static cJSON *mqtt_settings_json(
    const jetkvm_control_mqtt_settings_t *settings)
{
    cJSON *result = cJSON_CreateObject();
    if (!result ||
        !cJSON_AddBoolToObject(result, "enabled", settings->enabled) ||
        !cJSON_AddStringToObject(result, "broker", settings->broker) ||
        !cJSON_AddNumberToObject(result, "port", settings->port) ||
        !cJSON_AddStringToObject(result, "username", settings->username) ||
        !cJSON_AddStringToObject(result, "password", settings->password) ||
        !cJSON_AddStringToObject(result, "base_topic",
                                settings->base_topic) ||
        !cJSON_AddBoolToObject(result, "use_tls", settings->use_tls) ||
        !cJSON_AddBoolToObject(result, "tls_insecure",
                              settings->tls_insecure) ||
        !cJSON_AddBoolToObject(result, "enable_ha_discovery",
                              settings->enable_ha_discovery) ||
        !cJSON_AddBoolToObject(result, "enable_actions",
                              settings->enable_actions) ||
        !cJSON_AddNumberToObject(result, "debounce_ms",
                                settings->debounce_ms)) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

bool rpc_get_mqtt_settings(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    const jetkvm_mqtt_context_t *mqtt = mqtt_context(context);
    jetkvm_control_mqtt_settings_t settings = {0};
    if (!mqtt->get_settings ||
        !mqtt->get_settings(&settings, mqtt->ctx)) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "MQTT settings unavailable";
        return true;
    }
    dispatch->result = mqtt_settings_json(&settings);
    memset(&settings, 0, sizeof(settings));
    return dispatch->result != NULL;
}

bool rpc_set_mqtt_settings(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    jetkvm_control_mqtt_settings_t settings;
    if (!parse_mqtt_settings(params, &settings)) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "settings must contain the complete MQTT configuration";
        return true;
    }
    const jetkvm_mqtt_context_t *mqtt = mqtt_context(context);
    if (!mqtt->set_settings) {
        memset(&settings, 0, sizeof(settings));
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "MQTT settings unavailable";
        return true;
    }
    char error[JETKVM_CONTROL_MQTT_ERROR_CAPACITY] = {0};
    jetkvm_mqtt_config_change_result_t result = mqtt->set_settings(
        &settings, error, sizeof(error), mqtt->ctx);
    memset(&settings, 0, sizeof(settings));
    memset(error, 0, sizeof(error));
    switch (result) {
    case JETKVM_MQTT_CONFIG_CHANGE_ACCEPTED:
        dispatch->result = cJSON_CreateNull();
        return dispatch->result != NULL;
    case JETKVM_MQTT_CONFIG_CHANGE_INVALID:
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "Invalid MQTT settings";
        return true;
    case JETKVM_MQTT_CONFIG_CHANGE_UNAVAILABLE:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "MQTT settings unavailable";
        return true;
    case JETKVM_MQTT_CONFIG_CHANGE_FAILED:
    default:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Failed to save MQTT settings";
        return true;
    }
}

bool rpc_get_mqtt_status(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    const jetkvm_mqtt_context_t *mqtt = mqtt_context(context);
    jetkvm_control_mqtt_status_t status = {0};
    if (!mqtt->get_status || !mqtt->get_status(&status, mqtt->ctx)) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "MQTT status unavailable";
        return true;
    }
    cJSON *result = cJSON_CreateObject();
    if (!result ||
        !cJSON_AddBoolToObject(result, "connected", status.connected) ||
        (status.error[0] != '\0' &&
         !cJSON_AddStringToObject(result, "error", status.error))) {
        cJSON_Delete(result);
        memset(&status, 0, sizeof(status));
        return false;
    }
    memset(&status, 0, sizeof(status));
    dispatch->result = result;
    return true;
}

bool rpc_test_mqtt_connection(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    jetkvm_control_mqtt_settings_t settings;
    if (!parse_mqtt_settings(params, &settings)) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "settings must contain the complete MQTT configuration";
        return true;
    }
    const jetkvm_mqtt_context_t *mqtt = mqtt_context(context);
    jetkvm_control_mqtt_test_result_t test = {0};
    if (!mqtt->test_connection ||
        !mqtt->test_connection(&settings, &test, mqtt->ctx)) {
        memset(&settings, 0, sizeof(settings));
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "MQTT connection test unavailable";
        return true;
    }
    memset(&settings, 0, sizeof(settings));
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddBoolToObject(result, "success", test.success) ||
        (test.error[0] != '\0' &&
         !cJSON_AddStringToObject(result, "error", test.error))) {
        cJSON_Delete(result);
        memset(&test, 0, sizeof(test));
        return false;
    }
    memset(&test, 0, sizeof(test));
    dispatch->result = result;
    return true;
}

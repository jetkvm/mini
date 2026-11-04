/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "control_protocol_internal.h"

#include <stdio.h>
#include <string.h>

const jetkvm_ota_context_t *ota_context(
    const jetkvm_control_context_t *context)
{
    if (context &&
        (context->ota.get_update_status || context->ota.check_update ||
         context->ota.try_update)) {
        return &context->ota;
    }
    return &s_default_ota;
}

static cJSON *ota_status_json(const jetkvm_control_ota_status_t *status)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *local = cJSON_CreateObject();
    cJSON *remote = cJSON_CreateObject();
    if (!root || !local || !remote ||
        !cJSON_AddItemToObject(root, "local", local) ||
        !cJSON_AddItemToObject(root, "remote", remote) ||
        !cJSON_AddStringToObject(local, "appVersion",
                                 status->local_app_version) ||
        !cJSON_AddStringToObject(local, "systemVersion",
                                 status->local_system_version) ||
        !cJSON_AddStringToObject(remote, "appVersion",
                                 status->remote_app_version) ||
        !cJSON_AddStringToObject(remote, "appUrl", status->remote_app_url) ||
        !cJSON_AddStringToObject(remote, "appHash",
                                 status->remote_app_hash) ||
        !cJSON_AddStringToObject(remote, "systemVersion",
                                 status->remote_system_version) ||
        !cJSON_AddStringToObject(remote, "systemUrl",
                                 status->remote_system_url) ||
        !cJSON_AddStringToObject(remote, "systemHash",
                                 status->remote_system_hash) ||
        !cJSON_AddBoolToObject(root, "systemUpdateAvailable",
                               status->system_update_available) ||
        !cJSON_AddBoolToObject(root, "appUpdateAvailable",
                               status->app_update_available) ||
        !cJSON_AddBoolToObject(root, "willDisableAutoUpdate",
                               status->will_disable_auto_update)) {
        cJSON_Delete(root);
        cJSON_Delete(local);
        cJSON_Delete(remote);
        return NULL;
    }
    return root;
}

/* A failed check is an RPC error, as in Go: the UI shows its update error
 * view for a rejected RPC, and "up to date" for any result. */
static bool ota_check_result(const jetkvm_control_ota_status_t *status,
                             const char *failure_prefix,
                             rpc_dispatch_result_t *dispatch)
{
    if (status->error[0] != '\0') {
        (void)snprintf(dispatch->error_storage,
                       sizeof(dispatch->error_storage), "%s: %s",
                       failure_prefix, status->error);
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = dispatch->error_storage;
        return true;
    }
    dispatch->result = ota_status_json(status);
    return dispatch->result != NULL;
}

static bool ota_status_rpc_result(const jetkvm_ota_context_t *ota,
                                  bool ok, rpc_dispatch_result_t *dispatch)
{
    if (!ok) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "update status unavailable";
        return true;
    }
    (void)ota;
    return true;
}

bool rpc_get_update_status(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    const jetkvm_ota_context_t *ota = ota_context(context);
    jetkvm_control_ota_status_t status = {0};
    if (!ota->get_update_status ||
        !ota->get_update_status(&status, ota->ctx)) {
        return ota_status_rpc_result(ota, false, dispatch);
    }
    return ota_check_result(&status, "error checking for updates", dispatch);
}

/* The dev channel setting is the default for includePreRelease; the
 * frontend's tryUpdate sends no params, and Go reads the setting there. */
static bool ota_dev_channel(const jetkvm_ota_context_t *ota)
{
    return ota->get_dev_channel && ota->get_dev_channel(ota->ctx);
}

/* checkUpdateComponents carries a deliberately odd double-nested params
 * shape in the Go/frontend contract:
 * {"params":{"components":{"app":"...","system":"..."}},"includePreRelease":b}
 * include_prerelease keeps the caller's value when the key is absent. */
static bool parse_update_components(
    const cJSON *params, char *app_constraint, size_t app_capacity,
    char *system_constraint, size_t system_capacity, bool *include_prerelease,
    bool *has_components, bool *reset_config)
{
    app_constraint[0] = '\0';
    system_constraint[0] = '\0';
    *has_components = false;
    if (reset_config) {
        *reset_config = false;
    }
    if (params == NULL) {
        return true;
    }
    if (!cJSON_IsObject(params)) {
        return false;
    }
    const cJSON *prerelease_item = cJSON_GetObjectItemCaseSensitive(
        params, "includePreRelease");
    if (prerelease_item != NULL) {
        if (!cJSON_IsBool(prerelease_item)) {
            return false;
        }
        *include_prerelease = cJSON_IsTrue(prerelease_item);
    }
    if (reset_config) {
        const cJSON *reset_item =
            cJSON_GetObjectItemCaseSensitive(params, "resetConfig");
        if (reset_item != NULL) {
            if (!cJSON_IsBool(reset_item)) {
                return false;
            }
            *reset_config = cJSON_IsTrue(reset_item);
        }
    }
    const cJSON *nested =
        cJSON_GetObjectItemCaseSensitive(params, "params");
    if (nested == NULL) {
        return true;
    }
    if (!cJSON_IsObject(nested)) {
        return false;
    }
    const cJSON *components =
        cJSON_GetObjectItemCaseSensitive(nested, "components");
    if (components == NULL) {
        return true;
    }
    if (!cJSON_IsObject(components)) {
        return false;
    }
    *has_components = true;
    const cJSON *app_item =
        cJSON_GetObjectItemCaseSensitive(components, "app");
    if (app_item != NULL &&
        (!cJSON_IsString(app_item) || app_item->valuestring == NULL ||
         strlen(app_item->valuestring) >= app_capacity)) {
        return false;
    }
    if (app_item != NULL) {
        memcpy(app_constraint, app_item->valuestring,
               strlen(app_item->valuestring) + 1U);
    }
    const cJSON *system_item =
        cJSON_GetObjectItemCaseSensitive(components, "system");
    if (system_item != NULL &&
        (!cJSON_IsString(system_item) || system_item->valuestring == NULL ||
         strlen(system_item->valuestring) >= system_capacity)) {
        return false;
    }
    if (system_item != NULL) {
        memcpy(system_constraint, system_item->valuestring,
               strlen(system_item->valuestring) + 1U);
    }
    return true;
}

bool rpc_check_update_components(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const jetkvm_ota_context_t *ota = ota_context(context);
    char app_constraint[JETKVM_CONTROL_OTA_VERSION_CAPACITY];
    char system_constraint[JETKVM_CONTROL_OTA_VERSION_CAPACITY];
    bool include_prerelease = ota_dev_channel(ota);
    bool has_components = false;
    if (!parse_update_components(
            params, app_constraint, sizeof(app_constraint),
            system_constraint, sizeof(system_constraint),
            &include_prerelease, &has_components, NULL)) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "components and includePreRelease expected";
        return true;
    }
    jetkvm_control_ota_status_t status = {0};
    if (!ota->check_update ||
        !ota->check_update(app_constraint, system_constraint,
                           include_prerelease, &status, ota->ctx)) {
        return ota_status_rpc_result(ota, false, dispatch);
    }
    return ota_check_result(&status, "failed to check update", dispatch);
}

static bool ota_try_update_common(
    const jetkvm_control_context_t *context, const cJSON *params,
    bool components_variant, rpc_dispatch_result_t *dispatch)
{
    const jetkvm_ota_context_t *ota = ota_context(context);
    char app_constraint[JETKVM_CONTROL_OTA_VERSION_CAPACITY] = {0};
    char system_constraint[JETKVM_CONTROL_OTA_VERSION_CAPACITY] = {0};
    bool include_prerelease = ota_dev_channel(ota);
    bool has_components = false;
    bool reset_config = false;
    if (components_variant) {
        if (!parse_update_components(
                params, app_constraint, sizeof(app_constraint),
                system_constraint, sizeof(system_constraint),
                &include_prerelease, &has_components, &reset_config)) {
            dispatch->error_code = -32602;
            dispatch->error_message = "Invalid params";
            dispatch->error_data =
                "components, includePreRelease and resetConfig expected";
            return true;
        }
    } else if (params != NULL && !cJSON_IsObject(params)) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "object params expected";
        return true;
    }
    char error[JETKVM_CONTROL_OTA_ERROR_CAPACITY] = {0};
    if (!ota->try_update ||
        !ota->try_update(app_constraint, system_constraint,
                         include_prerelease, reset_config, error,
                         sizeof(error), ota->ctx)) {
        memset(error, 0, sizeof(error));
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "an update is already in progress";
        return true;
    }
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}

bool rpc_try_update(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    return ota_try_update_common(context, params, false, dispatch);
}

bool rpc_try_update_components(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    return ota_try_update_common(context, params, true, dispatch);
}

/* getX / setX pairs over one persisted boolean: no params in, bool out;
 * {enabled} in, the effective value out. */
static bool ota_get_flag(bool (*get)(void *ctx), void *ctx,
                         const char *unavailable, const cJSON *params,
                         rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    if (!get) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = unavailable;
        return true;
    }
    dispatch->result = cJSON_CreateBool(get(ctx));
    return dispatch->result != NULL;
}

static bool ota_set_flag(bool (*set)(bool enabled, void *ctx), void *ctx,
                         const char *unavailable, const cJSON *params,
                         rpc_dispatch_result_t *dispatch)
{
    const cJSON *enabled_item = rpc_param(params, "enabled");
    if (!cJSON_IsBool(enabled_item)) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "enabled boolean expected";
        return true;
    }
    if (!set) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = unavailable;
        return true;
    }
    dispatch->result = cJSON_CreateBool(set(cJSON_IsTrue(enabled_item), ctx));
    return dispatch->result != NULL;
}

bool rpc_get_auto_update_state(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const jetkvm_ota_context_t *ota = ota_context(context);
    return ota_get_flag(ota->get_auto_update, ota->ctx,
                        "auto-update state unavailable", params, dispatch);
}

bool rpc_set_auto_update_state(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const jetkvm_ota_context_t *ota = ota_context(context);
    return ota_set_flag(ota->set_auto_update, ota->ctx,
                        "auto-update state unavailable", params, dispatch);
}

bool rpc_get_dev_channel_state(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const jetkvm_ota_context_t *ota = ota_context(context);
    return ota_get_flag(ota->get_dev_channel, ota->ctx,
                        "dev channel state unavailable", params, dispatch);
}

bool rpc_set_dev_channel_state(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const jetkvm_ota_context_t *ota = ota_context(context);
    return ota_set_flag(ota->set_dev_channel, ota->ctx,
                        "dev channel state unavailable", params, dispatch);
}

bool rpc_is_update_pending(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    const jetkvm_ota_context_t *ota = ota_context(context);
    if (!ota->is_update_pending) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "update state unavailable";
        return true;
    }
    dispatch->result = cJSON_CreateBool(ota->is_update_pending(ota->ctx));
    return dispatch->result != NULL;
}

bool rpc_get_update_api_url(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    const jetkvm_ota_context_t *ota = ota_context(context);
    char url[JETKVM_CONTROL_OTA_API_URL_CAPACITY] = {0};
    if (!ota->get_update_api_url ||
        !ota->get_update_api_url(url, sizeof(url), ota->ctx)) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "update server configuration unavailable";
        return true;
    }
    dispatch->result = cJSON_CreateString(url);
    return dispatch->result != NULL;
}

bool rpc_set_update_api_url(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const cJSON *url_item = rpc_param(params, "url");
    if (!cJSON_IsString(url_item) || url_item->valuestring == NULL ||
        strlen(url_item->valuestring) >= JETKVM_CONTROL_OTA_API_URL_CAPACITY) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "url string expected";
        return true;
    }
    const jetkvm_ota_context_t *ota = ota_context(context);
    char error[JETKVM_CONTROL_OTA_ERROR_CAPACITY] = {0};
    if (!ota->set_update_api_url ||
        !ota->set_update_api_url(url_item->valuestring, error, sizeof(error),
                                 ota->ctx)) {
        memset(error, 0, sizeof(error));
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "invalid update server URL";
        return true;
    }
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}

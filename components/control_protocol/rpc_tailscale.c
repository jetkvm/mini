#include "control_protocol_internal.h"
#include <string.h>

const jetkvm_tailscale_context_t *tailscale_context(const jetkvm_control_context_t *context)
{
    return context && (context->tailscale.get_status || context->tailscale.get_config ||
        context->tailscale.set_config) ? &context->tailscale : &s_default_tailscale;
}

static bool unavailable(const jetkvm_tailscale_context_t *ts, rpc_dispatch_result_t *dispatch)
{
    if (ts->get_status && ts->get_config && ts->set_config) return false;
    dispatch->error_code = -32601;
    dispatch->error_message = "Method not found";
    return true;
}

bool rpc_get_tailscale_status(const jetkvm_control_context_t *context, const cJSON *params,
                              rpc_dispatch_result_t *dispatch)
{
    const jetkvm_tailscale_context_t *ts = tailscale_context(context);
    if (unavailable(ts, dispatch) || !require_no_rpc_params(params, dispatch)) return true;
    jetkvm_tailscale_status_t status = {0};
    if (!ts->get_status(&status, ts->ctx)) return set_hid_unavailable(dispatch, "Tailscale status unavailable");
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddBoolToObject(result, "installed", status.installed) ||
        !cJSON_AddBoolToObject(result, "running", status.running) ||
        !cJSON_AddBoolToObject(result, "configurable", true) ||
        !cJSON_AddBoolToObject(result, "enabled", status.enabled) ||
        !cJSON_AddBoolToObject(result, "authKeySet", status.auth_key_set) ||
        !cJSON_AddBoolToObject(result, "loggedIn", status.logged_in) ||
        !cJSON_AddStringToObject(result, "controlURL", status.control_url) ||
        !cJSON_AddStringToObject(result, "backendState", status.backend_state)) goto fail;
    if (status.auth_url[0] && !cJSON_AddStringToObject(result, "authURL", status.auth_url)) goto fail;
    if (status.health[0]) {
        cJSON *health = cJSON_AddArrayToObject(result, "health");
        if (!health || !cJSON_AddItemToArray(health, cJSON_CreateString(status.health))) goto fail;
    }
    if (status.ipv4[0]) {
        cJSON *self = cJSON_AddObjectToObject(result, "self");
        if (!self || !cJSON_AddStringToObject(self, "hostName", status.hostname) ||
            !cJSON_AddStringToObject(self, "dnsName", status.dns_name) ||
            !cJSON_AddStringToObject(self, "os", "esp-idf") ||
            !cJSON_AddBoolToObject(self, "online", status.running)) goto fail;
        cJSON *ips = cJSON_AddArrayToObject(self, "tailscaleIPs");
        if (!ips || !cJSON_AddItemToArray(ips, cJSON_CreateString(status.ipv4))) goto fail;
    }
    dispatch->result = result;
    return true;
fail:
    cJSON_Delete(result);
    return false;
}

bool rpc_get_tailscale_config(const jetkvm_control_context_t *context, const cJSON *params,
                              rpc_dispatch_result_t *dispatch)
{
    const jetkvm_tailscale_context_t *ts = tailscale_context(context);
    if (unavailable(ts, dispatch) || !require_no_rpc_params(params, dispatch)) return true;
    jetkvm_tailscale_config_t config = {0};
    if (!ts->get_config(&config, ts->ctx)) return set_hid_unavailable(dispatch, "Tailscale settings unavailable");
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddBoolToObject(result, "enabled", config.enabled) ||
        !cJSON_AddBoolToObject(result, "authKeySet", config.auth_key[0] != '\0') ||
        !cJSON_AddStringToObject(result, "controlURL", config.control_url)) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

bool rpc_get_tailscale_control_url(const jetkvm_control_context_t *context, const cJSON *params,
                                  rpc_dispatch_result_t *dispatch)
{
    const jetkvm_tailscale_context_t *ts = tailscale_context(context);
    if (unavailable(ts, dispatch) || !require_no_rpc_params(params, dispatch)) return true;
    jetkvm_tailscale_config_t config = {0};
    if (!ts->get_config(&config, ts->ctx)) return set_hid_unavailable(dispatch, "Tailscale settings unavailable");
    dispatch->result = cJSON_CreateString(config.control_url[0] ? config.control_url :
        "https://controlplane.tailscale.com");
    return dispatch->result != NULL;
}

static bool update(const jetkvm_tailscale_context_t *ts,
                   const jetkvm_tailscale_config_t *config, rpc_dispatch_result_t *dispatch)
{
    const char *error = ts->set_config(config, ts->ctx);
    if (error) return set_invalid_hid_params(dispatch, error);
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}

bool rpc_set_tailscale_control_url(const jetkvm_control_context_t *context, const cJSON *params,
                                  rpc_dispatch_result_t *dispatch)
{
    const jetkvm_tailscale_context_t *ts = tailscale_context(context);
    if (unavailable(ts, dispatch)) return true;
    jetkvm_tailscale_config_t config = {0};
    if (!ts->get_config(&config, ts->ctx)) return set_hid_unavailable(dispatch, "Tailscale settings unavailable");
    if (!cJSON_IsObject(params) || cJSON_GetArraySize(params) != 1 ||
        !copy_json_string(rpc_param(params, "controlURL"), config.control_url,
            sizeof(config.control_url), true))
        return set_invalid_hid_params(dispatch, "Expected controlURL string");
    return update(ts, &config, dispatch);
}

bool rpc_set_tailscale_config(const jetkvm_control_context_t *context, const cJSON *params,
                             rpc_dispatch_result_t *dispatch)
{
    const jetkvm_tailscale_context_t *ts = tailscale_context(context);
    if (unavailable(ts, dispatch)) return true;
    jetkvm_tailscale_config_t config = {0};
    if (!ts->get_config(&config, ts->ctx)) return set_hid_unavailable(dispatch, "Tailscale settings unavailable");
    const cJSON *enabled = rpc_param(params, "enabled"), *key = rpc_param(params, "authKey");
    if (!cJSON_IsObject(params) || !cJSON_IsBool(enabled) ||
        cJSON_GetArraySize(params) != (key ? 2 : 1) ||
        (key && !copy_json_string(key, config.auth_key, sizeof(config.auth_key), true)))
        return set_invalid_hid_params(dispatch, "Expected enabled boolean and optional authKey string");
    config.enabled = cJSON_IsTrue(enabled);
    return update(ts, &config, dispatch);
}

bool rpc_logout_tailscale(const jetkvm_control_context_t *context, const cJSON *params,
                          rpc_dispatch_result_t *dispatch)
{
    const jetkvm_tailscale_context_t *ts = tailscale_context(context);
    if (unavailable(ts, dispatch)) return true;
    if (!ts->logout) {
        dispatch->error_code = -32601;
        dispatch->error_message = "Method not found";
        return true;
    }
    if (!require_no_rpc_params(params, dispatch)) return true;
    const char *error = ts->logout(ts->ctx);
    if (error) return set_hid_unavailable(dispatch, error);
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}

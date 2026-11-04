/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "control_protocol_internal.h"

bool rpc_reboot(const jetkvm_control_context_t *context,
                       const cJSON *params,
                       rpc_dispatch_result_t *dispatch)
{
    const cJSON *force = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "force") : NULL;
    if (!cJSON_IsBool(force)) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "force must be a boolean";
        return true;
    }
    if (!context->request_reboot) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Reboot unavailable";
        return true;
    }
    switch (context->request_reboot(cJSON_IsTrue(force),
                                    context->request_reboot_ctx)) {
    case JETKVM_REBOOT_REQUEST_ACCEPTED:
        dispatch->result = cJSON_CreateNull();
        return dispatch->result != NULL;
    case JETKVM_REBOOT_REQUEST_BUSY:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Reboot already pending";
        return true;
    case JETKVM_REBOOT_REQUEST_UNAVAILABLE:
    default:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Reboot unavailable";
        return true;
    }
}

/* The transport defers this blocking operation off the peer task. */
bool rpc_deregister_device(const jetkvm_control_context_t *context,
                           const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    const char *error = context->deregister_device ?
        context->deregister_device(context->deregister_device_ctx) :
        "Cloud deregistration unavailable or busy";
    if (error) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = error;
        return true;
    }
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}


bool rpc_factory_reset(const jetkvm_control_context_t *context,
                       const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) return true;
    /* Allocate the reply before authorizing destructive work. */
    cJSON *result = cJSON_CreateNull();
    if (!result) return false;
    const char *error = context->factory_reset ?
        context->factory_reset(context->factory_reset_ctx) :
        "Factory reset unavailable or busy";
    if (error) {
        cJSON_Delete(result);
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = error;
    } else dispatch->result = result;
    return true;
}

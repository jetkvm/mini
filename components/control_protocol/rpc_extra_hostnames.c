#include "control_protocol_internal.h"
#include <string.h>

static bool hostnames_missing(rpc_dispatch_result_t *dispatch)
{
    if (s_default_extra_hostnames.get && s_default_extra_hostnames.set) return false;
    dispatch->error_code = -32601;
    dispatch->error_message = "Method not found";
    return true;
}

static bool hostnames_result(rpc_dispatch_result_t *dispatch)
{
    char (*names)[JETKVM_EXTRA_HOSTNAME_CAPACITY] =
        cJSON_malloc(JETKVM_EXTRA_HOSTNAMES_MAX * JETKVM_EXTRA_HOSTNAME_CAPACITY);
    if (!names) return false;
    size_t count = s_default_extra_hostnames.get(names, JETKVM_EXTRA_HOSTNAMES_MAX,
                                                 s_default_extra_hostnames.ctx);
    cJSON *result = cJSON_CreateArray();
    for (size_t i = 0; result && i < count; ++i) {
        cJSON *name = cJSON_CreateString(names[i]);
        if (!name || !cJSON_AddItemToArray(result, name)) {
            cJSON_Delete(name);
            cJSON_Delete(result);
            result = NULL;
        }
    }
    cJSON_free(names);
    dispatch->result = result;
    return result != NULL;
}

bool rpc_get_extra_hostnames(const jetkvm_control_context_t *context, const cJSON *params,
                             rpc_dispatch_result_t *dispatch)
{
    (void)context;
    if (hostnames_missing(dispatch)) return true;
    if (!require_no_rpc_params(params, dispatch)) return true;
    return hostnames_result(dispatch);
}

bool rpc_set_extra_hostnames(const jetkvm_control_context_t *context, const cJSON *params,
                             rpc_dispatch_result_t *dispatch)
{
    (void)context;
    if (hostnames_missing(dispatch)) return true;
    const cJSON *list = rpc_param(params, "hostnames");
    const char *names[JETKVM_EXTRA_HOSTNAMES_MAX];
    size_t count = 0;
    bool valid = cJSON_IsArray(list) && cJSON_GetArraySize(list) <= (int)JETKVM_EXTRA_HOSTNAMES_MAX;
    for (const cJSON *item = valid ? list->child : NULL; item; item = item->next) {
        if (!cJSON_IsString(item)) valid = false;
        else names[count++] = item->valuestring;
    }
    if (!valid) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        return true;
    }
    const char *error = s_default_extra_hostnames.set(names, count, s_default_extra_hostnames.ctx);
    if (error) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid host names";
        dispatch->error_data = error;
        return true;
    }
    return hostnames_result(dispatch);
}

#include "control_protocol_internal.h"
#include <string.h>

/* A build without the HTTPS listener registers no TLS provider. It does not
 * report the "tls" capability either, so the UI neither shows the HTTPS
 * setting nor calls these methods; its tests skip the feature. */
static bool tls_missing(const jetkvm_tls_context_t *tls, rpc_dispatch_result_t *dispatch)
{
    if (tls->get || tls->set) return false;
    dispatch->error_code = -32601;
    dispatch->error_message = "Method not found";
    return true;
}

bool rpc_get_tls_state(const jetkvm_control_context_t *context, const cJSON *params,
                       rpc_dispatch_result_t *dispatch)
{
    (void)params;
    const jetkvm_tls_context_t *tls = context->tls.get ? &context->tls : &s_default_tls;
    if (tls_missing(tls, dispatch)) return true;
    char mode[16] = {0};
    char *certificate = cJSON_malloc(8193);
    bool ok = certificate && tls->get && tls->get(mode, sizeof(mode), certificate, 8193, tls->ctx);
    cJSON *result = ok ? cJSON_CreateObject() : NULL;
    ok = result && cJSON_AddStringToObject(result, "mode", mode) &&
        cJSON_AddStringToObject(result, "certificate", certificate) &&
        cJSON_AddStringToObject(result, "privateKey", "");
    cJSON_free(certificate);
    if (!ok) {
        cJSON_Delete(result);
        dispatch->error_code = -32603;
        dispatch->error_message = "TLS state unavailable";
    } else dispatch->result = result;
    return true;
}

bool rpc_set_tls_state(const jetkvm_control_context_t *context, const cJSON *params,
                       rpc_dispatch_result_t *dispatch)
{
    const jetkvm_tls_context_t *tls = context->tls.set ? &context->tls : &s_default_tls;
    if (tls_missing(tls, dispatch)) return true;
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(params, "state");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(state, "mode");
    const cJSON *cert = cJSON_GetObjectItemCaseSensitive(state, "certificate");
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(state, "privateKey");
    if (!cJSON_IsObject(state) || !cJSON_IsString(mode) ||
        (cert && !cJSON_IsString(cert)) || (key && !cJSON_IsString(key))) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid TLS state";
        return true;
    }
    strcpy(dispatch->error_storage, "TLS operation unavailable");
    if (!tls->set || !tls->set(mode->valuestring, cert ? cert->valuestring : "",
                              key ? key->valuestring : "", dispatch->error_storage, sizeof(dispatch->error_storage), tls->ctx)) {
        dispatch->error_code = -32603;
        dispatch->error_message = "TLS configuration failed";
        dispatch->error_data = dispatch->error_storage;
        return true;
    }
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}

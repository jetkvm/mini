#include "control_protocol_internal.h"
#include <string.h>

/* Upstream rpcCountsAsLocalAuthActivity: calls that only read state do not
 * count, because the UI polls several of them. */
bool jetkvm_control_rpc_counts_as_activity(const char *method)
{
    static const char *const readers[] = {"get", "list", "check", "is"};
    if (!method) return false;
    for (size_t i = 0; i < sizeof(readers) / sizeof(readers[0]); ++i) {
        if (strncmp(method, readers[i], strlen(readers[i])) == 0) return false;
    }
    return strcmp(method, "ping") != 0;
}

/* Upstream hidMessageCountsAsLocalAuthActivity: input, not the keepalive of
 * a held key. The handshake makes no event. */
bool jetkvm_control_hidrpc_counts_as_activity(jetkvm_hidrpc_event_type_t type)
{
    return type != JETKVM_HIDRPC_EVENT_KEYPRESS_KEEPALIVE_REPORT;
}

static bool session_timeout_missing(rpc_dispatch_result_t *dispatch)
{
    if (s_default_session_timeout.get && s_default_session_timeout.set) return false;
    dispatch->error_code = -32601;
    dispatch->error_message = "Method not found";
    return true;
}

bool rpc_get_local_auth_session_timeout(const jetkvm_control_context_t *context,
                                        const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    (void)context;
    if (session_timeout_missing(dispatch)) return true;
    if (!require_no_rpc_params(params, dispatch)) return true;
    dispatch->result = cJSON_CreateNumber(s_default_session_timeout.get(s_default_session_timeout.ctx));
    return dispatch->result != NULL;
}

bool rpc_set_local_auth_session_timeout(const jetkvm_control_context_t *context,
                                        const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    (void)context;
    if (session_timeout_missing(dispatch)) return true;
    int64_t minutes = 0;
    if (!json_integer_in_range(rpc_param(params, "timeoutMinutes"), 0,
                               JETKVM_SESSION_TIMEOUT_MAX_MINUTES, &minutes)) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "session timeout must be between 0 and 1440 minutes";
        return true;
    }
    const char *error = s_default_session_timeout.set((unsigned)minutes, s_default_session_timeout.ctx);
    if (error) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = error;
        return true;
    }
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}

#include "jetkvm_tailscale.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static jetkvm_tailscale_config_t saved = {
    .enabled = true, .control_url = JETKVM_TAILSCALE_DEFAULT_CONTROL_URL,
    .auth_key = "tskey-auth-secret-never-return",
};
static unsigned mutations;
static bool get_config(jetkvm_tailscale_config_t *config, void *ctx)
{
    (void)ctx;
    *config = saved;
    return true;
}
static const char *set_config(const jetkvm_tailscale_config_t *config, void *ctx)
{
    (void)ctx;
    const char *error = jetkvm_tailscale_validate_config(config);
    if (error) return error;
    saved = *config;
    ++mutations;
    return NULL;
}
static bool get_status(jetkvm_tailscale_status_t *status, void *ctx)
{
    (void)ctx;
    *status = (jetkvm_tailscale_status_t){
        .installed = true, .enabled = true, .running = true, .auth_key_set = true,
        .backend_state = "Running", .control_url = JETKVM_TAILSCALE_DEFAULT_CONTROL_URL,
        .hostname = "mini", .dns_name = "mini.example.ts.net", .ipv4 = "100.90.1.2",
        .logged_in = true,
    };
    return true;
}

static unsigned logouts;
static const char *logout_error;
static const char *logout(void *ctx)
{
    (void)ctx;
    ++logouts;
    return logout_error;
}

static cJSON *rpc(const jetkvm_control_context_t *context, const char *method, const char *params)
{
    char request[1024], response[2048];
    size_t length = 0;
    snprintf(request, sizeof(request), "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}", method, params);
    assert(jetkvm_control_handle_rpc_with_context(context, (const uint8_t *)request,
        strlen(request), response, sizeof(response), &length) == JETKVM_CONTROL_RESPONSE_READY);
    assert(!strstr(response, "secret-never-return"));
    cJSON *json = cJSON_Parse(response);
    assert(json);
    return json;
}

int main(void)
{
    extern void test_packet_filter(void);
    test_packet_filter();
    extern void test_h2_message(void);
    test_h2_message();
    extern void test_peer_map(void);
    test_peer_map();
    extern void test_peer_select(void);
    test_peer_select();
    extern void test_key_expiry(void);
    test_key_expiry();
    char output[128], host[64];
    bool tls;
    uint16_t port;
    assert(!jetkvm_tailscale_normalize_url("  https://headscale.example:8443/ \n", output,
        sizeof(output), host, sizeof(host), &tls, &port));
    assert(!strcmp(output, "https://headscale.example:8443") &&
        !strcmp(host, "headscale.example") && tls && port == 8443);
    assert(!jetkvm_tailscale_normalize_url("", output, sizeof(output), host, sizeof(host), &tls, &port));
    assert(!strcmp(output, JETKVM_TAILSCALE_DEFAULT_CONTROL_URL) && tls && port == 443);
    const char *invalid[] = {"https://", "ftp://example.com", "http://192.168.1.3:8080", "https://user@example.com",
        "https://example.com/path", "https://example.com?x", "https://example.com#x",
        "https://example.com:", "https://example.com:0", "https://example.com:65536",
        "https://example.com:999999999999999999999", "https://a b", "https://example.com//",
        "https://-example.com", "https://example.com:443/extra", "https://example.com%00"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(jetkvm_tailscale_normalize_url(invalid[i], output, sizeof(output), host, sizeof(host), &tls, &port));
    assert(jetkvm_tailscale_normalize_url("https://example.com", output, 8, host, sizeof(host), &tls, &port));

    jetkvm_tailscale_config_t legacy = {.control_url = "https://headscale.example:8080",
        .auth_key = "0123456789abcdef0123456789abcdef0123456789abcdef"};
    assert(!jetkvm_tailscale_validate_config(&legacy));
    strcpy(legacy.control_url, JETKVM_TAILSCALE_DEFAULT_CONTROL_URL);
    assert(jetkvm_tailscale_validate_config(&legacy));
    strcpy(legacy.control_url, "https://headscale.example:8080");
    legacy.auth_key[0] = 'z';
    assert(jetkvm_tailscale_validate_config(&legacy));

    jetkvm_control_context_t context = {.tailscale = {
        .get_config = get_config, .set_config = set_config, .get_status = get_status,
    }};
    cJSON *reply = rpc(&context, "getTailscaleStatus", "{}");
    cJSON *result = cJSON_GetObjectItemCaseSensitive(reply, "result");
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "running")));
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "loggedIn")));
    cJSON *self = cJSON_GetObjectItemCaseSensitive(result, "self");
    cJSON *ips = cJSON_GetObjectItemCaseSensitive(self, "tailscaleIPs");
    assert(!strcmp(cJSON_GetArrayItem(ips, 0)->valuestring, "100.90.1.2"));
    cJSON_Delete(reply);
    reply = rpc(&context, "getTailscaleConfig", "{}");
    result = cJSON_GetObjectItemCaseSensitive(reply, "result");
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "authKeySet")));
    assert(!cJSON_GetObjectItemCaseSensitive(result, "authKey"));
    cJSON_Delete(reply);

    const char *bad_params[] = {"null", "[]", "{\"enabled\":1}", "{\"enabled\":false,\"extra\":true}",
        "{\"enabled\":true,\"authKey\":123}", "{\"enabled\":false,\"enabled\":true}",
        "{\"enabled\":true,\"authKey\":\"tskey-api-incorrect\"}"};
    for (unsigned i = 0; i < sizeof(bad_params) / sizeof(bad_params[0]); ++i) {
        reply = rpc(&context, "setTailscaleConfig", bad_params[i]);
        assert(cJSON_GetObjectItemCaseSensitive(reply, "error"));
        assert(mutations == 0 && saved.enabled);
        cJSON_Delete(reply);
    }
    reply = rpc(&context, "setTailscaleControlURL", "{\"controlURL\":\"https://user@example.com\"}");
    assert(cJSON_GetObjectItemCaseSensitive(reply, "error") && mutations == 0);
    cJSON_Delete(reply);
    reply = rpc(&context, "setTailscaleConfig", "{\"enabled\":false}");
    assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(reply, "result")));
    assert(mutations == 1 && !saved.enabled && !strcmp(saved.auth_key, "tskey-auth-secret-never-return"));
    cJSON_Delete(reply);
    reply = rpc(&context, "setTailscaleConfig", "{\"enabled\":false,\"authKey\":\"\"}");
    assert(mutations == 2 && !saved.auth_key[0]);
    cJSON_Delete(reply);
    /* Log out takes no parameters, and a firmware without the hook does not
     * offer it; an erase that fails is an error, not a success. */
    reply = rpc(&context, "logoutTailscale", "{}");
    assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(reply, "error"), "code")->valueint == -32601);
    cJSON_Delete(reply);
    context.tailscale.logout = logout;
    reply = rpc(&context, "logoutTailscale", "{\"authKey\":\"\"}");
    assert(cJSON_GetObjectItemCaseSensitive(reply, "error") && logouts == 0);
    cJSON_Delete(reply);
    reply = rpc(&context, "logoutTailscale", "{}");
    assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(reply, "result")) && logouts == 1);
    cJSON_Delete(reply);
    logout_error = "Could not erase the Tailscale identity";
    reply = rpc(&context, "logoutTailscale", "{}");
    assert(cJSON_GetObjectItemCaseSensitive(reply, "error") && logouts == 2);
    cJSON_Delete(reply);
    jetkvm_control_context_t none = {0};
    reply = rpc(&none, "getTailscaleStatus", "{}");
    cJSON *error = cJSON_GetObjectItemCaseSensitive(reply, "error");
    assert(cJSON_GetObjectItemCaseSensitive(error, "code")->valueint == -32601);
    cJSON_Delete(reply);
    puts("Tailscale URL validation, RPC boundaries and secret redaction: PASS");
    return 0;
}

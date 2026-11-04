/* Network RPC shapes shared with the Go UI; Mini capabilities are explicit. */
#include "control_protocol_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const struct { const char *key; const char *value; } fixed_settings[] = {
    {"dhcp_client", "lwip"}, {"domain", "local"}, {"http_proxy", ""},
    {"ipv6_mode", "disabled"}, {"lldp_mode", "disabled"},
    {"mdns_mode", "disabled"},
};
/* The shared form always submits the mode; the Mini has NTP with user servers
 * (upstream "custom") or public NTP only. HTTP time sources do not exist here. */
static const char *time_sync_mode_name(const jetkvm_network_settings_t *settings)
{
    return settings->time_sync_custom ? "custom" : "ntp_only";
}

static cJSON *settings_json(const jetkvm_network_settings_t *settings)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *readonly = cJSON_AddArrayToObject(result, "read_only_fields");
    if (!result || !readonly ||
        !cJSON_AddStringToObject(result, "hostname", settings->hostname) ||
        !cJSON_AddStringToObject(result, "ipv4_mode", settings->dhcp ? "dhcp" : "static")) goto fail;
    for (size_t i = 0; i < sizeof(fixed_settings) / sizeof(fixed_settings[0]); ++i) {
        if (!cJSON_AddStringToObject(result, fixed_settings[i].key, fixed_settings[i].value) ||
            !cJSON_AddItemToArray(readonly, cJSON_CreateString(fixed_settings[i].key))) goto fail;
    }
    if (!cJSON_AddItemToArray(readonly, cJSON_CreateString("tailscale"))) goto fail;
    cJSON *servers = NULL;
    if (!cJSON_AddStringToObject(result, "time_sync_mode", time_sync_mode_name(settings)) ||
        !(servers = cJSON_AddArrayToObject(result, "time_sync_ntp_servers")) ||
        !cJSON_AddBoolToObject(result, "time_sync_disable_fallback", settings->time_sync_disable_fallback)) goto fail;
    for (size_t i = 0; i < JETKVM_TIME_SYNC_MAX_SERVERS; ++i) {
        if (settings->time_sync_ntp_servers[i][0] &&
            !cJSON_AddItemToArray(servers, cJSON_CreateString(settings->time_sync_ntp_servers[i]))) goto fail;
    }
    if (settings->address[0]) {
        cJSON *ipv4 = cJSON_AddObjectToObject(result, "ipv4_static");
        if (!ipv4 || !cJSON_AddStringToObject(ipv4, "address", settings->address) ||
            !cJSON_AddStringToObject(ipv4, "netmask", settings->netmask) ||
            !cJSON_AddStringToObject(ipv4, "gateway", settings->gateway)) goto fail;
        cJSON *dns = cJSON_AddArrayToObject(ipv4, "dns");
        if (!dns) goto fail;
        for (size_t i = 0; i < 3; ++i) {
            if (settings->dns[i][0] && !cJSON_AddItemToArray(dns, cJSON_CreateString(settings->dns[i]))) goto fail;
        }
    }
    return result;
fail:
    cJSON_Delete(result);
    return NULL;
}

bool rpc_get_network_settings(const jetkvm_control_context_t *context,
                             const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    (void)params;
    jetkvm_network_settings_t settings = context->network_settings;
    if (context->get_network_settings &&
        !context->get_network_settings(&settings, context->network_ctx)) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Network settings unavailable";
        return true;
    }
    dispatch->result = settings_json(&settings);
    return dispatch->result != NULL;
}

/* The form keeps blank entries for rows the user never filled in. */
static bool blank_strings(const cJSON *array)
{
    const cJSON *item;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsString(item) || item->valuestring[0]) return false;
    }
    return true;
}
static bool absent(const cJSON *value)
{
    return !value || cJSON_IsNull(value);
}
/* Returns NULL when parsed, otherwise the unsupported field name (a Mini
 * capability gap) or "" for malformed input. */
static const char *parse_time_sync(const cJSON *settings, jetkvm_network_settings_t *parsed)
{
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(settings, "time_sync_mode");
    if (!absent(mode)) {
        if (!cJSON_IsString(mode)) return "";
        if (strcmp(mode->valuestring, "custom") == 0) parsed->time_sync_custom = true;
        else if (strcmp(mode->valuestring, "ntp_and_http") == 0 ||
                 strcmp(mode->valuestring, "http_only") == 0) return "time_sync_mode";
        else if (strcmp(mode->valuestring, "ntp_only") != 0) return "";
    }
    const cJSON *fallback = cJSON_GetObjectItemCaseSensitive(settings, "time_sync_disable_fallback");
    if (!absent(fallback)) {
        if (!cJSON_IsBool(fallback)) return "";
        parsed->time_sync_disable_fallback = cJSON_IsTrue(fallback);
    }
    const cJSON *servers = cJSON_GetObjectItemCaseSensitive(settings, "time_sync_ntp_servers");
    if (!absent(servers)) {
        if (!cJSON_IsArray(servers)) return "";
        size_t count = 0;
        const cJSON *item;
        cJSON_ArrayForEach(item, servers) {
            if (!cJSON_IsString(item)) return "";
            if (!item->valuestring[0]) continue;
            if (count == JETKVM_TIME_SYNC_MAX_SERVERS ||
                strlen(item->valuestring) >= JETKVM_TIME_SYNC_SERVER_CAPACITY) return "time_sync_ntp_servers";
            strcpy(parsed->time_sync_ntp_servers[count++], item->valuestring);
        }
    }
    const cJSON *urls = cJSON_GetObjectItemCaseSensitive(settings, "time_sync_http_urls");
    if (!absent(urls) && (!cJSON_IsArray(urls) || !blank_strings(urls))) return "time_sync_http_urls";
    const cJSON *ordering = cJSON_GetObjectItemCaseSensitive(settings, "time_sync_ordering");
    if (!absent(ordering) && (!cJSON_IsArray(ordering) || ordering->child)) return "time_sync_ordering";
    const cJSON *parallel = cJSON_GetObjectItemCaseSensitive(settings, "time_sync_parallel");
    if (!absent(parallel) && !cJSON_IsNumber(parallel)) return "";
    return NULL;
}

static bool copy_string(const cJSON *object, const char *key, char *output, size_t capacity)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(value) || strlen(value->valuestring) >= capacity) return false;
    memcpy(output, value->valuestring, strlen(value->valuestring) + 1);
    return true;
}

bool rpc_set_network_settings(const jetkvm_control_context_t *context,
                             const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(params, "settings");
    jetkvm_network_settings_t parsed = {0};
    char mode[8];
    if (!cJSON_IsObject(settings) ||
        !copy_string(settings, "hostname", parsed.hostname, sizeof(parsed.hostname)) ||
        !copy_string(settings, "ipv4_mode", mode, sizeof(mode)) ||
        (strcmp(mode, "dhcp") != 0 && strcmp(mode, "static") != 0)) goto invalid;
    parsed.dhcp = strcmp(mode, "dhcp") == 0;
    for (size_t i = 0; i < sizeof(fixed_settings) / sizeof(fixed_settings[0]); ++i) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(settings, fixed_settings[i].key);
        if (value && !cJSON_IsNull(value) &&
            (!cJSON_IsString(value) || strcmp(value->valuestring, fixed_settings[i].value) != 0)) {
            dispatch->error_code = -32602;
            dispatch->error_message = "This network setting is not supported on Mini";
            dispatch->error_data = fixed_settings[i].key;
            return true;
        }
    }
    const char *unsupported = parse_time_sync(settings, &parsed);
    if (unsupported && !unsupported[0]) goto invalid;
    if (unsupported) {
        dispatch->error_code = -32602;
        dispatch->error_message = strcmp(unsupported, "time_sync_ntp_servers") == 0 ?
            "Mini supports at most three NTP servers" : "This network setting is not supported on Mini";
        dispatch->error_data = unsupported;
        return true;
    }
    /* The UI submits empty forms for disabled features. Reject actual values,
     * including unknown settings, rather than silently reporting them saved. */
    static const char *const time_sync_fields[] = {
        "time_sync_mode", "time_sync_disable_fallback", "time_sync_ntp_servers",
        "time_sync_http_urls", "time_sync_ordering", "time_sync_parallel",
    };
    const cJSON *field;
    cJSON_ArrayForEach(field, settings) {
        bool known = strcmp(field->string, "hostname") == 0 ||
            strcmp(field->string, "ipv4_mode") == 0 ||
            strcmp(field->string, "ipv4_static") == 0 ||
            strcmp(field->string, "read_only_fields") == 0;
        for (size_t i = 0; i < sizeof(time_sync_fields) / sizeof(time_sync_fields[0]); ++i) {
            known |= strcmp(field->string, time_sync_fields[i]) == 0;
        }
        for (size_t i = 0; i < sizeof(fixed_settings) / sizeof(fixed_settings[0]); ++i) {
            known |= strcmp(field->string, fixed_settings[i].key) == 0;
        }
        if (known) continue;
        if (strcmp(field->string, "ipv6_static") == 0 && cJSON_IsObject(field)) {
            const cJSON *part;
            cJSON_ArrayForEach(part, field) {
                bool empty_text = (strcmp(part->string, "prefix") == 0 || strcmp(part->string, "gateway") == 0) &&
                    cJSON_IsString(part) && !part->valuestring[0];
                bool empty_dns = strcmp(part->string, "dns") == 0 && cJSON_IsArray(part) && !part->child;
                if (!empty_text && !empty_dns) goto invalid;
            }
            continue;
        }
        if (strcmp(field->string, "lldp_tx_tlvs") == 0 && cJSON_IsArray(field) && !field->child) continue;
        goto invalid;
    }
    const cJSON *ipv4 = cJSON_GetObjectItemCaseSensitive(settings, "ipv4_static");
    /* The shared form also sends lease-derived fields in DHCP mode. They do
     * not configure a static address and must not constrain a DHCP save. */
    if (!parsed.dhcp && ipv4 && !cJSON_IsNull(ipv4)) {
        if (!cJSON_IsObject(ipv4) ||
            !copy_string(ipv4, "address", parsed.address, sizeof(parsed.address)) ||
            !copy_string(ipv4, "gateway", parsed.gateway, sizeof(parsed.gateway))) goto invalid;
        if (!copy_string(ipv4, "netmask", parsed.netmask, sizeof(parsed.netmask))) {
            dispatch->error_code = -32602;
            dispatch->error_message = "Netmask must use dotted-decimal notation";
            return true;
        }
        const cJSON *dns = cJSON_GetObjectItemCaseSensitive(ipv4, "dns");
        if (!cJSON_IsArray(dns)) goto invalid;
        if (cJSON_GetArraySize(dns) > 3) {
            dispatch->error_code = -32602;
            dispatch->error_message = "Mini supports at most three DNS servers";
            return true;
        }
        for (int i = 0; i < cJSON_GetArraySize(dns); ++i) {
            const cJSON *value = cJSON_GetArrayItem(dns, i);
            if (!cJSON_IsString(value) || strlen(value->valuestring) >= sizeof(parsed.dns[i])) goto invalid;
            strcpy(parsed.dns[i], value->valuestring);
        }
    }
    const char *error = context->set_network_settings ?
        context->set_network_settings(&parsed, context->network_ctx) : "Network settings unavailable";
    if (error) {
        dispatch->error_code = -32602;
        dispatch->error_message = error;
        return true;
    }
    /* Reply with what was saved, like upstream; the setter may fill a default hostname. */
    if (context->get_network_settings) (void)context->get_network_settings(&parsed, context->network_ctx);
    dispatch->result = settings_json(&parsed);
    return dispatch->result != NULL;
invalid:
    dispatch->error_code = -32602;
    dispatch->error_message = "Invalid network settings";
    return true;
}

bool rpc_renew_dhcp_lease(const jetkvm_control_context_t *context,
                        const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    if (params && !cJSON_IsNull(params) &&
        (!cJSON_IsObject(params) || params->child)) {
        dispatch->error_code = -32602;
        dispatch->error_message = "renewDHCPLease does not accept parameters";
        return true;
    }
    const char *error = context->renew_dhcp_lease ?
        context->renew_dhcp_lease(context->network_ctx) : "DHCP renewal unavailable";
    if (error) {
        dispatch->error_code = -32603;
        dispatch->error_message = error;
        return true;
    }
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}

/* The MAC address setting: "default" is the factory MAC of eFuse, "espressif"
 * the chip's own. Without a factory MAC only `available` (false) is reported. */
static bool mac_address_reply(const jetkvm_control_context_t *context, rpc_dispatch_result_t *dispatch)
{
    jetkvm_mac_address_settings_t settings = {0};
    if (context->get_mac_address_settings &&
        !context->get_mac_address_settings(&settings, context->network_ctx)) {
        dispatch->error_code = -32603;
        dispatch->error_message = "MAC address setting unavailable";
        return true;
    }
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddBoolToObject(result, "available", settings.available) ||
        (settings.available &&
         (!cJSON_AddStringToObject(result, "source", settings.espressif ? "espressif" : "default") ||
          !cJSON_AddStringToObject(result, "default_mac", settings.default_mac) ||
          !cJSON_AddStringToObject(result, "espressif_mac", settings.espressif_mac)))) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

bool rpc_get_mac_address_settings(const jetkvm_control_context_t *context,
                                  const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    (void)params;
    return mac_address_reply(context, dispatch);
}

bool rpc_set_mac_address_settings(const jetkvm_control_context_t *context,
                                  const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(params, "source");
    if (!cJSON_IsString(source) ||
        (strcmp(source->valuestring, "default") != 0 && strcmp(source->valuestring, "espressif") != 0)) {
        dispatch->error_code = -32602;
        dispatch->error_message = "source must be \"default\" or \"espressif\"";
        return true;
    }
    const char *error = context->set_mac_address_settings ?
        context->set_mac_address_settings(strcmp(source->valuestring, "espressif") == 0, context->network_ctx) :
        "MAC address setting unavailable";
    if (error) {
        dispatch->error_code = -32602;
        dispatch->error_message = error;
        return true;
    }
    return mac_address_reply(context, dispatch);
}

static cJSON *network_state_json(const jetkvm_network_state_t *state)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *ipv4 = cJSON_AddArrayToObject(result, "ipv4_addresses");
    if (!result || !ipv4 || !cJSON_AddArrayToObject(result, "ipv6_addresses") ||
        !cJSON_AddStringToObject(result, "interface_name", state->interface_name) ||
        !cJSON_AddStringToObject(result, "hostname", state->hostname) ||
        !cJSON_AddStringToObject(result, "mac_address", state->mac_address) ||
        !cJSON_AddBoolToObject(result, "up", state->up) ||
        !cJSON_AddBoolToObject(result, "online", state->online) ||
        !cJSON_AddBoolToObject(result, "ipv4_ready", state->ipv4_ready) ||
        !cJSON_AddBoolToObject(result, "ipv6_ready", false) ||
        !cJSON_AddStringToObject(result, "last_updated", state->last_updated)) goto fail;
    if (state->ipv4_ready &&
        (!cJSON_AddStringToObject(result, "ipv4_address", state->ipv4_address) ||
         !cJSON_AddItemToArray(ipv4, cJSON_CreateString(state->ipv4_address)))) goto fail;
    cJSON *lease = cJSON_AddObjectToObject(result, "dhcp_lease");
    if (!lease) goto fail;
    if (state->dhcp && state->lease_valid) {
        if (!lease || !cJSON_AddStringToObject(lease, "ip", state->ipv4_address) ||
            !cJSON_AddStringToObject(lease, "netmask", state->netmask) ||
            !cJSON_AddStringToObject(lease, "hostname", state->hostname) ||
            !cJSON_AddStringToObject(lease, "server_id", state->lease_server) ||
            !cJSON_AddStringToObject(lease, "dhcp_client", "lwip") ||
            !cJSON_AddNumberToObject(lease, "lease", (double)state->lease_seconds * 1000000000.0)) goto fail;
        if (state->lease_expiry[0] && !cJSON_AddStringToObject(lease, "lease_expiry", state->lease_expiry)) goto fail;
        cJSON *routers = cJSON_AddArrayToObject(lease, "routers");
        cJSON *dns = cJSON_AddArrayToObject(lease, "dns_servers");
        if (!routers || !dns ||
            (state->gateway[0] && !cJSON_AddItemToArray(routers, cJSON_CreateString(state->gateway))) ||
            (state->dns_main[0] && !cJSON_AddItemToArray(dns, cJSON_CreateString(state->dns_main))) ||
            (state->dns_backup[0] && !cJSON_AddItemToArray(dns, cJSON_CreateString(state->dns_backup))) ||
            (state->dns_fallback[0] && !cJSON_AddItemToArray(dns, cJSON_CreateString(state->dns_fallback)))) goto fail;
    }
    return result;
fail:
    cJSON_Delete(result);
    return NULL;
}

bool rpc_get_network_state(const jetkvm_control_context_t *context,
                          const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    (void)params;
    if (context->snapshot_network) {
        jetkvm_network_state_t live;
        context->snapshot_network(&live);
        dispatch->result = network_state_json(&live);
    } else {
        dispatch->result = network_state_json(&context->network);
    }
    return dispatch->result != NULL;
}

bool jetkvm_control_render_network_notification(const jetkvm_network_state_t *state,
    char *output, size_t capacity, size_t *length)
{
    cJSON *notification = cJSON_CreateObject();
    cJSON *params = network_state_json(state);
    if (!notification || !params ||
        !cJSON_AddStringToObject(notification, "jsonrpc", "2.0") ||
        !cJSON_AddStringToObject(notification, "method", "networkState") ||
        !cJSON_AddItemToObject(notification, "params", params)) {
        cJSON_Delete(params);
        cJSON_Delete(notification);
        return false;
    }
    bool rendered = cJSON_PrintPreallocated(notification, output, (int)capacity, false);
    cJSON_Delete(notification);
    if (rendered) *length = strlen(output);
    return rendered;
}

static const char *wifi_band_name(jetkvm_wifi_band_t band)
{
    return band == JETKVM_WIFI_BAND_2G ? "2.4ghz" :
           band == JETKVM_WIFI_BAND_5G ? "5ghz" : "auto";
}

bool rpc_get_wifi_settings(const jetkvm_control_context_t *context,
                           const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    (void)params;
    jetkvm_wifi_settings_t settings = {0};
    if (context->get_wifi_settings &&
        !context->get_wifi_settings(&settings, context->network_ctx)) return false;
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddBoolToObject(result, "available", settings.available) ||
        !cJSON_AddBoolToObject(result, "connected", settings.connected) ||
        !cJSON_AddBoolToObject(result, "applying", settings.applying) ||
        !cJSON_AddBoolToObject(result, "apply_failed", settings.apply_failed) ||
        !cJSON_AddStringToObject(result, "band", wifi_band_name(settings.band)) ||
        !cJSON_AddStringToObject(result, "ssid", settings.ssid) ||
        !cJSON_AddNumberToObject(result, "channel", settings.channel) ||
        !cJSON_AddNumberToObject(result, "rssi", settings.rssi) ||
        !cJSON_AddStringToObject(result, "connect_error", settings.connect_error) ||
        !cJSON_AddBoolToObject(result, "provisioning", settings.provisioning) ||
        !cJSON_AddBoolToObject(result, "switching", settings.switching) ||
        !cJSON_AddStringToObject(result, "static_ssid", settings.static_ssid) ||
        !cJSON_AddNumberToObject(result, "max_networks", JETKVM_WIFI_MAX_NETWORKS)) {
        cJSON_Delete(result);
        return false;
    }
    /* Passwords stay on the device; the list carries names only. */
    cJSON *networks = cJSON_AddArrayToObject(result, "networks");
    uint8_t count = settings.network_count <= JETKVM_WIFI_MAX_NETWORKS ?
        settings.network_count : JETKVM_WIFI_MAX_NETWORKS;
    for (uint8_t i = 0; networks && i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddItemToArray(networks, item) ||
            !cJSON_AddStringToObject(item, "ssid", settings.networks[i].ssid) ||
            !cJSON_AddBoolToObject(item, "connected", settings.networks[i].connected) ||
            !cJSON_AddBoolToObject(item, "secure", settings.networks[i].secure)) {
            networks = NULL;
        }
    }
    if (!networks) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

static bool optional_bool(const cJSON *params, const char *name, bool *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(params, name);
    if (!item) { *value = false; return true; }
    if (!cJSON_IsBool(item)) return false;
    *value = cJSON_IsTrue(item);
    return true;
}

static void wifi_error(rpc_dispatch_result_t *dispatch, int code, const char *message)
{
    dispatch->error_code = code;
    dispatch->error_message = message;
}

/* As Go: [{"ip", "last_updated"}]. A refresh reaches the cloud only when the
 * device is adopted, so an unadopted device sends nothing to it. */
bool rpc_get_public_ip_addresses(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch)
{
    bool refresh;
    if (!optional_bool(params, "refresh", &refresh)) {
        dispatch->error_code = -32602;
        dispatch->error_message = "refresh must be a boolean";
        return true;
    }
    refresh = refresh && context->cloud_registered;
    char ip[46] = "";
    int64_t last_updated = 0;
    const char *error = context->get_public_ip ?
        context->get_public_ip(refresh, context->cloud_url, ip, sizeof(ip),
                               &last_updated, context->network_ctx) :
        "public IP lookup unavailable";
    if (error) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = error;
        return true;
    }
    cJSON *result = cJSON_CreateArray();
    if (result && ip[0] != '\0') {
        char updated[sizeof("YYYY-MM-DDTHH:MM:SSZ")];
        time_t seconds = (time_t)last_updated;
        const struct tm *utc = gmtime(&seconds);
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddItemToArray(result, entry);
        if (utc == NULL ||
            strftime(updated, sizeof(updated), "%Y-%m-%dT%H:%M:%SZ", utc) == 0U) {
            updated[0] = '\0';
        }
        if (!cJSON_AddStringToObject(entry, "ip", ip) ||
            !cJSON_AddStringToObject(entry, "last_updated", updated)) {
            cJSON_Delete(result);
            result = NULL;
        }
    }
    dispatch->result = result;
    return result != NULL;
}

bool rpc_scan_wifi_networks(const jetkvm_control_context_t *context,
                            const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    bool refresh;
    if (!optional_bool(params, "refresh", &refresh)) {
        wifi_error(dispatch, -32602, "refresh must be a boolean");
        return true;
    }
    if (!context->scan_wifi_networks) {
        wifi_error(dispatch, -32000, "Wi-Fi settings unavailable");
        return true;
    }
    /* About 1.5 KB; keep it off the peer task stack. */
    jetkvm_wifi_scan_t *scan = calloc(1, sizeof(*scan));
    if (!scan) return false;
    bool ok = context->scan_wifi_networks(refresh, scan, context->network_ctx);
    cJSON *result = ok ? cJSON_CreateObject() : NULL;
    cJSON *networks = result && cJSON_AddBoolToObject(result, "scanning", scan->scanning) ?
        cJSON_AddArrayToObject(result, "networks") : NULL;
    uint8_t count = scan->count <= JETKVM_WIFI_MAX_SCAN_RESULTS ?
        scan->count : JETKVM_WIFI_MAX_SCAN_RESULTS;
    for (uint8_t i = 0; networks && i < count; ++i) {
        const jetkvm_wifi_scan_entry_t *entry = &scan->entries[i];
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddItemToArray(networks, item) ||
            !cJSON_AddStringToObject(item, "ssid", entry->ssid) ||
            !cJSON_AddNumberToObject(item, "rssi", entry->rssi) ||
            !cJSON_AddNumberToObject(item, "channel", entry->channel) ||
            !cJSON_AddBoolToObject(item, "secure", entry->secure) ||
            !cJSON_AddBoolToObject(item, "saved", entry->saved)) {
            networks = NULL;
        }
    }
    free(scan);
    if (!networks) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

bool rpc_save_wifi_network(const jetkvm_control_context_t *context,
                           const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    jetkvm_wifi_network_request_t request = {0};
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(params, "password");
    if (!copy_json_string(cJSON_GetObjectItemCaseSensitive(params, "ssid"),
                          request.ssid, sizeof(request.ssid), false)) {
        wifi_error(dispatch, -32602, "SSID must be 1 to 32 bytes");
        return true;
    }
    if (password && !copy_json_string(password, request.password,
                                      sizeof(request.password), true)) {
        wifi_error(dispatch, -32602, "Password must be at most 64 characters");
        return true;
    }
    size_t password_length = strlen(request.password);
    if (password_length != 0U && password_length < 8U) {
        wifi_error(dispatch, -32602, "Password must be at least 8 characters");
        return true;
    }
    if (!optional_bool(params, "connect", &request.connect)) {
        wifi_error(dispatch, -32602, "connect must be a boolean");
        return true;
    }
    bool connecting = false;
    const char *error = context->save_wifi_network ?
        context->save_wifi_network(&request, &connecting, context->network_ctx) :
        "Wi-Fi settings unavailable";
    /* The request held the password; do not leave it on the stack. */
    memset(&request, 0, sizeof(request));
    if (error) {
        wifi_error(dispatch, -32000, error);
        return true;
    }
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddBoolToObject(result, "connecting", connecting)) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

/* removeWifiNetwork and connectWifiNetwork take one saved SSID. */
static bool rpc_saved_network_call(const cJSON *params, rpc_dispatch_result_t *dispatch,
                                   const char *(*call)(const char *ssid, void *ctx),
                                   void *ctx)
{
    char ssid[JETKVM_WIFI_SSID_CAPACITY];
    if (!copy_json_string(cJSON_GetObjectItemCaseSensitive(params, "ssid"),
                          ssid, sizeof(ssid), false)) {
        wifi_error(dispatch, -32602, "SSID must be 1 to 32 bytes");
        return true;
    }
    const char *error = call ? call(ssid, ctx) : "Wi-Fi settings unavailable";
    if (error) {
        wifi_error(dispatch, -32000, error);
        return true;
    }
    dispatch->result = cJSON_CreateBool(true);
    return dispatch->result != NULL;
}

bool rpc_remove_wifi_network(const jetkvm_control_context_t *context,
                             const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    return rpc_saved_network_call(params, dispatch, context->remove_wifi_network,
                                  context->network_ctx);
}

bool rpc_connect_wifi_network(const jetkvm_control_context_t *context,
                              const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    return rpc_saved_network_call(params, dispatch, context->connect_wifi_network,
                                  context->network_ctx);
}

bool rpc_set_wifi_band(const jetkvm_control_context_t *context,
                       const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(params, "band");
    jetkvm_wifi_band_t band;
    if (!cJSON_IsString(value)) goto invalid;
    if (strcmp(value->valuestring, "auto") == 0) band = JETKVM_WIFI_BAND_AUTO;
    else if (strcmp(value->valuestring, "2.4ghz") == 0) band = JETKVM_WIFI_BAND_2G;
    else if (strcmp(value->valuestring, "5ghz") == 0) band = JETKVM_WIFI_BAND_5G;
    else goto invalid;
    const char *error = context->set_wifi_band ?
        context->set_wifi_band(band, context->network_ctx) : "Wi-Fi settings unavailable";
    if (error) {
        dispatch->error_code = -32000;
        dispatch->error_message = error;
        return true;
    }
    dispatch->result = cJSON_CreateBool(true);
    return dispatch->result != NULL;
invalid:
    dispatch->error_code = -32602;
    dispatch->error_message = "Wi-Fi band must be auto, 2.4ghz or 5ghz";
    return true;
}

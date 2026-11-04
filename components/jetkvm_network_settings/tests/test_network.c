#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "jetkvm_network_settings.h"

static jetkvm_network_settings_t saved;
static unsigned writes, renewals;
static const char *save_error;
static bool get(jetkvm_network_settings_t *settings, void *ctx)
{
    (void)ctx; *settings = saved; return true;
}
static const char *set(const jetkvm_network_settings_t *requested, void *ctx)
{
    (void)ctx;
    /* As jetkvm_network_settings_set: an empty hostname saves the default. */
    jetkvm_network_settings_t settings = *requested;
    (void)jetkvm_network_settings_fill_hostname(&settings, "e8f60a01b2cf");
    const char *error = jetkvm_network_settings_validate(&settings);
    if (error) return error;
    if (save_error) return save_error;
    saved = settings; ++writes; return NULL;
}
static const char *renew(void *ctx)
{
    (void)ctx;
    if (!saved.dhcp) return "DHCP disabled";
    ++renewals; return NULL;
}
static cJSON *rpc(jetkvm_control_context_t *context, const char *method, const cJSON *params)
{
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddStringToObject(request, "method", method);
    cJSON_AddNumberToObject(request, "id", 1);
    if (params) cJSON_AddItemToObject(request, "params", cJSON_Duplicate(params, true));
    char *input = cJSON_PrintUnformatted(request);
    char output[2048]; size_t size;
    assert(jetkvm_control_handle_rpc_with_context(context, (const uint8_t *)input,
        strlen(input), output, sizeof(output), &size) == JETKVM_CONTROL_RESPONSE_READY);
    cJSON_free(input); cJSON_Delete(request);
    cJSON *reply = cJSON_Parse(output); assert(reply); return reply;
}
static bool submit(jetkvm_control_context_t *context, cJSON *settings)
{
    cJSON *params = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "settings", cJSON_Duplicate(settings, true));
    cJSON *reply = rpc(context, "setNetworkSettings", params);
    bool accepted = cJSON_HasObjectItem(reply, "result");
    cJSON_Delete(reply); cJSON_Delete(params); return accepted;
}
static void replace(cJSON *object, const char *key, const char *value)
{
    cJSON_DeleteItemFromObjectCaseSensitive(object, key);
    cJSON_AddStringToObject(object, key, value);
}
static void test_lease_clock(void)
{
    jetkvm_network_lease_clock_t clock = {0};
    const int64_t wall = 1800000000;
    assert(jetkvm_network_lease_expiry(&clock, 86400, false, 1, 0, wall) == wall + 86400);
    assert(jetkvm_network_lease_expiry(&clock, 86400, false, 1, 1000000, wall + 1) == wall + 86400);
    /* Browser absent through automatic renewal: same XID, new age greater
     * than the last observed zero age. Renewal flag is deliberately false. */
    assert(jetkvm_network_lease_expiry(&clock, 80000, false, 1, 50000000000LL, wall + 50000) == wall + 130000);
    assert(jetkvm_network_lease_expiry(&clock, 86400, true, 1, 50001000000LL, wall + 50001) == wall + 136401);
    assert(jetkvm_network_lease_expiry(&clock, 86400, false, 1, 50002000000LL, wall + 50102) == wall + 136501);
    memset(&clock, 0, sizeof(clock));
    assert(jetkvm_network_lease_expiry(&clock, 30, false, 1, 0, wall) == wall + 30);
    assert(jetkvm_network_lease_expiry(&clock, 20, false, 1, 40000000, wall + 40) == wall + 60);
    memset(&clock, 0, sizeof(clock));
    assert(jetkvm_network_lease_expiry(&clock, 300, false, 1, 0, 0) == 0);
    assert(jetkvm_network_lease_expiry(&clock, 300, false, 1, 1000000, wall) == wall + 299);
}

static void test_dhcp_inactive_fields_and_hostname(void)
{
    jetkvm_control_context_t context = {
        .get_network_settings = get, .set_network_settings = set,
    };
    save_error = NULL;
    jetkvm_network_settings_defaults(&saved, "e8f60a01b2cf");
    cJSON *settings = cJSON_CreateObject();
    cJSON_AddStringToObject(settings, "hostname", "mini-dhcp");
    cJSON_AddStringToObject(settings, "ipv4_mode", "dhcp");
    cJSON *ipv4 = cJSON_AddObjectToObject(settings, "ipv4_static");
    cJSON_AddStringToObject(ipv4, "address", "192.168.1.44");
    cJSON_AddStringToObject(ipv4, "gateway", "192.168.2.1");
    cJSON_AddArrayToObject(ipv4, "dns");
    const char *masks[] = {"255.255.255.254", "255.255.255.255", "255.255.255.0"};
    for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); ++i) {
        replace(ipv4, "netmask", masks[i]);
        assert(submit(&context, settings));
        assert(saved.dhcp && !saved.address[0] && !saved.netmask[0] && !saved.gateway[0]);
        replace(settings, "ipv4_mode", "static");
        unsigned before = writes;
        assert(!submit(&context, settings) && writes == before);
        replace(settings, "ipv4_mode", "dhcp");
    }
    /* Disabled form fields must not affect a DHCP hostname save. */
    cJSON *dns = cJSON_GetObjectItemCaseSensitive(ipv4, "dns");
    for (int i = 0; i < 4; ++i) cJSON_AddItemToArray(dns, cJSON_CreateString("stale-field"));
    assert(submit(&context, settings) && !saved.dns[0][0]);
    char hostname[33];
    memset(hostname, 'a', 31); hostname[31] = 0;
    replace(settings, "hostname", hostname);
    assert(submit(&context, settings) && strlen(saved.hostname) == 31);
    hostname[31] = 'a'; hostname[32] = 0;
    replace(settings, "hostname", hostname);
    unsigned before = writes;
    assert(!submit(&context, settings) && writes == before && strlen(saved.hostname) == 31);
    /* A cleared hostname field saves the default name, and the reply
     * carries the saved settings. */
    replace(settings, "hostname", "");
    cJSON *params = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "settings", cJSON_Duplicate(settings, true));
    cJSON *reply = rpc(&context, "setNetworkSettings", params);
    cJSON *result = cJSON_GetObjectItemCaseSensitive(reply, "result");
    assert(result && !strcmp(cJSON_GetObjectItemCaseSensitive(result, "hostname")->valuestring,
                             "jetkvm-e8f60a01b2cf"));
    assert(!strcmp(saved.hostname, "jetkvm-e8f60a01b2cf") && writes == before + 1);
    cJSON_Delete(reply); cJSON_Delete(params);
    cJSON_Delete(settings);

    /* Older DHCP records may include lease-derived static fields. */
    strcpy(saved.address, "192.168.1.44"); strcpy(saved.netmask, "255.255.255.255");
    strcpy(saved.gateway, "192.168.2.1");
    assert(!jetkvm_network_settings_validate(&saved));
    saved.dhcp = false; assert(jetkvm_network_settings_validate(&saved));
    saved.dhcp = true; memset(saved.dns[0], 'x', sizeof(saved.dns[0]));
    assert(jetkvm_network_settings_validate(&saved));
}

static const char *submit_error(jetkvm_control_context_t *context, cJSON *settings, const char **data)
{
    static char message[128], field[64];
    cJSON *params = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "settings", cJSON_Duplicate(settings, true));
    cJSON *reply = rpc(context, "setNetworkSettings", params);
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(reply, "error");
    message[0] = field[0] = 0;
    if (cJSON_IsObject(error)) {
        snprintf(message, sizeof(message), "%s", cJSON_GetObjectItemCaseSensitive(error, "message")->valuestring);
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(error, "data");
        if (cJSON_IsString(value)) snprintf(field, sizeof(field), "%s", value->valuestring);
    }
    cJSON_Delete(reply); cJSON_Delete(params);
    if (data) *data = field;
    return message[0] ? message : NULL;
}

/* The upstream form submits every time-sync field; the Mini keeps NTP only. */
static void test_time_sync_settings(void)
{
    jetkvm_control_context_t context = {
        .get_network_settings = get, .set_network_settings = set,
    };
    save_error = NULL;
    jetkvm_network_settings_defaults(&saved, "e8f60a01b2cf");
    cJSON *reply = rpc(&context, "getNetworkSettings", NULL);
    cJSON *settings = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(reply, "result"), true);
    cJSON_Delete(reply);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(settings, "time_sync_mode")->valuestring, "ntp_only"));
    assert(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(settings, "time_sync_ntp_servers")));
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(settings, "time_sync_disable_fallback")));
    unsigned before = writes;
    /* Round trip of the reported settings changes nothing. */
    assert(submit(&context, settings) && writes == before + 1 && !saved.time_sync_custom);

    /* The e2e reset step: mode ntp_only with empty lists and fallback disabled. */
    cJSON_AddArrayToObject(settings, "time_sync_http_urls");
    cJSON_DeleteItemFromObjectCaseSensitive(settings, "time_sync_disable_fallback");
    cJSON_AddBoolToObject(settings, "time_sync_disable_fallback", true);
    assert(submit(&context, settings) && saved.time_sync_disable_fallback && !saved.time_sync_custom);

    /* Custom mode with one filled row and the form's blank rows. */
    replace(settings, "time_sync_mode", "custom");
    cJSON *servers = cJSON_GetObjectItemCaseSensitive(settings, "time_sync_ntp_servers");
    cJSON_AddItemToArray(servers, cJSON_CreateString(""));
    cJSON_AddItemToArray(servers, cJSON_CreateString("192.168.1.135"));
    cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(settings, "time_sync_http_urls"), cJSON_CreateString(""));
    assert(submit(&context, settings));
    assert(saved.time_sync_custom && !strcmp(saved.time_sync_ntp_servers[0], "192.168.1.135") &&
           !saved.time_sync_ntp_servers[1][0]);
    reply = rpc(&context, "getNetworkSettings", NULL);
    cJSON *reported = cJSON_GetObjectItemCaseSensitive(reply, "result");
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(reported, "time_sync_mode")->valuestring, "custom"));
    cJSON *reported_servers = cJSON_GetObjectItemCaseSensitive(reported, "time_sync_ntp_servers");
    assert(cJSON_GetArraySize(reported_servers) == 1 &&
           !strcmp(cJSON_GetArrayItem(reported_servers, 0)->valuestring, "192.168.1.135"));
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reported, "time_sync_disable_fallback")));
    /* What getNetworkSettings reports must save back unchanged (afterEach restore). */
    before = writes;
    jetkvm_network_settings_t snapshot = saved;
    assert(submit(&context, reported) && writes == before + 1 && !memcmp(&snapshot, &saved, sizeof(saved)));
    cJSON_Delete(reply);

    /* Hostnames are accepted; anything else names the field in the error. */
    cJSON_DeleteItemFromArray(servers, 0);
    cJSON_ReplaceItemInArray(servers, 0, cJSON_CreateString("ntp.lab.test"));
    assert(submit(&context, settings) && !strcmp(saved.time_sync_ntp_servers[0], "ntp.lab.test"));
    const char *data = NULL;
    snapshot = saved; before = writes;
    const char *bad[] = {"bad host", "-ntp.test", "ntp..test", "ntp.test.", "http://ntp.test",
                         "ntp.-lab.test", "ntp-.lab.test"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        cJSON_ReplaceItemInArray(servers, 0, cJSON_CreateString(bad[i]));
        const char *message = submit_error(&context, settings, &data);
        assert(message && strstr(message, "hostname or IPv4"));
    }
    cJSON_ReplaceItemInArray(servers, 0, cJSON_CreateString("ntp.lab.test"));
    for (int i = 0; i < 3; ++i) cJSON_AddItemToArray(servers, cJSON_CreateString("10.0.0.1"));
    assert(strstr(submit_error(&context, settings, &data), "three NTP") && !strcmp(data, "time_sync_ntp_servers"));
    for (int i = 0; i < 3; ++i) cJSON_DeleteItemFromArray(servers, 1);
    assert(writes == before && !memcmp(&snapshot, &saved, sizeof(saved)));

    /* HTTP time sources and orderings are upstream-only features. */
    const char *modes[] = {"ntp_and_http", "http_only"};
    for (size_t i = 0; i < 2; ++i) {
        replace(settings, "time_sync_mode", modes[i]);
        assert(strstr(submit_error(&context, settings, &data), "not supported") && !strcmp(data, "time_sync_mode"));
    }
    replace(settings, "time_sync_mode", "never"); assert(!submit(&context, settings));
    replace(settings, "time_sync_mode", "custom");
    cJSON_ReplaceItemInArray(cJSON_GetObjectItemCaseSensitive(settings, "time_sync_http_urls"), 0,
                             cJSON_CreateString("https://time.test"));
    assert(strstr(submit_error(&context, settings, &data), "not supported") && !strcmp(data, "time_sync_http_urls"));
    cJSON_DeleteItemFromObjectCaseSensitive(settings, "time_sync_http_urls");
    cJSON *ordering = cJSON_AddArrayToObject(settings, "time_sync_ordering");
    assert(submit(&context, settings));
    cJSON_AddItemToArray(ordering, cJSON_CreateString("http"));
    assert(strstr(submit_error(&context, settings, &data), "not supported") && !strcmp(data, "time_sync_ordering"));
    cJSON_DeleteItemFromObjectCaseSensitive(settings, "time_sync_ordering");
    cJSON_AddNumberToObject(settings, "time_sync_parallel", 4);
    assert(submit(&context, settings));
    cJSON_DeleteItemFromObjectCaseSensitive(settings, "time_sync_disable_fallback");
    cJSON_AddStringToObject(settings, "time_sync_disable_fallback", "yes");
    assert(!submit(&context, settings));
    cJSON_Delete(settings);

    /* Only time-sync fields changed: no interface reconfiguration. */
    jetkvm_network_settings_t a, b;
    jetkvm_network_settings_defaults(&a, "e8f60a01b2cf"); b = a;
    b.time_sync_custom = true; strcpy(b.time_sync_ntp_servers[0], "10.0.0.1");
    assert(jetkvm_network_settings_connectivity_equal(&a, &b));
    strcpy(b.hostname, "other");
    assert(!jetkvm_network_settings_connectivity_equal(&a, &b));

    /* Persisted blobs: the pre-time-sync layout loads as public NTP. */
    jetkvm_network_settings_t loaded;
    assert(jetkvm_network_settings_from_blob(&a, JETKVM_NETWORK_SETTINGS_LEGACY_SIZE, &loaded));
    assert(!memcmp(&a, &loaded, sizeof(a)));
    b.time_sync_disable_fallback = true; strcpy(b.hostname, "esp32p4");
    assert(jetkvm_network_settings_from_blob(&b, sizeof(b), &loaded) && !memcmp(&b, &loaded, sizeof(b)));
    assert(!jetkvm_network_settings_from_blob(&b, sizeof(b) - 1, &loaded));
    assert(!jetkvm_network_settings_from_blob(&b, JETKVM_NETWORK_SETTINGS_LEGACY_SIZE + 1, &loaded));
    ((unsigned char *)&b)[JETKVM_NETWORK_SETTINGS_LEGACY_SIZE] = 2;
    assert(!jetkvm_network_settings_from_blob(&b, sizeof(b), &loaded));
    b.time_sync_custom = true;
    memset(b.time_sync_ntp_servers[2], 'n', sizeof(b.time_sync_ntp_servers[2]));
    assert(!jetkvm_network_settings_from_blob(&b, sizeof(b), &loaded));
}

static void test_default_hostname(void)
{
    jetkvm_network_settings_t settings;
    jetkvm_network_settings_defaults(&settings, "e8f60a01b2cf");
    assert(!strcmp(settings.hostname, "jetkvm-e8f60a01b2cf") && settings.dhcp);
    assert(!jetkvm_network_settings_validate(&settings));
    jetkvm_network_settings_defaults(&settings, "");
    assert(!strcmp(settings.hostname, "jetkvm"));

    /* Clearing the hostname field saves the default name; a set name stays. */
    settings.hostname[0] = '\0';
    assert(jetkvm_network_settings_fill_hostname(&settings, "e8f60a01b2cf"));
    assert(!strcmp(settings.hostname, "jetkvm-e8f60a01b2cf"));
    strcpy(settings.hostname, "mini-lab");
    assert(!jetkvm_network_settings_fill_hostname(&settings, "e8f60a01b2cf"));
    assert(!strcmp(settings.hostname, "mini-lab"));

    /* Saved settings with the old factory hostname move to the default. */
    assert(!jetkvm_network_settings_migrate_hostname(&settings, "e8f60a01b2cf"));
    assert(!strcmp(settings.hostname, "mini-lab"));
    strcpy(settings.hostname, "esp32p4");
    assert(jetkvm_network_settings_migrate_hostname(&settings, "e8f60a01b2cf"));
    assert(!strcmp(settings.hostname, "jetkvm-e8f60a01b2cf"));
}

static void test_static_network_binding(void)
{
    jetkvm_network_settings_t settings;
    jetkvm_network_settings_defaults(&settings, "e8f60a01b2cf");
    /* DHCP never applies static settings. */
    assert(!jetkvm_network_settings_static_applies(&settings, "Home", "Home"));
    settings.dhcp = false;
    /* Bound to Home: static on Home, DHCP on any other network. */
    assert(jetkvm_network_settings_static_applies(&settings, "Home", "Home"));
    assert(!jetkvm_network_settings_static_applies(&settings, "Home", "Office"));
    assert(!jetkvm_network_settings_static_applies(&settings, "Home", "home"));
    /* Unbound (Ethernet, or before an upgrade binds it) and no known network. */
    assert(jetkvm_network_settings_static_applies(&settings, "", "Office"));
    assert(jetkvm_network_settings_static_applies(&settings, "Home", NULL));
}

int main(void)
{
    test_static_network_binding();
    test_lease_clock();
    jetkvm_network_settings_defaults(&saved, "e8f60a01b2cf");
    assert(!jetkvm_network_settings_validate(&saved));
    jetkvm_control_context_t context = {
        .get_network_settings = get, .set_network_settings = set, .renew_dhcp_lease = renew,
    };
    cJSON *reply = rpc(&context, "getNetworkSettings", NULL);
    cJSON *settings = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(reply, "result"), true);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(settings, "read_only_fields")) == 7);
    assert(submit(&context, settings)); assert(writes == 1);
    replace(settings, "hostname", "mini-lab");
    cJSON *ipv4 = cJSON_AddObjectToObject(settings, "ipv4_static");
    cJSON_AddStringToObject(ipv4, "address", "192.168.1.44");
    cJSON_AddStringToObject(ipv4, "netmask", "255.255.255.0");
    cJSON_AddStringToObject(ipv4, "gateway", "192.168.1.1");
    cJSON *dns = cJSON_AddArrayToObject(ipv4, "dns");
    cJSON_AddItemToArray(dns, cJSON_CreateString("1.1.1.1"));
    replace(settings, "ipv4_mode", "static");
    assert(submit(&context, settings)); assert(writes == 2 && !saved.dhcp);
    const char *bad_addresses[] = {"192.168.1.0", "192.168.1.255", "127.0.0.1", "256.1.1.1", "192.168.001.44", "224.1.1.1", "1.2.3", "1.2.3.4x"};
    for (size_t i = 0; i < sizeof(bad_addresses) / sizeof(bad_addresses[0]); ++i) {
        replace(ipv4, "address", bad_addresses[i]); assert(!submit(&context, settings));
    }
    replace(ipv4, "address", "192.168.1.44");
    const char *bad_masks[] = {"24", "/24", "192.168.1.0/24", "192.168.100.100/24",
        "", "abc", "255.255.255", "255.0.255.0",
        "0.0.0.0", "255.255.255.254", "255.255.255.255"};
    jetkvm_network_settings_t before = saved;
    for (size_t i = 0; i < sizeof(bad_masks) / sizeof(bad_masks[0]); ++i) {
        replace(ipv4, "netmask", bad_masks[i]);
        cJSON *mask_params = cJSON_CreateObject();
        cJSON_AddItemToObject(mask_params, "settings", cJSON_Duplicate(settings, true));
        cJSON *mask_reply = rpc(&context, "setNetworkSettings", mask_params);
        const cJSON *mask_error = cJSON_GetObjectItemCaseSensitive(mask_reply, "error");
        assert(cJSON_IsObject(mask_error));
        assert(strstr(cJSON_GetObjectItemCaseSensitive(mask_error, "message")->valuestring,
                      i < 7 ? "dotted-decimal" : "/1 to /30"));
        cJSON_Delete(mask_reply); cJSON_Delete(mask_params);
        assert(writes == 2 && memcmp(&before, &saved, sizeof(saved)) == 0);
    }
    replace(ipv4, "netmask", "255.255.255.0");
    for (unsigned i = 0; i < 3; ++i) cJSON_AddItemToArray(dns, cJSON_CreateString("8.8.8.8"));
    cJSON *too_many_params = cJSON_CreateObject();
    cJSON_AddItemToObject(too_many_params, "settings", cJSON_Duplicate(settings, true));
    cJSON *too_many = rpc(&context, "setNetworkSettings", too_many_params);
    assert(strstr(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(too_many,
        "error"), "message")->valuestring, "three DNS"));
    assert(writes == 2 && memcmp(&before, &saved, sizeof(saved)) == 0);
    cJSON_Delete(too_many); cJSON_Delete(too_many_params);
    for (unsigned i = 0; i < 3; ++i) cJSON_DeleteItemFromArray(dns, 1);
    replace(ipv4, "gateway", "192.168.2.1"); assert(!submit(&context, settings));
    replace(ipv4, "gateway", "192.168.1.1");
    replace(settings, "hostname", "-bad"); assert(!submit(&context, settings));
    replace(settings, "hostname", "abcdefghijklmnopqrstuvwxyz1234567"); assert(!submit(&context, settings));
    replace(settings, "hostname", "mini lab"); assert(!submit(&context, settings));
    replace(settings, "hostname", "mini-lab");
    replace(settings, "http_proxy", "http://proxy.test"); assert(!submit(&context, settings));
    replace(settings, "http_proxy", "");
    cJSON_AddBoolToObject(settings, "unknown", true); assert(!submit(&context, settings));
    cJSON_DeleteItemFromObjectCaseSensitive(settings, "unknown");
    cJSON *ipv6 = cJSON_AddObjectToObject(settings, "ipv6_static");
    cJSON_AddStringToObject(ipv6, "prefix", "2001:db8::1/64"); assert(!submit(&context, settings));
    replace(ipv6, "prefix", "");
    cJSON_AddStringToObject(ipv6, "gateway", ""); cJSON_AddArrayToObject(ipv6, "dns");
    assert(submit(&context, settings)); assert(writes == 3);
    save_error = "Persistence failed";
    replace(settings, "hostname", "unsaved"); assert(!submit(&context, settings));
    assert(strcmp(saved.hostname, "mini-lab") == 0 && writes == 3);
    cJSON *renew_reply = rpc(&context, "renewDHCPLease", NULL);
    assert(cJSON_HasObjectItem(renew_reply, "error") && renewals == 0); cJSON_Delete(renew_reply);
    saved.dhcp = true;
    renew_reply = rpc(&context, "renewDHCPLease", NULL);
    assert(cJSON_HasObjectItem(renew_reply, "result") && renewals == 1); cJSON_Delete(renew_reply);
    context.network.dhcp = context.network.lease_valid = context.network.ipv4_ready = true;
    strcpy(context.network.ipv4_address, "192.168.1.44");
    strcpy(context.network.lease_server, "192.168.1.1");
    context.network.lease_seconds = 3600;
    strcpy(context.network.dns_main, "1.1.1.1");
    strcpy(context.network.dns_backup, "8.8.8.8");
    strcpy(context.network.dns_fallback, "9.9.9.9");
    cJSON *state = rpc(&context, "getNetworkState", NULL);
    cJSON *lease = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(state, "result"), "dhcp_lease");
    assert(lease && cJSON_GetObjectItemCaseSensitive(lease, "lease")->valuedouble == 3600000000000.0);
    cJSON *servers = cJSON_GetObjectItemCaseSensitive(lease, "dns_servers");
    assert(cJSON_GetArraySize(servers) == 3);
    assert(!strcmp(cJSON_GetArrayItem(servers, 2)->valuestring, "9.9.9.9"));
    assert(!cJSON_HasObjectItem(lease, "lease_expiry")); /* no synchronized clock */
    cJSON_Delete(state); context.network.lease_valid = false;
    state = rpc(&context, "getNetworkState", NULL);
    assert(!cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(state, "result"), "dhcp_lease")->child);
    cJSON_Delete(state); cJSON_Delete(reply); cJSON_Delete(settings);
    memset(&saved, 0xff, sizeof(saved)); assert(jetkvm_network_settings_validate(&saved));
    test_dhcp_inactive_fields_and_hostname();
    test_default_hostname();
    test_time_sync_settings();
    puts("network validation, RPC round-trip, unsupported fields, time sync and lease tests passed");
}

/*
 * The MAC address setting with the real eFuse read and device ID code of
 * jetkvm_identity over a fake eFuse, a fake IDF MAC table and a fake NVS.
 * mac_address.c is part of this file so that a test can end a boot: its boot
 * state is cleared and NVS is kept, as at a restart.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_efuse.h"
#include "jetkvm_network_settings.h"
#include "../mac_address.c"

/* The P4's factory MAC (device ID and Espressif MAC) and the custom field. */
static const uint8_t P4_FACTORY[6] = {0x60, 0x55, 0xf9, 0xaa, 0xbb, 0xcc};
static const uint8_t JETKVM_MAC[6] = {0x30, 0x52, 0x53, 0x12, 0x34, 0x56};
static const uint8_t BENCH_MAC[6] = {0x02, 0x4a, 0x4b, 0x00, 0x00, 0x01};
static const uint8_t EMPTY[6] = {0};
static uint8_t s_efuse_custom[6];

static uint8_t s_eth[6];
static bool s_eth_set;
static esp_err_t s_iface_error;

const esp_efuse_desc_t *ESP_EFUSE_USER_DATA_MAC_CUSTOM[] = {NULL};
int esp_efuse_get_field_size(const esp_efuse_desc_t *field[])
{
    assert(field == ESP_EFUSE_USER_DATA_MAC_CUSTOM);
    return 48;
}
esp_err_t esp_efuse_read_field_blob(const esp_efuse_desc_t *field[], void *dst, size_t bits)
{
    assert(field == ESP_EFUSE_USER_DATA_MAC_CUSTOM && bits == 48);
    memcpy(dst, s_efuse_custom, 6);
    return ESP_OK;
}
esp_err_t esp_efuse_mac_get_default(uint8_t *mac)
{
    memcpy(mac, P4_FACTORY, 6);
    return ESP_OK;
}
/* As IDF on the P4: ESP_MAC_ETH is the base MAC until it is set. */
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    assert(type == ESP_MAC_ETH);
    memcpy(mac, s_eth_set ? s_eth : P4_FACTORY, 6);
    return ESP_OK;
}
esp_err_t esp_iface_mac_addr_set(const uint8_t *mac, esp_mac_type_t type)
{
    assert(type == ESP_MAC_ETH);
    if (s_iface_error != ESP_OK) return s_iface_error;
    memcpy(s_eth, mac, 6);
    s_eth_set = true;
    return ESP_OK;
}

/* One NVS key, as jk_network/mac_espressif; a write counts after commit. */
static bool s_namespace, s_present, s_staged_present, s_staged;
static uint8_t s_value, s_staged_value;
static bool s_fail_commit;
esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(strcmp(namespace_name, "jk_network") == 0);
    if (mode == NVS_READONLY && !s_namespace) return ESP_ERR_NVS_NOT_FOUND;
    s_namespace = true;
    s_staged = false;
    *handle = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; s_staged = false; }
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    (void)handle;
    assert(strcmp(key, "mac_espressif") == 0);
    if (!s_present) return ESP_ERR_NVS_NOT_FOUND;
    *value = s_value;
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle;
    assert(strcmp(key, "mac_espressif") == 0);
    s_staged = s_staged_present = true;
    s_staged_value = value;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    assert(strcmp(key, "mac_espressif") == 0);
    if (!s_present) return ESP_ERR_NVS_NOT_FOUND;
    s_staged = true;
    s_staged_present = false;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    if (s_fail_commit) return ESP_FAIL;
    if (s_staged) { s_present = s_staged_present; s_value = s_staged_value; }
    return ESP_OK;
}

/* A new boot: the MAC table and this module start over; NVS stays. */
static void boot(const uint8_t custom[6])
{
    memcpy(s_efuse_custom, custom, 6);
    s_eth_set = false;
    s_iface_error = ESP_OK;
    s_decided = s_registered = s_factory_valid = s_boot_espressif = s_saved_espressif = false;
    memset(s_factory, 0, sizeof(s_factory));
    memset(s_espressif, 0, sizeof(s_espressif));
}
static void erase_nvs(void)
{
    s_namespace = s_present = false;
    s_fail_commit = false;
}
static void ethernet_mac(uint8_t mac[6])
{
    jetkvm_mac_address_apply_ethernet();
    assert(esp_read_mac(mac, ESP_MAC_ETH) == ESP_OK);
}

static cJSON *rpc(jetkvm_control_context_t *context, const char *method, const char *params)
{
    char input[256];
    snprintf(input, sizeof(input), "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"id\":1%s%s}",
             method, params ? ",\"params\":" : "", params ? params : "");
    char output[1024];
    size_t size;
    assert(jetkvm_control_handle_rpc_with_context(context, (const uint8_t *)input, strlen(input),
        output, sizeof(output), &size) == JETKVM_CONTROL_RESPONSE_READY);
    cJSON *reply = cJSON_Parse(output);
    assert(reply);
    return reply;
}
static const char *text(const cJSON *object, const char *key)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}
static bool available(jetkvm_control_context_t *context)
{
    cJSON *reply = rpc(context, "getMacAddressSettings", NULL);
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(reply, "result");
    assert(result);
    bool value = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "available"));
    if (!value) assert(!text(result, "source") && !text(result, "default_mac"));
    cJSON_Delete(reply);
    return value;
}

#if CONFIG_JETKVM_BENCH_CUSTOM_MAC
static void test_bench_option(void)
{
    jetkvm_control_context_t context = {
        .get_mac_address_settings = jetkvm_mac_address_settings_get,
        .set_mac_address_settings = jetkvm_mac_address_settings_set,
    };
    /* The test MAC takes the place of eFuse, also of a factory MAC there. */
    const uint8_t *fields[] = {EMPTY, JETKVM_MAC};
    for (size_t i = 0; i < 2; ++i) {
        erase_nvs();
        boot(fields[i]);
        uint8_t mac[6];
        ethernet_mac(mac);
        assert(memcmp(mac, BENCH_MAC, 6) == 0);
        assert(available(&context));
        char id[JETKVM_DEVICE_ID_CAPACITY];
        assert(jetkvm_device_id_get(id, sizeof(id)) == ESP_OK && strcmp(id, "6055f9aabbcc") == 0);
    }
    puts("bench option: the test MAC 02:4a:4b:00:00:01 in place of eFuse, device ID unchanged: OK");
}
#else
static void test_invalid_custom_field_keeps_espressif_mac(void)
{
    jetkvm_control_context_t context = {
        .get_mac_address_settings = jetkvm_mac_address_settings_get,
        .set_mac_address_settings = jetkvm_mac_address_settings_set,
    };
    const uint8_t ones[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t multicast[6] = {0x31, 0x52, 0x53, 0x12, 0x34, 0x56};
    const uint8_t *fields[] = {EMPTY, ones, multicast};
    for (size_t i = 0; i < 3; ++i) {
        for (int espressif = 0; espressif < 2; ++espressif) {
            erase_nvs();
            if (espressif) { s_namespace = s_present = true; s_value = 1; }
            boot(fields[i]);
            uint8_t mac[6];
            ethernet_mac(mac);
            assert(memcmp(mac, P4_FACTORY, 6) == 0);
            assert(!available(&context));
            cJSON *reply = rpc(&context, "setMacAddressSettings", "{\"source\":\"default\"}");
            assert(cJSON_HasObjectItem(reply, "error"));
            cJSON_Delete(reply);
        }
    }
    assert(jetkvm_custom_mac_valid(JETKVM_MAC) && jetkvm_custom_mac_valid(BENCH_MAC));
    assert(!jetkvm_custom_mac_valid(EMPTY) && !jetkvm_custom_mac_valid(ones) &&
           !jetkvm_custom_mac_valid(multicast) && !jetkvm_custom_mac_valid(NULL));
    puts("no valid factory MAC (zero, all ff, multicast): the Espressif MAC, no setting: OK");
}

static void test_factory_mac_and_setting_round_trip(void)
{
    jetkvm_control_context_t context = {
        .get_mac_address_settings = jetkvm_mac_address_settings_get,
        .set_mac_address_settings = jetkvm_mac_address_settings_set,
    };
    erase_nvs();
    boot(JETKVM_MAC);
    uint8_t mac[6];
    ethernet_mac(mac);
    assert(memcmp(mac, JETKVM_MAC, 6) == 0);
    /* The device ID and the default hostname keep the P4's factory MAC. */
    char id[JETKVM_DEVICE_ID_CAPACITY];
    assert(jetkvm_device_id_get(id, sizeof(id)) == ESP_OK && strcmp(id, "6055f9aabbcc") == 0);
    jetkvm_network_settings_t settings;
    jetkvm_network_settings_defaults(&settings, id);
    assert(strcmp(settings.hostname, "jetkvm-6055f9aabbcc") == 0);

    cJSON *reply = rpc(&context, "getMacAddressSettings", NULL);
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(reply, "result");
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result, "available")));
    assert(strcmp(text(result, "source"), "default") == 0);
    assert(strcmp(text(result, "default_mac"), "30:52:53:12:34:56") == 0);
    assert(strcmp(text(result, "espressif_mac"), "60:55:f9:aa:bb:cc") == 0);
    cJSON_Delete(reply);

    reply = rpc(&context, "setMacAddressSettings", "{\"source\":\"espressif\"}");
    result = cJSON_GetObjectItemCaseSensitive(reply, "result");
    assert(result && strcmp(text(result, "source"), "espressif") == 0);
    cJSON_Delete(reply);
    assert(s_present && s_value == 1);
    /* The interface keeps its MAC until the restart. */
    uint8_t same_boot[6];
    assert(jetkvm_mac_address_for_interface(P4_FACTORY, same_boot) && memcmp(same_boot, JETKVM_MAC, 6) == 0);

    boot(JETKVM_MAC);
    ethernet_mac(mac);
    assert(memcmp(mac, P4_FACTORY, 6) == 0);
    reply = rpc(&context, "getMacAddressSettings", NULL);
    result = cJSON_GetObjectItemCaseSensitive(reply, "result");
    assert(strcmp(text(result, "source"), "espressif") == 0);
    assert(strcmp(text(result, "default_mac"), "30:52:53:12:34:56") == 0);
    cJSON_Delete(reply);
    assert(jetkvm_device_id_get(id, sizeof(id)) == ESP_OK && strcmp(id, "6055f9aabbcc") == 0);

    reply = rpc(&context, "setMacAddressSettings", "{\"source\":\"default\"}");
    assert(cJSON_HasObjectItem(reply, "result"));
    cJSON_Delete(reply);
    assert(!s_present);
    boot(JETKVM_MAC);
    ethernet_mac(mac);
    assert(memcmp(mac, JETKVM_MAC, 6) == 0);
    puts("factory MAC: Default and Espressif MAC round trip over RPC, NVS and a restart: OK");
}

static void test_failures_keep_a_working_mac(void)
{
    jetkvm_control_context_t context = {
        .get_mac_address_settings = jetkvm_mac_address_settings_get,
        .set_mac_address_settings = jetkvm_mac_address_settings_set,
    };
    erase_nvs();
    boot(JETKVM_MAC);
    s_iface_error = ESP_ERR_INVALID_ARG;
    uint8_t mac[6];
    ethernet_mac(mac);
    assert(memcmp(mac, P4_FACTORY, 6) == 0);

    s_fail_commit = true;
    cJSON *reply = rpc(&context, "setMacAddressSettings", "{\"source\":\"espressif\"}");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(reply, "error");
    assert(error && strcmp(text(error, "message"), "Could not save the MAC address setting") == 0);
    cJSON_Delete(reply);
    s_fail_commit = false;
    reply = rpc(&context, "getMacAddressSettings", NULL);
    assert(strcmp(text(cJSON_GetObjectItemCaseSensitive(reply, "result"), "source"), "default") == 0);
    cJSON_Delete(reply);

    const char *invalid[] = {"{\"source\":\"chip\"}", "{\"source\":true}", "{}", NULL};
    for (size_t i = 0; i < 4; ++i) {
        reply = rpc(&context, "setMacAddressSettings", invalid[i]);
        error = cJSON_GetObjectItemCaseSensitive(reply, "error");
        assert(error && cJSON_GetObjectItemCaseSensitive(error, "code")->valueint == -32602);
        cJSON_Delete(reply);
    }
    assert(!s_present);
    puts("a failed MAC set or NVS save keeps a working MAC and the saved setting: OK");
}
#endif

int main(void)
{
#if CONFIG_JETKVM_BENCH_CUSTOM_MAC
    test_bench_option();
#else
    test_invalid_custom_field_keeps_espressif_mac();
    test_factory_mac_and_setting_round_trip();
    test_failures_keep_a_working_mac();
#endif
    return 0;
}

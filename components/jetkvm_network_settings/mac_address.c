#include "jetkvm_mac_address.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "jetkvm_identity.h"

static const char *TAG = "mac_address";
#define MAC_NVS_NAMESPACE "jk_network"
#define MAC_NVS_KEY "mac_espressif"

/* Written once at boot by the interface bring-up, before the RPC server
 * starts; the RPC methods read them. Only s_saved_espressif changes later. */
static bool s_decided, s_registered, s_factory_valid, s_boot_espressif, s_saved_espressif;
static uint8_t s_factory[6], s_espressif[6];

static bool saved_espressif(void)
{
    nvs_handle_t nvs;
    uint8_t value = 0;
    if (nvs_open(MAC_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_get_u8(nvs, MAC_NVS_KEY, &value);
    nvs_close(nvs);
    return err == ESP_OK && value == 1U;
}
static void format_mac(const uint8_t mac[6], char *text, size_t capacity)
{
    snprintf(text, capacity, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool jetkvm_mac_address_for_interface(const uint8_t espressif_mac[6], uint8_t mac[6])
{
    if (!espressif_mac || !mac) return false;
    if (!s_decided) {
        s_factory_valid = jetkvm_custom_mac_get(s_factory);
        s_boot_espressif = saved_espressif();
        __atomic_store_n(&s_saved_espressif, s_boot_espressif, __ATOMIC_RELEASE);
        s_decided = true;
    }
    /* A later call of the same boot may find the interface on the factory
     * MAC already; the first call's address is the Espressif MAC. */
    if (!s_registered) memcpy(s_espressif, espressif_mac, sizeof(s_espressif));
    s_registered = true;
    bool factory = s_factory_valid && !s_boot_espressif;
    memcpy(mac, factory ? s_factory : s_espressif, 6);
    char espressif_text[JETKVM_NETWORK_MAC_CAPACITY], factory_text[JETKVM_NETWORK_MAC_CAPACITY];
    format_mac(s_espressif, espressif_text, sizeof(espressif_text));
    format_mac(s_factory, factory_text, sizeof(factory_text));
    if (!s_factory_valid) {
        ESP_LOGI(TAG, "No factory MAC in eFuse; the Espressif MAC %s stays", espressif_text);
    } else if (factory) {
        ESP_LOGI(TAG, "Factory MAC %s in use (Espressif MAC %s)", factory_text, espressif_text);
    } else {
        ESP_LOGI(TAG, "Espressif MAC %s in use, as set (factory MAC %s)", espressif_text, factory_text);
    }
    return factory;
}

void jetkvm_mac_address_apply_ethernet(void)
{
    uint8_t espressif[6], mac[6];
    esp_err_t err = esp_read_mac(espressif, ESP_MAC_ETH);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No Espressif Ethernet MAC (%s); the MAC stays as it is", esp_err_to_name(err));
        return;
    }
    if (!jetkvm_mac_address_for_interface(espressif, mac)) return;
    err = esp_iface_mac_addr_set(mac, ESP_MAC_ETH);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Setting the factory MAC failed (%s); Ethernet uses the Espressif MAC",
                 esp_err_to_name(err));
    }
}

bool jetkvm_mac_address_settings_get(jetkvm_mac_address_settings_t *settings, void *ctx)
{
    (void)ctx;
    if (!settings) return false;
    memset(settings, 0, sizeof(*settings));
    settings->available = s_registered && s_factory_valid;
    if (!settings->available) return true;
    settings->espressif = __atomic_load_n(&s_saved_espressif, __ATOMIC_ACQUIRE);
    format_mac(s_factory, settings->default_mac, sizeof(settings->default_mac));
    format_mac(s_espressif, settings->espressif_mac, sizeof(settings->espressif_mac));
    return true;
}

const char *jetkvm_mac_address_settings_set(bool espressif, void *ctx)
{
    (void)ctx;
    if (!s_registered || !s_factory_valid) return "This device has no factory MAC address";
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MAC_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        /* No key is the default, so a factory reset returns to it. */
        err = espressif ? nvs_set_u8(nvs, MAC_NVS_KEY, 1U) : nvs_erase_key(nvs, MAC_NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Saving the MAC address setting failed: %s", esp_err_to_name(err));
        return "Could not save the MAC address setting";
    }
    __atomic_store_n(&s_saved_espressif, espressif, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "MAC address setting saved: %s from the next boot", espressif ? "Espressif MAC" : "Default");
    return NULL;
}

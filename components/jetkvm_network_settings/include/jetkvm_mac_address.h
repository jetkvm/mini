#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "jetkvm_control.h"

/*
 * The MAC address setting. A production Mini has a factory MAC in eFuse
 * USER_DATA.MAC_CUSTOM (jetkvm_custom_mac_get). The setting chooses between
 * that address ("Default", the default) and the Espressif MAC of the chip; it
 * is the NVS key jk_network/mac_espressif and takes effect at the next boot.
 * Ethernet uses it for ESP_MAC_ETH; the Wi-Fi Mini for the C5's station and
 * its BLE address, so the BLE name JetKVM_XXXXXX follows it. A Mini without a
 * valid factory MAC keeps the Espressif MAC and has no setting. The device ID
 * and the default hostname stay on the factory MAC of the P4
 * (esp_efuse_mac_get_default) in either case.
 */

/*
 * The MAC that the network interface uses from this boot on. `espressif_mac`
 * is the interface's own Espressif MAC; `mac` gets the factory MAC or that
 * address. Returns true when `mac` is the factory MAC, which the caller then
 * sets on the interface before the interface starts. The first call reads
 * eFuse and NVS; later calls of the same boot give the same answer.
 */
bool jetkvm_mac_address_for_interface(const uint8_t espressif_mac[6], uint8_t mac[6]);

/* Ethernet: sets the MAC of jetkvm_mac_address_for_interface for ESP_MAC_ETH.
 * Call before esp_eth_driver_install. Any failure keeps the Espressif MAC. */
void jetkvm_mac_address_apply_ethernet(void);

/* RPC getMacAddressSettings and setMacAddressSettings. */
bool jetkvm_mac_address_settings_get(jetkvm_mac_address_settings_t *settings, void *ctx);
const char *jetkvm_mac_address_settings_set(bool espressif, void *ctx);

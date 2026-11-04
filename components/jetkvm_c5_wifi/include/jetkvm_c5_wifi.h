#pragma once

#include "esp_err.h"
#include "jetkvm_control.h"

/* Provision the ESP32-C5, start ESP-Hosted/SDIO and leave Wi-Fi running. */
esp_err_t jetkvm_c5_wifi_start(void);

/* Cached state; changing the band reconnects asynchronously after the RPC reply. */
bool jetkvm_c5_wifi_get_settings(jetkvm_wifi_settings_t *settings, void *ctx);
const char *jetkvm_c5_wifi_set_band(jetkvm_wifi_band_t band, void *ctx);

/* Saved networks. These run on the RPC task and never wait for the radio:
 * a scan or a connect-now request runs later on the connection task. */
bool jetkvm_c5_wifi_scan_networks(bool refresh, jetkvm_wifi_scan_t *scan, void *ctx);
const char *jetkvm_c5_wifi_save_network(const jetkvm_wifi_network_request_t *request,
                                        bool *connecting, void *ctx);
const char *jetkvm_c5_wifi_remove_network(const char *ssid, void *ctx);
const char *jetkvm_c5_wifi_connect_network(const char *ssid, void *ctx);

/* BLE Wi-Fi setup from the local menu. Start leaves the current network and
 * joins no saved network until a phone sets up a new one, stop is called, or
 * no phone connects for 10 minutes; the band is Auto meanwhile. Stop returns
 * to the saved networks. Both return at once (NULL or an error message). */
const char *jetkvm_c5_wifi_start_setup(void);
const char *jetkvm_c5_wifi_stop_setup(void);

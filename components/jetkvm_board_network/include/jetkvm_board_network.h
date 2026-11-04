#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"
#include "jetkvm_screen.h"

esp_err_t jetkvm_board_network_screen_start(void);
void jetkvm_board_network_ethernet_start(void);
void jetkvm_board_network_screen_status_start(void);

/* Project a Wi-Fi station transition into the shared screen/video network
 * state. Pass ipv4=NULL when disconnected. */
void jetkvm_board_network_set_wifi_state(bool connected,
                                         const uint8_t mac[6],
                                         const esp_ip4_addr_t *ipv4);

/* Refresh the identity and live RSSI of the AP serving the Wi-Fi station. */
void jetkvm_board_network_set_wifi_details(const uint8_t *ssid,
                                           size_t ssid_length,
                                           int8_t rssi);

/* Report BLE Wi-Fi setup for the no-network screen. `code` is NULL until a
 * phone connects, and while provisioning is off `advertising` is false. */
void jetkvm_board_network_set_wifi_setup(bool advertising, const char *code);

/* Report what the connected phone did next: CODE (it must enter the code
 * again), ACCEPTED (it entered the code, or the join it sent failed and it
 * can send another network) or JOINING `ssid`. Ignored unless a phone is
 * connected (the screen is at CODE or a later step). */
void jetkvm_board_network_set_wifi_setup_phone(jetkvm_screen_wifi_setup_t step,
                                               const char *ssid);

/* Report BLE Wi-Fi setup started from the local menu. The screen shows the
 * setup page instead of the home screen, and while `stoppable` (a saved
 * network exists) the top button can end setup. */
void jetkvm_board_network_set_wifi_setup_menu(bool active, bool stoppable);
/* Change only whether menu setup can be stopped, and only while it is still
 * active, so a call that races the end of setup cannot bring it back. */
void jetkvm_board_network_set_wifi_setup_stoppable(bool stoppable);

/* Report that the Wi-Fi station is joining `ssid` ("" while it looks for a
 * saved network), or NULL when it stops. While it joins, the display keeps
 * the status screen instead of the offline HDMI preview. */
void jetkvm_board_network_set_wifi_joining(const char *ssid);

/* Report why the last join failed, as the status screen shows it (for
 * example "Wrong password\nHome"). The device keeps the text until it has an
 * address again. */
void jetkvm_board_network_set_wifi_failure(const char *text);

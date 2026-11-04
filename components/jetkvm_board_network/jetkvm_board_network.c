/* Board network ownership and screen status projection. */
#include "jetkvm_board_network.h"
#include "jetkvm_network_settings.h"
#include "jetkvm_mac_address.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_eth.h"
#include "esp_eth_phy_ip101.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "jetkvm_board_config.h"
#include "jetkvm_cloud_service.h"
#include "jetkvm_device_state.h"
#include "jetkvm_ethernet_stats.h"
#include "jetkvm_screen.h"
#include "jetkvm_usb_debugging.h"
#include "jetkvm_usb_hid.h"
#include "jetkvm_video.h"
#include "lwip/etharp.h"
#include "lwip/esp_netif_net_stack.h"
#include "lwip/netif.h"
#include "webrtc_transport.h"

#define SCREEN_STATUS_CORE          0
#define SCREEN_STATUS_UPDATE_MS     500U
#define SCREEN_STATUS_TASK_STACK    (8 * 1024)

static const char *TAG = "board_network";
static bool s_screen_started;

typedef struct {
    bool link_up;
    bool ipv4_valid;
    bool wifi_connection;
    bool wifi_signal_valid;
    int8_t wifi_rssi;
    char ipv4[JETKVM_SCREEN_IPV4_CAPACITY];
    char mac[JETKVM_SCREEN_MAC_CAPACITY];
    char wifi_ssid[JETKVM_SCREEN_WIFI_SSID_CAPACITY];
    jetkvm_screen_wifi_setup_t wifi_setup;
    char wifi_setup_code[JETKVM_SCREEN_WIFI_SETUP_CODE_CAPACITY];
    /* The network the phone sent, for JETKVM_SCREEN_WIFI_SETUP_JOINING. */
    char wifi_setup_network[JETKVM_SCREEN_WIFI_SSID_CAPACITY];
    bool wifi_joining_active;
    char wifi_joining[JETKVM_SCREEN_WIFI_SSID_CAPACITY];
    char wifi_failure[JETKVM_SCREEN_WIFI_FAILURE_CAPACITY];
    bool wifi_setup_menu;
    bool wifi_setup_stoppable;
    uint32_t wifi_setup_menu_starts;
} screen_network_runtime_t;

static portMUX_TYPE s_screen_network_mux = portMUX_INITIALIZER_UNLOCKED;
static screen_network_runtime_t s_screen_network;

static const char *ETH_TAG = "eth";
static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t *s_eth_netif = NULL;

static void screen_copy_text(char *destination, size_t capacity, const char *source)
{
    if (destination == NULL || capacity == 0U) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    (void)strlcpy(destination, source, capacity);
}

static void screen_network_set_mac(const uint8_t mac[6])
{
    char text[JETKVM_SCREEN_MAC_CAPACITY] = {0};
    (void)snprintf(text, sizeof(text),
                   "%02x:%02x:%02x:%02x:%02x:%02x",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    portENTER_CRITICAL(&s_screen_network_mux);
    memcpy(s_screen_network.mac, text, sizeof(text));
    portEXIT_CRITICAL(&s_screen_network_mux);
}

static void screen_network_set_ipv4(const esp_ip4_addr_t *ip)
{
    char text[JETKVM_SCREEN_IPV4_CAPACITY] = {0};
    bool valid = ip != NULL && ip->addr != 0U && ip->addr != IPADDR_NONE;
    if (valid) {
        (void)snprintf(text, sizeof(text), IPSTR, IP2STR(ip));
    }

    portENTER_CRITICAL(&s_screen_network_mux);
    s_screen_network.ipv4_valid = valid;
    memcpy(s_screen_network.ipv4, text, sizeof(text));
    portEXIT_CRITICAL(&s_screen_network_mux);
}

static void screen_network_set_link(bool link_up, esp_eth_handle_t eth_handle)
{
    uint8_t mac[6] = {0};
    bool have_mac = link_up && eth_handle != NULL &&
                    esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac) == ESP_OK;
    esp_netif_ip_info_t ip_info = {0};
    bool have_ip = link_up && s_eth_netif != NULL &&
                   esp_netif_get_ip_info(s_eth_netif, &ip_info) == ESP_OK &&
                   ip_info.ip.addr != 0U && ip_info.ip.addr != IPADDR_NONE;
    char ipv4[JETKVM_SCREEN_IPV4_CAPACITY] = {0};
    char mac_text[JETKVM_SCREEN_MAC_CAPACITY] = {0};
    if (have_ip) {
        (void)snprintf(ipv4, sizeof(ipv4), IPSTR, IP2STR(&ip_info.ip));
    }
    if (have_mac) {
        (void)snprintf(mac_text, sizeof(mac_text),
                       "%02x:%02x:%02x:%02x:%02x:%02x",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    portENTER_CRITICAL(&s_screen_network_mux);
    s_screen_network.link_up = link_up;
    s_screen_network.ipv4_valid = have_ip;
    s_screen_network.wifi_connection = false;
    s_screen_network.wifi_signal_valid = false;
    s_screen_network.wifi_rssi = 0;
    memcpy(s_screen_network.ipv4, ipv4, sizeof(ipv4));
    s_screen_network.wifi_ssid[0] = '\0';
    if (have_mac) {
        memcpy(s_screen_network.mac, mac_text, sizeof(mac_text));
    }
    portEXIT_CRITICAL(&s_screen_network_mux);

    /* Keep this boundary non-blocking: the video owner observes the atomic
     * state and performs the exclusive preview/H.264 rebuild on its task. */
    jetkvm_video_set_network_available(link_up);
}

static void wifi_update_display_mode(void);

void jetkvm_board_network_set_wifi_state(bool connected,
                                         const uint8_t mac[6],
                                         const esp_ip4_addr_t *ipv4)
{
    const bool have_ip = connected && ipv4 != NULL && ipv4->addr != 0U &&
                         ipv4->addr != IPADDR_NONE;
    char ipv4_text[JETKVM_SCREEN_IPV4_CAPACITY] = {0};
    char mac_text[JETKVM_SCREEN_MAC_CAPACITY] = {0};
    if (have_ip) {
        (void)snprintf(ipv4_text, sizeof(ipv4_text), IPSTR, IP2STR(ipv4));
    }
    if (mac != NULL) {
        (void)snprintf(mac_text, sizeof(mac_text),
                       "%02x:%02x:%02x:%02x:%02x:%02x",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    portENTER_CRITICAL(&s_screen_network_mux);
    s_screen_network.link_up = connected;
    s_screen_network.ipv4_valid = have_ip;
    s_screen_network.wifi_connection = true;
    memcpy(s_screen_network.ipv4, ipv4_text, sizeof(ipv4_text));
    if (!connected) {
        s_screen_network.wifi_signal_valid = false;
        s_screen_network.wifi_rssi = 0;
        s_screen_network.wifi_ssid[0] = '\0';
    }
    if (have_ip) {
        s_screen_network.wifi_failure[0] = '\0';
    }
    if (mac != NULL) {
        memcpy(s_screen_network.mac, mac_text, sizeof(mac_text));
    }
    portEXIT_CRITICAL(&s_screen_network_mux);

    wifi_update_display_mode();
}

void jetkvm_board_network_set_wifi_details(const uint8_t *ssid,
                                           size_t ssid_length,
                                           int8_t rssi)
{
    char ssid_text[JETKVM_SCREEN_WIFI_SSID_CAPACITY] = {0};
    if (ssid != NULL) {
        if (ssid_length >= sizeof(ssid_text)) {
            ssid_length = sizeof(ssid_text) - 1U;
        }
        memcpy(ssid_text, ssid, ssid_length);
    }

    portENTER_CRITICAL(&s_screen_network_mux);
    /* A status RPC may finish just after a disconnect. Do not restore stale
     * AP details after the disconnect handler has cleared them. */
    if (s_screen_network.wifi_connection && s_screen_network.link_up) {
        memcpy(s_screen_network.wifi_ssid, ssid_text, sizeof(ssid_text));
        s_screen_network.wifi_rssi = rssi;
        s_screen_network.wifi_signal_valid = true;
    }
    portEXIT_CRITICAL(&s_screen_network_mux);
}

/* The offline HDMI preview replaces the status screen, so hold it back
 * while the station joins a network or BLE setup runs (the screen shows
 * the setup instructions, then the code). */
static void wifi_update_display_mode(void)
{
    portENTER_CRITICAL(&s_screen_network_mux);
    const bool available = (s_screen_network.link_up && s_screen_network.ipv4_valid) ||
                           s_screen_network.wifi_joining_active ||
                           s_screen_network.wifi_setup_menu ||
                           s_screen_network.wifi_setup != JETKVM_SCREEN_WIFI_SETUP_CONNECTING;
    portEXIT_CRITICAL(&s_screen_network_mux);
    jetkvm_video_set_network_available(available);
}

void jetkvm_board_network_set_wifi_joining(const char *ssid)
{
    char text[JETKVM_SCREEN_WIFI_SSID_CAPACITY] = {0};
    if (ssid != NULL) {
        screen_copy_text(text, sizeof(text), ssid);
    }
    portENTER_CRITICAL(&s_screen_network_mux);
    s_screen_network.wifi_joining_active = ssid != NULL;
    memcpy(s_screen_network.wifi_joining, text, sizeof(text));
    portEXIT_CRITICAL(&s_screen_network_mux);
    wifi_update_display_mode();
}

void jetkvm_board_network_set_wifi_failure(const char *text)
{
    char failure[JETKVM_SCREEN_WIFI_FAILURE_CAPACITY] = {0};
    if (text != NULL) {
        screen_copy_text(failure, sizeof(failure), text);
    }
    portENTER_CRITICAL(&s_screen_network_mux);
    memcpy(s_screen_network.wifi_failure, failure, sizeof(failure));
    portEXIT_CRITICAL(&s_screen_network_mux);
}

void jetkvm_board_network_set_wifi_setup_stoppable(bool stoppable)
{
    portENTER_CRITICAL(&s_screen_network_mux);
    bool active = s_screen_network.wifi_setup_menu;
    if (active) s_screen_network.wifi_setup_stoppable = stoppable;
    portEXIT_CRITICAL(&s_screen_network_mux);
    if (active) wifi_update_display_mode();
}

void jetkvm_board_network_set_wifi_setup_menu(bool active, bool stoppable)
{
    portENTER_CRITICAL(&s_screen_network_mux);
    s_screen_network.wifi_setup_menu = active;
    s_screen_network.wifi_setup_stoppable = active && stoppable;
    if (active) s_screen_network.wifi_setup_menu_starts++;
    portEXIT_CRITICAL(&s_screen_network_mux);
    wifi_update_display_mode();
}

static const char *wifi_setup_name(jetkvm_screen_wifi_setup_t step)
{
    switch (step) {
    case JETKVM_SCREEN_WIFI_SETUP_CONNECTING: return "connecting";
    case JETKVM_SCREEN_WIFI_SETUP_ADVERTISING: return "advertising";
    case JETKVM_SCREEN_WIFI_SETUP_CODE: return "code";
    case JETKVM_SCREEN_WIFI_SETUP_ACCEPTED: return "code accepted";
    case JETKVM_SCREEN_WIFI_SETUP_JOINING: return "joining";
    }
    return "unknown";
}

void jetkvm_board_network_set_wifi_setup(bool advertising, const char *code)
{
    char code_text[JETKVM_SCREEN_WIFI_SETUP_CODE_CAPACITY] = {0};
    if (advertising && code != NULL) {
        screen_copy_text(code_text, sizeof(code_text), code);
    }
    const jetkvm_screen_wifi_setup_t step =
        !advertising ? JETKVM_SCREEN_WIFI_SETUP_CONNECTING :
        code_text[0] != '\0' ? JETKVM_SCREEN_WIFI_SETUP_CODE :
                               JETKVM_SCREEN_WIFI_SETUP_ADVERTISING;
    portENTER_CRITICAL(&s_screen_network_mux);
    const bool changed = s_screen_network.wifi_setup != step;
    s_screen_network.wifi_setup = step;
    memcpy(s_screen_network.wifi_setup_code, code_text, sizeof(code_text));
    s_screen_network.wifi_setup_network[0] = '\0';
    portEXIT_CRITICAL(&s_screen_network_mux);
    if (changed) ESP_LOGI(TAG, "Wi-Fi setup screen: %s", wifi_setup_name(step));
    wifi_update_display_mode();
}

void jetkvm_board_network_set_wifi_setup_phone(jetkvm_screen_wifi_setup_t step,
                                               const char *ssid)
{
    if (step < JETKVM_SCREEN_WIFI_SETUP_CODE) return;
    char network[JETKVM_SCREEN_WIFI_SSID_CAPACITY] = {0};
    if (step == JETKVM_SCREEN_WIFI_SETUP_JOINING && ssid != NULL) {
        screen_copy_text(network, sizeof(network), ssid);
    }
    portENTER_CRITICAL(&s_screen_network_mux);
    /* Only while a phone is connected: a step that arrives after the phone
     * left must not replace the advertising screen. */
    const bool phone = s_screen_network.wifi_setup >= JETKVM_SCREEN_WIFI_SETUP_CODE;
    const bool changed = phone && s_screen_network.wifi_setup != step;
    if (phone) {
        s_screen_network.wifi_setup = step;
        memcpy(s_screen_network.wifi_setup_network, network, sizeof(network));
    }
    portEXIT_CRITICAL(&s_screen_network_mux);
    if (changed) ESP_LOGI(TAG, "Wi-Fi setup screen: %s", wifi_setup_name(step));
}

static void screen_network_apply(jetkvm_screen_state_t *state)
{
    screen_network_runtime_t runtime;
    portENTER_CRITICAL(&s_screen_network_mux);
    runtime = s_screen_network;
    portEXIT_CRITICAL(&s_screen_network_mux);

    state->network_connected = runtime.link_up && runtime.ipv4_valid;
    state->wifi_connection = runtime.wifi_connection;
    state->wifi_signal_valid = runtime.wifi_signal_valid;
    state->wifi_rssi = runtime.wifi_rssi;
    screen_copy_text(state->wifi_ssid, sizeof(state->wifi_ssid),
                     runtime.wifi_ssid);
    state->wifi_setup = runtime.wifi_setup;
    state->wifi_setup_menu = runtime.wifi_setup_menu;
    state->wifi_setup_stoppable = runtime.wifi_setup_stoppable;
    state->wifi_setup_menu_starts = runtime.wifi_setup_menu_starts;
    screen_copy_text(state->wifi_joining, sizeof(state->wifi_joining),
                     runtime.wifi_setup == JETKVM_SCREEN_WIFI_SETUP_JOINING ?
                         runtime.wifi_setup_network : runtime.wifi_joining);
    screen_copy_text(state->wifi_failure, sizeof(state->wifi_failure), runtime.wifi_failure);
    /* The code reaches the screen only while it asks for it. */
    screen_copy_text(state->wifi_setup_code, sizeof(state->wifi_setup_code),
                     runtime.wifi_setup == JETKVM_SCREEN_WIFI_SETUP_CODE ?
                         runtime.wifi_setup_code : "");
    screen_copy_text(state->mac, sizeof(state->mac), runtime.mac);
    screen_copy_text(state->ipv4, sizeof(state->ipv4),
                     state->network_connected ? runtime.ipv4 : "");
    state->ipv6[0] = '\0';
}

static void screen_cloud_domain_from_url(char *destination, size_t capacity,
                                         const char *url)
{
    destination[0] = '\0';
    if (capacity < 2U || url == NULL || url[0] == '\0') {
        return;
    }
    const char *host = strstr(url, "://");
    host = host != NULL ? host + 3 : url;
    const char *end = host;
    if (*host == '[') {
        const char *bracket = strchr(host, ']');
        end = bracket != NULL ? bracket + 1 : host;
    } else {
        while (*end != '\0' && *end != ':' && *end != '/' &&
               *end != '?' && *end != '#') {
            end++;
        }
    }
    size_t length = (size_t)(end - host);
    if (length >= capacity) {
        length = capacity - 1U;
    }
    memcpy(destination, host, length);
    destination[length] = '\0';
}

static void screen_fill_static_state(jetkvm_screen_state_t *state,
                                     jetkvm_screen_page_t page)
{
    memset(state, 0, sizeof(*state));
    state->page = page;
    state->cloud_state = JETKVM_SCREEN_CLOUD_NOT_CONFIGURED;
    state->usb_connected = false;
    /* Also in the boot state, so the Advanced menu shows the setting from
     * the start. */
    state->usb_debugging_enabled = jetkvm_usb_debugging_enabled();
    state->usb_debugging_active = jetkvm_usb_debugging_active();

    const esp_app_desc_t *description = esp_app_get_description();
    if (description != NULL) {
        screen_copy_text(state->app_version, sizeof(state->app_version),
                         description->version);
        (void)snprintf(state->build_date, sizeof(state->build_date), "%s %s",
                       description->date, description->time);
    }
    /* The About rows read Hardware, Firmware, Build Date, ESP-IDF, LVGL,
     * FreeRTOS and Device ID on this board; the screen retitles the
     * generated labels to match. */
    screen_copy_text(state->system_version, sizeof(state->system_version),
                     JETKVM_BOARD_WIFI ? "ESP32-P4 Wi-Fi" : "ESP32-P4 Ethernet");
    screen_copy_text(state->runtime_version, sizeof(state->runtime_version),
                     esp_get_idf_version());
    screen_copy_text(state->kernel_version, sizeof(state->kernel_version),
                     tskKERNEL_VERSION_NUMBER);
    if (jetkvm_device_state_get_id(state->device_id,
                                   sizeof(state->device_id)) == ESP_OK) {
        screen_copy_text(state->cpu_serial, sizeof(state->cpu_serial),
                         state->device_id);
    }
}

static void screen_update_dynamic_state(jetkvm_screen_state_t *state)
{
    screen_network_apply(state);
    state->hdmi_connected = jetkvm_video_is_hdmi_connected();
    jetkvm_usb_hid_state_t usb_state = {0};
    state->usb_connected =
        jetkvm_usb_hid_get_state(&usb_state) == ESP_OK &&
        usb_state.initialized && usb_state.mounted;
    state->usb_debugging_enabled = jetkvm_usb_debugging_enabled();
    state->usb_debugging_active = jetkvm_usb_debugging_active();
    state->active_sessions = 0;
    state->cloud_state = JETKVM_SCREEN_CLOUD_NOT_CONFIGURED;
    state->cloud_domain[0] = '\0';

    webrtc_transport_stats_t transport_stats;
    if (webrtc_transport_get_stats(&transport_stats) == ESP_OK &&
        transport_stats.browser_session_active &&
        transport_stats.peer_connected) {
        /* The P4 transport intentionally owns at most one browser session. */
        state->active_sessions = 1;
    }

    jetkvm_device_cloud_public_state_t public_state;
    if (jetkvm_device_state_cloud_public_snapshot(&public_state) != ESP_OK) {
        return;
    }
    screen_cloud_domain_from_url(state->cloud_domain,
                                 sizeof(state->cloud_domain),
                                 public_state.cloud_url);
    if (!public_state.registered) {
        return;
    }

    state->cloud_state = JETKVM_SCREEN_CLOUD_DISCONNECTED;
    jetkvm_cloud_service_stats_t cloud_stats;
    if (jetkvm_cloud_service_get_stats(&cloud_stats) != ESP_OK) {
        return;
    }
    if (cloud_stats.client.connected) {
        state->cloud_state = JETKVM_SCREEN_CLOUD_CONNECTED;
    } else if (cloud_stats.started && cloud_stats.configured) {
        state->cloud_state = JETKVM_SCREEN_CLOUD_CONNECTING;
    }
}

static void screen_status_task(void *argument)
{
    (void)argument;
    jetkvm_screen_state_t state;
    screen_fill_static_state(&state, JETKVM_SCREEN_PAGE_AUTO);
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t update_failures = 0;

    for (;;) {
        screen_update_dynamic_state(&state);
        esp_err_t error = jetkvm_screen_update(&state);
        if (error != ESP_OK) {
            update_failures++;
            if (update_failures == 1U || (update_failures % 120U) == 0U) {
                ESP_LOGW(TAG, "screen status update failed: %s",
                         esp_err_to_name(error));
            }
        } else {
            update_failures = 0;
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SCREEN_STATUS_UPDATE_MS));
    }
}

esp_err_t jetkvm_board_network_screen_start(void)
{
    esp_err_t error = jetkvm_screen_start();
    if (error != ESP_OK) {
        return error;
    }
    s_screen_started = true;
    jetkvm_screen_state_t boot_state;
    screen_fill_static_state(&boot_state, JETKVM_SCREEN_PAGE_BOOT);
    return jetkvm_screen_update(&boot_state);
}

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == ETH_EVENT) {
        esp_eth_handle_t eth_handle = event_data != NULL ?
            *(esp_eth_handle_t *)event_data : s_eth_handle;
        switch (event_id) {
        case ETHERNET_EVENT_CONNECTED: {
            uint8_t mac_addr[6] = {0};
            if (eth_handle != NULL) {
                (void)esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
            }
            eth_speed_t speed = ETH_SPEED_10M;
            eth_duplex_t duplex = ETH_DUPLEX_HALF;
            if (eth_handle != NULL) {
                (void)esp_eth_ioctl(eth_handle, ETH_CMD_G_SPEED, &speed);
                (void)esp_eth_ioctl(eth_handle, ETH_CMD_G_DUPLEX_MODE, &duplex);
            }
            screen_network_set_link(true, eth_handle);
            ESP_LOGI(ETH_TAG,
                     "Link Up HW=%02x:%02x:%02x:%02x:%02x:%02x speed=%s duplex=%s",
                     mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3],
                     mac_addr[4], mac_addr[5],
                     speed == ETH_SPEED_100M ? "100M" : "10M",
                     duplex == ETH_DUPLEX_FULL ? "full" : "half");
            break;
        }
        case ETHERNET_EVENT_DISCONNECTED:
            screen_network_set_link(false, eth_handle);
            ESP_LOGI(ETH_TAG, "Link Down");
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(ETH_TAG, "Started");
            break;
        case ETHERNET_EVENT_STOP:
            screen_network_set_link(false, eth_handle);
            ESP_LOGI(ETH_TAG, "Stopped");
            break;
        default:
            break;
        }
        return;
    }

    if (event_base != IP_EVENT) {
        return;
    }
    if (event_id == IP_EVENT_ETH_GOT_IP && event_data != NULL) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        screen_network_set_ipv4(&event->ip_info.ip);
        ESP_LOGI(ETH_TAG, "ETH Got IP: " IPSTR " gw: " IPSTR,
                 IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw));
    } else if (event_id == IP_EVENT_ETH_LOST_IP) {
        screen_network_set_ipv4(NULL);
        ESP_LOGI(ETH_TAG, "ETH Lost IP");
    }
}

void jetkvm_board_network_ethernet_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    /* Preserve the stock Ethernet stack while observing its two otherwise
     * silent software RX loss boundaries. esp_netif_new() copies the function
     * pointers, so this stack configuration may remain local. */
    esp_netif_netstack_config_t tracked_stack = *cfg.stack;
    tracked_stack.lwip.input_fn = jetkvm_ethernet_input;
    cfg.stack = &tracked_stack;
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    ESP_ERROR_CHECK(eth_netif != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK((esp_netif_get_flags(eth_netif) &
                     ESP_NETIF_DHCP_CLIENT) != 0U ?
                    ESP_OK : ESP_ERR_INVALID_STATE);
    s_eth_netif = eth_netif;

    ESP_LOGI(ETH_TAG, "Initializing Ethernet...");

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    /* Drain the small hardware DMA ring before tcpip_thread starts consuming
     * the software queue. With the IDF defaults (EMAC RX priority 15 versus
     * tcpip priority 18), tcpip_thread can preempt this task after every frame
     * and leave a burst in DMA long enough to overrun the ring. */
    _Static_assert(JETKVM_EMAC_RX_TASK_PRIORITY > 0 &&
                       JETKVM_EMAC_RX_TASK_PRIORITY < configMAX_PRIORITIES,
                   "EMAC RX task priority must fit FreeRTOS");
    mac_config.rx_task_prio = JETKVM_EMAC_RX_TASK_PRIORITY;
    jetkvm_ethernet_rx_stats_set_task_priority(mac_config.rx_task_prio);
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = 51;

    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();

    ESP_LOGI(ETH_TAG, "EMAC RX priority=%" PRIu32 " tcpip priority=%d",
             mac_config.rx_task_prio, CONFIG_LWIP_TCPIP_TASK_PRIO);

    /* The EMAC reads ESP_MAC_ETH when the driver installs: the factory MAC of
     * eFuse, or the Espressif MAC as the MAC address setting has it. */
    jetkvm_mac_address_apply_ethernet();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t eth_ret = esp_eth_driver_install(&config, &s_eth_handle);
    esp_eth_handle_t eth_handle = s_eth_handle;
    if (eth_ret != ESP_OK) {
        ESP_LOGE(ETH_TAG, "EMAC init failed: %s", esp_err_to_name(eth_ret));
        return;
    }
    uint8_t mac_addr[6] = {0};
    if (esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr) == ESP_OK) {
        screen_network_set_mac(mac_addr);
    }

    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));

    /* IDF 6's default glue starts DHCP only in INIT state. Restoring static
     * settings below leaves it STOPPED, which survives start and link-up. */

#if CONFIG_ETH_SOFT_FLOW_CONTROL
    /* Advertise and enable IEEE 802.3x pause before auto-negotiation starts.
     * The P4 EMAC has only twenty RX descriptors, so the driver's low-water
     * pause protects the ring from a WAN TCP burst without reserving another
     * large software buffer or capping steady-state throughput. The IDF ioctl
     * reads the same argument as both bool and uint32_t, hence this storage. */
    uint32_t flow_control_enabled = 1U;
    ESP_ERROR_CHECK(esp_eth_ioctl(
        eth_handle, ETH_CMD_S_FLOW_CTRL, &flow_control_enabled));
    ESP_LOGI(ETH_TAG, "IEEE 802.3x RX-ring flow control enabled");
#endif

    ESP_ERROR_CHECK(jetkvm_network_settings_init(eth_netif));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    /* Check DHCP client state */
    esp_netif_dhcp_status_t dhcp_status;
    esp_netif_dhcpc_get_status(eth_netif, &dhcp_status);
    ESP_LOGI(ETH_TAG, "Ethernet initialized, DHCP status=%d (0=INIT,1=STARTED,2=STOPPED)", dhcp_status);
}


void jetkvm_board_network_screen_status_start(void)
{
    if (!s_screen_started) {
        return;
    }
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        screen_status_task, "screen_status", SCREEN_STATUS_TASK_STACK,
        NULL, tskIDLE_PRIORITY + 1, NULL, SCREEN_STATUS_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "screen status task unavailable; local video continues");
    }
}

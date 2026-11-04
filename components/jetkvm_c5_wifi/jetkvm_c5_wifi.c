#include "jetkvm_c5_wifi.h"

#include "jetkvm_board_config.h"

#if JETKVM_BOARD_WIFI

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp32_sdio_port.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_misc.h"
#include "esp_heap_caps.h"
#include "esp_loader.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "jetkvm_board_network.h"
#include "jetkvm_factory_reset.h"
#include "jetkvm_network_settings.h"
#include "jetkvm_mac_address.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "nvs.h"
#include "protocomm_security.h"
#include "wifi_join_failure.h"
#include "wifi_network_list.h"

/* The web UI list keeps the strongest 32 SSIDs; saved networks are
 * matched against up to 128 scanned APs. */
#define C5_SCAN_RECORDS_MAX 128U
#define C5_START_TASK_STACK_BYTES (12U * 1024U)
#define C5_SCAN_RETRY_COUNT 5U
#define C5_SCAN_RETRY_DELAY_MS 200U
#define C5_SCAN_TIMEOUT_MS 20000U
#define C5_FLASH_BLOCK_SIZE 1024U
#define C5_PROV_CONNECT_ATTEMPTS 5U
#define C5_PROV_NAME_CAPACITY 16U
#define C5_STATUS_UPDATE_MS 2000U
#define C5_STATUS_TASK_STACK_BYTES (16U * 1024U)
#define C5_STATUS_TASK_CORE 0
/* The connection task writes NVS, so its stack must be in internal RAM. */
#define C5_CONN_TASK_STACK_BYTES (8U * 1024U)
#define C5_JOIN_TIMEOUT_MS 20000U
#define C5_AUTH_RETRY_MS 1000U
/* The longest search after a band change: one scan and, for each saved
 * network in range, a join and its retry after a rejected handshake. */
#define C5_BAND_SEARCH_LIMIT_MS \
    (C5_SCAN_TIMEOUT_MS + WIFI_NETWORK_LIST_MAX * (2U * C5_JOIN_TIMEOUT_MS + C5_AUTH_RETRY_MS))
/* How long the device announces a network switch before it leaves. */
#define C5_SWITCH_NOTICE_MS 2000U
#define C5_CONNECT_ACK_MS 3000U
#define C5_OFFLINE_RESCAN_MS 30000U
/* While offline, a saved network whose last join failed authentication is
 * tried again only after this long, so a network saved with a wrong password
 * does not stop BLE setup at every 30 s rescan. */
#define C5_AUTH_FAILED_SKIP_MS (5U * 60U * 1000U)
#define C5_PROV_STOP_TIMEOUT_MS 5000U
/* Setup started from the local menu ends after this long without a phone. */
#define C5_MENU_SETUP_IDLE_MS (10U * 60U * 1000U)
#define C5_POP_DIGITS 6U
#define C5_POP_MISMATCH_LIMIT 3U
/* CMake sets this to 1 only for bench builds (-DJETKVM_WIFI_SETUP_LOG_CODE=1),
 * which use the fixed setup code C5_BENCH_POP and log it. */
#ifndef JETKVM_WIFI_SETUP_LOG_CODE
#define JETKVM_WIFI_SETUP_LOG_CODE 0
#endif
#if JETKVM_WIFI_SETUP_LOG_CODE
/* tools/e2e/wifi_reprovision.py uses the same code. */
#define C5_BENCH_POP "246810"
#endif

#define C5_PROV_NVS_NAMESPACE "wifi_prov"
#define C5_PROV_NVS_NETWORKS_KEY "networks"
/* Single-network record of older firmware, moved into the list on boot. */
#define C5_PROV_NVS_STA_KEY "sta_cfg"
#define C5_PROV_NVS_C5_CLEARED_KEY "c5_clean"

static const char *TAG = "c5_wifi";

extern const uint8_t c5_bootloader_zlib_start[]
    asm("_binary_c5_bootloader_zlib_bin_start");
extern const uint8_t c5_bootloader_zlib_end[]
    asm("_binary_c5_bootloader_zlib_bin_end");
extern const uint8_t c5_partition_table_zlib_start[]
    asm("_binary_c5_partition_table_zlib_bin_start");
extern const uint8_t c5_partition_table_zlib_end[]
    asm("_binary_c5_partition_table_zlib_bin_end");
extern const uint8_t c5_ota_data_initial_zlib_start[]
    asm("_binary_c5_ota_data_initial_zlib_bin_start");
extern const uint8_t c5_ota_data_initial_zlib_end[]
    asm("_binary_c5_ota_data_initial_zlib_bin_end");
extern const uint8_t c5_network_adapter_zlib_start[]
    asm("_binary_c5_network_adapter_zlib_bin_start");
extern const uint8_t c5_network_adapter_zlib_end[]
    asm("_binary_c5_network_adapter_zlib_bin_end");

typedef struct {
    const char *name;
    uint32_t flash_offset;
    uint32_t raw_size;
    const uint8_t *raw_md5;
    const uint8_t *compressed_start;
    const uint8_t *compressed_end;
    bool runtime_mutable;
} c5_release_segment_t;

/* Exact layout, sizes and MD5 values from the ESP-Hosted-MCU 2.12.3 slave
 * image built for ESP32-C5 with ESP-IDF 5.5.4. */
static const c5_release_segment_t c5_release_segments[] = {
    {
        .name = "bootloader",
        .flash_offset = 0x2000,
        .raw_size = 23952,
        .raw_md5 = (const uint8_t *)"009879df5e0b779354a4129739e038c7",
        .compressed_start = c5_bootloader_zlib_start,
        .compressed_end = c5_bootloader_zlib_end,
    },
    {
        .name = "partition table",
        .flash_offset = 0xc000,
        .raw_size = 3072,
        .raw_md5 = (const uint8_t *)"0815089a35fc57f2259b102897bee436",
        .compressed_start = c5_partition_table_zlib_start,
        .compressed_end = c5_partition_table_zlib_end,
    },
    {
        .name = "OTA data",
        .flash_offset = 0x16000,
        .raw_size = 8192,
        .raw_md5 = (const uint8_t *)"84d04c9d6cc8ef35bf825d51a5277699",
        .compressed_start = c5_ota_data_initial_zlib_start,
        .compressed_end = c5_ota_data_initial_zlib_end,
        .runtime_mutable = true,
    },
    {
        .name = "network adapter",
        .flash_offset = 0x20000,
        .raw_size = 1267936,
        .raw_md5 = (const uint8_t *)"b7934a97930fc6a5c2b1f300dfa22a4d",
        .compressed_start = c5_network_adapter_zlib_start,
        .compressed_end = c5_network_adapter_zlib_end,
    },
};

typedef struct {
    TaskHandle_t waiter;
    esp_err_t (*worker)(void);
    esp_err_t result;
} c5_task_context_t;

static bool s_hosted_initialized;
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_c5_wifi_ready;
static bool s_provisioning_manager_initialized;
static bool s_provisioning_active;
static bool s_bt_controller_initialized;
static bool s_bt_controller_enabled;
/* The factory MAC that the station took this boot (jetkvm_mac_address.h);
 * the BLE controller takes it too. */
static bool s_factory_mac_in_use;
static uint8_t s_factory_mac[6];
/* GAP listener registered while provisioning runs; NimBLE drops it on stop. */
static struct ble_gap_event_listener s_gap_listener;
static esp_netif_t *s_station;
static TaskHandle_t s_status_task;
static bool s_station_connected;
static bool s_station_stopped;
static portMUX_TYPE s_settings_lock = portMUX_INITIALIZER_UNLOCKED;
static jetkvm_wifi_settings_t s_wifi_settings = {.available = true};
static jetkvm_wifi_band_t s_previous_band;
/* SSID the station joined or is joining; guarded by s_settings_lock. */
static char s_current_ssid[WIFI_NETWORK_SSID_MAX + 1U];

/* Saved networks, scan cache and connect-now request; guarded by
 * s_networks_lock, which is also held across the NVS write of the list. */
static SemaphoreHandle_t s_networks_lock;
static wifi_network_list_t s_networks;
static jetkvm_wifi_scan_t s_scan_cache;
static wifi_saved_network_t s_connect_request;

/* The connection task owns every connect, scan and radio restart. Other
 * tasks and the event handlers send it these notification bits. */
#define C5_NOTIFY_GOT_IP (1U << 0)
#define C5_NOTIFY_DISCONNECTED (1U << 1)
#define C5_NOTIFY_RESCAN (1U << 2)
#define C5_NOTIFY_SCAN_REQUEST (1U << 3)
#define C5_NOTIFY_PROV_END (1U << 4)
#define C5_NOTIFY_POP_ROTATE (1U << 5)
#define C5_NOTIFY_BAND (1U << 6)
#define C5_NOTIFY_CONNECT_NOW (1U << 7)
#define C5_NOTIFY_SCAN_DONE (1U << 8)
#define C5_NOTIFY_BLE_CENTRAL (1U << 9)
#define C5_NOTIFY_SETUP_START (1U << 10)
#define C5_NOTIFY_SETUP_STOP (1U << 11)
/* Requests that end a join, a wait on the current network or a scan. */
#define C5_NOTIFY_COMMANDS (C5_NOTIFY_BAND | C5_NOTIFY_CONNECT_NOW | C5_NOTIFY_SETUP_START)
static TaskHandle_t s_conn_task;
/* Band restarts requested by the band worker and completed by the
 * connection task; the worker's connection check counts only after its own
 * restart. */
static uint32_t s_band_requested;
static uint32_t s_band_applied;
/* The band request whose search the connection task has finished (joined a
 * network, or tried every candidate, or failed to restart the radio). */
static uint32_t s_band_searched;
static uint16_t s_disconnect_reason;
/* The AP admitted the station (WIFI_EVENT_STA_CONNECTED) and no disconnect
 * came since: a join that times out now waits for DHCP. */
static bool s_station_associated;
static bool s_ble_central_connected;
/* When the last phone disconnected from BLE setup. */
static TickType_t s_ble_central_left_at;
/* BLE setup started from the local menu: the device joins no saved network
 * until a phone sets one up, the menu ends setup, or no phone comes for
 * C5_MENU_SETUP_IDLE_MS. Set with s_settings_lock held, so a band change or
 * connect-now cannot start after it; cleared by the connection task. */
static bool s_menu_setup;
/* Each start from the menu gets a number; a stop acts only on the setup it
 * was sent for, not on a later one. */
static uint32_t s_menu_setup_generation;
static uint32_t s_menu_setup_stop_generation;
/* Connection task only: when menu setup started, and the band it replaced
 * with Auto (restored when setup ends without a new network). */
static TickType_t s_menu_setup_started_at;
static bool s_menu_setup_band_changed;
/* The Auto band is selected but the radio has not switched yet. */
static bool s_menu_setup_band_pending;
static jetkvm_wifi_band_t s_menu_setup_band;
/* True while the connection task scans. ESP-Hosted mismatches replies when
 * a request to the C5 times out, so nothing else queries the C5 then. */
static bool s_scan_active;
/* The last c5_scan could not list the APs (the scan failed, timed out or
 * stopped, or memory ran out); connection task only. */
static bool s_scan_failed;
static unsigned s_pop_mismatches;
static char s_pop[C5_POP_DIGITS + 1U];
/* The code stays the same for one offline period, also when BLE setup
 * restarts for a rescan join; a new one comes after 3 wrong codes or after
 * the device was online again. */
static bool s_pop_issued;

typedef struct {
    uint8_t ssid[WIFI_NETWORK_SSID_MAX];
    uint8_t ssid_len;
    TickType_t at;
} c5_auth_failure_t;
/* Saved networks whose last join failed authentication (RAM only). Saving a
 * network again clears its entry. Guarded by s_settings_lock. */
static c5_auth_failure_t s_auth_failures[WIFI_NETWORK_LIST_MAX];

static void c5_show_failure(const uint8_t *ssid, size_t ssid_len, wifi_join_failure_t failure);

static void c5_conn_notify(uint32_t bits)
{
    TaskHandle_t task = __atomic_load_n(&s_conn_task, __ATOMIC_ACQUIRE);
    if (task != NULL) xTaskNotify(task, bits, eSetBits);
}

static void c5_set_current_ssid(const uint8_t *ssid, size_t length)
{
    portENTER_CRITICAL(&s_settings_lock);
    memset(s_current_ssid, 0, sizeof(s_current_ssid));
    memcpy(s_current_ssid, ssid, length < WIFI_NETWORK_SSID_MAX ? length : WIFI_NETWORK_SSID_MAX);
    portEXIT_CRITICAL(&s_settings_lock);
}

/* Erase the code; only while the provisioning service is not running. */
static void c5_pop_forget(void)
{
    memset(s_pop, 0, sizeof(s_pop));
    __atomic_store_n(&s_pop_issued, false, __ATOMIC_RELEASE);
}

/* The next BLE setup start makes a new code. The service may still be using
 * this one (a phone just set up a network, or a rotation stops it), so the
 * buffer is erased at NETWORK_PROV_END, not here. */
static void c5_pop_retire(void)
{
    __atomic_store_n(&s_pop_issued, false, __ATOMIC_RELEASE);
}

static c5_auth_failure_t *c5_auth_failure_find_locked(const uint8_t *ssid, size_t length)
{
    for (size_t i = 0; i < WIFI_NETWORK_LIST_MAX; ++i) {
        c5_auth_failure_t *entry = &s_auth_failures[i];
        if (entry->ssid_len == length && memcmp(entry->ssid, ssid, length) == 0) {
            return entry;
        }
    }
    return NULL;
}

/* Record (failed) or clear (!failed) an authentication failure of a network. */
static void c5_auth_failure_set(const uint8_t *ssid, size_t length, bool failed)
{
    if (length == 0U || length > WIFI_NETWORK_SSID_MAX) return;
    portENTER_CRITICAL(&s_settings_lock);
    c5_auth_failure_t *entry = c5_auth_failure_find_locked(ssid, length);
    if (failed) {
        if (entry == NULL) {
            /* A free slot, or else the entry with the largest age. Ages are
             * unsigned differences, so they stay right when the tick count
             * wraps (about every 49 days). */
            TickType_t now = xTaskGetTickCount();
            entry = &s_auth_failures[0];
            for (size_t i = 0; i < WIFI_NETWORK_LIST_MAX; ++i) {
                if (s_auth_failures[i].ssid_len == 0U) {
                    entry = &s_auth_failures[i];
                    break;
                }
                if (now - s_auth_failures[i].at > now - entry->at) entry = &s_auth_failures[i];
            }
        }
        memcpy(entry->ssid, ssid, length);
        entry->ssid_len = (uint8_t)length;
        entry->at = xTaskGetTickCount();
    } else if (entry != NULL) {
        memset(entry, 0, sizeof(*entry));
    }
    portEXIT_CRITICAL(&s_settings_lock);
}

static bool c5_auth_failed_recently(const uint8_t *ssid, size_t length)
{
    portENTER_CRITICAL(&s_settings_lock);
    const c5_auth_failure_t *entry = c5_auth_failure_find_locked(ssid, length);
    bool recent = entry != NULL &&
                  xTaskGetTickCount() - entry->at < pdMS_TO_TICKS(C5_AUTH_FAILED_SKIP_MS);
    portEXIT_CRITICAL(&s_settings_lock);
    return recent;
}

/* The band change reports through apply_failed; a connect-now request
 * reports through connect_error. Both end the shared `applying` state. */
static void c5_set_band_result(bool failed)
{
    portENTER_CRITICAL(&s_settings_lock);
    s_wifi_settings.applying = false;
    s_wifi_settings.apply_failed = failed;
    portEXIT_CRITICAL(&s_settings_lock);
}

static void c5_set_connect_result(const char *error)
{
    char text[sizeof(s_wifi_settings.connect_error)];
    (void)snprintf(text, sizeof(text), "%s", error != NULL ? error : "");
    portENTER_CRITICAL(&s_settings_lock);
    s_wifi_settings.applying = false;
    s_wifi_settings.switching = false;
    memcpy(s_wifi_settings.connect_error, text, sizeof(text));
    portEXIT_CRITICAL(&s_settings_lock);
}

static wifi_band_mode_t c5_selected_band(void)
{
    portENTER_CRITICAL(&s_settings_lock);
    jetkvm_wifi_band_t band = s_wifi_settings.band;
    portEXIT_CRITICAL(&s_settings_lock);
    return band == JETKVM_WIFI_BAND_2G ? WIFI_BAND_MODE_2G_ONLY :
           band == JETKVM_WIFI_BAND_5G ? WIFI_BAND_MODE_5G_ONLY : WIFI_BAND_MODE_AUTO;
}

static esp_err_t c5_wait_station_stopped(void)
{
    for (unsigned i = 0; i < 200; ++i) {
        if (__atomic_load_n(&s_station_stopped, __ATOMIC_ACQUIRE)) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_ERR_TIMEOUT;
}

/* A station start after the first one (a band switch, a radio restart). The
 * C5 keeps the station MAC across a stop, but the factory MAC is set again
 * so that each STA_START gives the P4's netif the MAC of the setting. */
static esp_err_t c5_station_start(void)
{
    if (s_factory_mac_in_use) {
        esp_err_t error = esp_wifi_set_mac(WIFI_IF_STA, s_factory_mac);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "Setting the factory MAC again failed (%s)", esp_err_to_name(error));
        }
    }
    return esp_wifi_start();
}

/* Run from the connection task, never the Wi-Fi event loop: changing
 * physical bands emits STA_STOP. Drain it before Hosted synthesizes the next
 * STA_START. */
static esp_err_t c5_apply_band(void)
{
    wifi_band_mode_t desired = c5_selected_band(), current;
    wifi_band_t physical_band;
    esp_err_t err = esp_wifi_get_band_mode(&current);
    if (err != ESP_OK) return err;
    err = esp_wifi_get_band(&physical_band);
    if (err != ESP_OK) return err;
    if (current != desired) {
        bool switches_radio =
            (desired == WIFI_BAND_MODE_2G_ONLY && physical_band != WIFI_BAND_2G) ||
            (desired == WIFI_BAND_MODE_5G_ONLY && physical_band != WIFI_BAND_5G);
        __atomic_store_n(&s_station_stopped, false, __ATOMIC_RELEASE);
        err = esp_wifi_set_band_mode(desired);
        if (err != ESP_OK) return err;
        if (switches_radio) {
            err = c5_wait_station_stopped();
            if (err != ESP_OK) return err;
            err = c5_station_start();
            if (err != ESP_OK) return err;
        }
    }
    return ESP_OK;
}

static esp_err_t c5_save_band(jetkvm_wifi_band_t band)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(C5_PROV_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "band", (uint8_t)band);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool jetkvm_c5_wifi_get_settings(jetkvm_wifi_settings_t *settings, void *ctx)
{
    (void)ctx;
    if (!settings) return false;
    char current[sizeof(s_current_ssid)];
    portENTER_CRITICAL(&s_settings_lock);
    *settings = s_wifi_settings;
    memcpy(current, s_current_ssid, sizeof(current));
    portEXIT_CRITICAL(&s_settings_lock);
    settings->connected = __atomic_load_n(&s_station_connected, __ATOMIC_ACQUIRE);
    settings->provisioning = __atomic_load_n(&s_provisioning_active, __ATOMIC_ACQUIRE);
    if (!settings->connected) {settings->channel = 0; settings->rssi = 0;}
    /* The link refresh fills ssid every 2 s; right after a join it can still
     * name the previous network. The connection task knows the network it
     * joined, and networks[].connected below uses the same name. */
    if (settings->connected) {
        memcpy(settings->ssid, current, sizeof(settings->ssid) < sizeof(current) ?
               sizeof(settings->ssid) : sizeof(current));
        settings->ssid[sizeof(settings->ssid) - 1U] = '\0';
    }
    settings->network_count = 0;
    jetkvm_network_settings_static_network(settings->static_ssid, sizeof(settings->static_ssid));
    if (s_networks_lock == NULL ||
        xSemaphoreTake(s_networks_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        return true;
    }
    for (uint8_t i = 0; i < s_networks.count && i < JETKVM_WIFI_MAX_NETWORKS; ++i) {
        const wifi_saved_network_t *saved = &s_networks.items[i];
        jetkvm_wifi_saved_network_t *out = &settings->networks[i];
        memcpy(out->ssid, saved->ssid, saved->ssid_len);
        out->ssid[saved->ssid_len] = '\0';
        out->connected = settings->connected && strcmp(out->ssid, current) == 0;
        /* Passwords stay on the device; the UI only learns whether there is one. */
        out->secure = saved->password_len > 0U;
        settings->network_count = i + 1U;
    }
    xSemaphoreGive(s_networks_lock);
    return true;
}

/* The connection task restarts the radio on the selected band, then joins
 * the strongest saved network on it. */
static esp_err_t c5_restart_selected_band(void)
{
    __atomic_add_fetch(&s_band_requested, 1U, __ATOMIC_ACQ_REL);
    c5_conn_notify(C5_NOTIFY_BAND);
    return ESP_OK;
}

/* Wait for a connection made after the connection task restarted the radio
 * for the latest band request. The connection task reports when its search
 * on the new band has ended without one, so a band with no usable network
 * rolls back at once, and one whose candidates take long is not cut off. */
static esp_err_t c5_wait_station_connected(void)
{
    for (unsigned i = 0; i < C5_BAND_SEARCH_LIMIT_MS / 100U; ++i) {
        uint32_t requested = __atomic_load_n(&s_band_requested, __ATOMIC_ACQUIRE);
        bool connected = __atomic_load_n(&s_station_connected, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s_band_applied, __ATOMIC_ACQUIRE) == requested && connected) {
            return ESP_OK;
        }
        if (__atomic_load_n(&s_band_searched, __ATOMIC_ACQUIRE) == requested && !connected) {
            return ESP_ERR_NOT_FOUND;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_ERR_TIMEOUT;
}

static void c5_band_worker(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    /* Deliver the RPC acknowledgement before the band restart interrupts the
     * current network (see c5_conn_connect_now). */
    vTaskDelay(pdMS_TO_TICKS(C5_CONNECT_ACK_MS));
    esp_err_t err = c5_restart_selected_band();
    if (err == ESP_OK) err = c5_wait_station_connected();
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_settings_lock);
        jetkvm_wifi_band_t band = s_wifi_settings.band;
        portEXIT_CRITICAL(&s_settings_lock);
        err = c5_save_band(band);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi band change failed; restoring previous band: %s",
                 esp_err_to_name(err));
        portENTER_CRITICAL(&s_settings_lock);
        s_wifi_settings.band = s_previous_band;
        portEXIT_CRITICAL(&s_settings_lock);
        /* The saved band is unchanged until the new connection obtains IP. */
        esp_err_t restore_error = c5_restart_selected_band();
        if (restore_error != ESP_OK)
            ESP_LOGE(TAG, "Wi-Fi band rollback failed: %s", esp_err_to_name(restore_error));
    }
    c5_set_band_result(err != ESP_OK);
    vTaskDelete(NULL);
}

const char *jetkvm_c5_wifi_set_band(jetkvm_wifi_band_t band, void *ctx)
{
    (void)ctx;
    if ((unsigned)band > JETKVM_WIFI_BAND_5G) return "Invalid Wi-Fi band";
    if (!s_c5_wifi_ready || s_provisioning_active) return "Wi-Fi is not ready";
    portENTER_CRITICAL(&s_settings_lock);
    if (s_wifi_settings.applying) {
        portEXIT_CRITICAL(&s_settings_lock);
        return "Wi-Fi settings are still being applied";
    }
    if (s_menu_setup) {
        portEXIT_CRITICAL(&s_settings_lock);
        return "Wi-Fi setup is running";
    }
    if (s_wifi_settings.band == band && !s_wifi_settings.apply_failed) {
        portEXIT_CRITICAL(&s_settings_lock);
        return NULL;
    }
    s_wifi_settings.applying = true;
    s_previous_band = s_wifi_settings.band;
    portEXIT_CRITICAL(&s_settings_lock);
    TaskHandle_t worker = NULL;
    const char *error = NULL;
    if (xTaskCreate(c5_band_worker, "c5_band", 8192, NULL, 4, &worker) != pdPASS)
        error = "Could not start Wi-Fi settings worker";
    portENTER_CRITICAL(&s_settings_lock);
    if (error) s_wifi_settings.applying = false;
    else {
        s_wifi_settings.band = band;
        s_wifi_settings.apply_failed = false;
    }
    portEXIT_CRITICAL(&s_settings_lock);
    if (!error) xTaskNotifyGive(worker);
    return error;
}

static char s_provisioning_name[C5_PROV_NAME_CAPACITY];
static wifi_sta_config_t s_pending_station_config;
static bool s_pending_station_config_valid;

static void c5_start_status_task_if_needed(void);


static esp_err_t c5_prepare_normal_boot(void)
{
    /* FPC1 exposes C5_BOOT and C5_EN. Hold BOOT high for normal boot; the
     * ESP-Hosted SDIO driver subsequently owns C5_EN as its reset signal. */
    esp_err_t error = gpio_set_level(JETKVM_C5_PIN_BOOT, 1);
    if (error != ESP_OK) {
        return error;
    }
    error = gpio_set_level(JETKVM_C5_PIN_EN, 1);
    if (error != ESP_OK) {
        return error;
    }

    const gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << JETKVM_C5_PIN_BOOT) |
                        (1ULL << JETKVM_C5_PIN_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    error = gpio_config(&output_config);
    if (error != ESP_OK) {
        return error;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "C5 normal boot: BOOT=GPIO%d EN=GPIO%d",
             JETKVM_C5_PIN_BOOT, JETKVM_C5_PIN_EN);
    return ESP_OK;
}

static esp_loader_error_t c5_flash_release_segment(
    esp_loader_t *loader, const c5_release_segment_t *segment)
{
    const uint32_t compressed_size =
        (uint32_t)(segment->compressed_end - segment->compressed_start);
    esp_loader_flash_deflate_cfg_t flash_config = {
        .offset = segment->flash_offset,
        .image_size = segment->raw_size,
        .compressed_size = compressed_size,
        .block_size = C5_FLASH_BLOCK_SIZE,
    };
    static uint8_t payload[C5_FLASH_BLOCK_SIZE];

    ESP_LOGI(TAG, "C5 flash %s: offset=0x%lx raw=%lu compressed=%lu",
             segment->name, (unsigned long)segment->flash_offset,
             (unsigned long)segment->raw_size,
             (unsigned long)compressed_size);

    esp_loader_error_t loader_error =
        esp_loader_flash_deflate_start(loader, &flash_config);
    if (loader_error != ESP_LOADER_SUCCESS) {
        return loader_error;
    }

    uint32_t written = 0;
    unsigned last_percent = 0;
    while (written < compressed_size) {
        const uint32_t remaining = compressed_size - written;
        const uint32_t block_size = remaining < sizeof(payload) ?
                                    remaining : sizeof(payload);
        memcpy(payload, segment->compressed_start + written, block_size);
        loader_error = esp_loader_flash_deflate_write(
            loader, &flash_config, payload, block_size);
        if (loader_error != ESP_LOADER_SUCCESS) {
            (void)esp_loader_flash_deflate_finish(loader, &flash_config);
            return loader_error;
        }
        written += block_size;

        const unsigned percent =
            (unsigned)(((uint64_t)written * 100U) / compressed_size);
        if (percent == 100U || percent >= last_percent + 10U) {
            ESP_LOGI(TAG, "C5 flash %s: %u%%", segment->name, percent);
            last_percent = percent;
        }
    }

    loader_error = esp_loader_flash_deflate_finish(loader, &flash_config);
    if (loader_error != ESP_LOADER_SUCCESS) {
        return loader_error;
    }
    return esp_loader_flash_verify_known_md5(
        loader, segment->flash_offset, segment->raw_size,
        segment->raw_md5);
}

static esp_err_t c5_provision_hosted_mcu(void)
{
    esp_loader_t loader;
    bool loader_initialized = false;
    bool target_connected = false;
    esp_err_t result = ESP_FAIL;

    esp32_sdio_port_t port = {
        .port.ops = &esp32_sdio_ops,
        .slot = SDMMC_HOST_SLOT_1,
        .max_freq_khz = SDMMC_FREQ_DEFAULT,
        .reset_pin = JETKVM_C5_PIN_EN,
        .boot_pin = JETKVM_C5_PIN_BOOT,
        .bus_width = SDIO_4BIT,
        .sdio_cmd_pin = JETKVM_C5_PIN_SDIO_CMD,
        .sdio_clk_pin = JETKVM_C5_PIN_SDIO_CLK,
        .sdio_d0_pin = JETKVM_C5_PIN_SDIO_D0,
        .sdio_d1_pin = JETKVM_C5_PIN_SDIO_D1,
        .sdio_d2_pin = JETKVM_C5_PIN_SDIO_D2,
        .sdio_d3_pin = JETKVM_C5_PIN_SDIO_D3,
    };

    ESP_LOGI(TAG,
             "Trying C5 BootROM SDIO provisioning: BOOT=%d EN=%d CMD=%d CLK=%d D0=%d D1=%d D2=%d D3=%d",
             JETKVM_C5_PIN_BOOT, JETKVM_C5_PIN_EN,
             JETKVM_C5_PIN_SDIO_CMD, JETKVM_C5_PIN_SDIO_CLK,
             JETKVM_C5_PIN_SDIO_D0, JETKVM_C5_PIN_SDIO_D1,
             JETKVM_C5_PIN_SDIO_D2, JETKVM_C5_PIN_SDIO_D3);

    esp_loader_error_t loader_error = esp_loader_init_sdio(&loader, &port.port);
    if (loader_error != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG, "C5 BootROM SDIO init failed: loader error %d",
                 loader_error);
        goto cleanup;
    }
    loader_initialized = true;

    esp_loader_connect_args_t connect_config = ESP_LOADER_CONNECT_DEFAULT();
    loader_error = esp_loader_connect(&loader, &connect_config);
    if (loader_error != ESP_LOADER_SUCCESS) {
        ESP_LOGE(TAG,
                 "C5 BootROM SDIO connect failed before firmware transfer: loader error %d",
                 loader_error);
        goto cleanup;
    }
    target_connected = true;

    /* Boot mode was sampled when EN rose; release the carrier control only
     * after CMD5, CIS discovery and the flasher-stub upload have succeeded. */
    gpio_set_level(JETKVM_C5_PIN_BOOT, 1);

    if (esp_loader_get_target(&loader) != ESP32C5_CHIP) {
        ESP_LOGE(TAG, "BootROM target is not ESP32-C5");
        goto cleanup;
    }
    ESP_LOGI(TAG, "C5 BootROM connected; checking ESP-Hosted-MCU 2.12.3 image");

    enum {
        C5_RELEASE_SEGMENT_COUNT =
            sizeof(c5_release_segments) / sizeof(c5_release_segments[0]),
    };
    bool segment_matches[C5_RELEASE_SEGMENT_COUNT] = {false};
    bool release_changed = false;

    /* OTA selection data is initialized to erased flash in the release image,
     * then legitimately changes as soon as the C5 bootloader selects ota_0.
     * Compare the immutable artifacts first so normal boots do not rewrite
     * that runtime state. A real release update resets it together with the
     * changed application/partition artifacts. */
    for (size_t index = 0; index < C5_RELEASE_SEGMENT_COUNT; ++index) {
        const c5_release_segment_t *segment = &c5_release_segments[index];
        if (segment->runtime_mutable) {
            continue;
        }
        loader_error = esp_loader_flash_verify_known_md5(
            &loader, segment->flash_offset, segment->raw_size,
            segment->raw_md5);
        if (loader_error == ESP_LOADER_SUCCESS) {
            segment_matches[index] = true;
            ESP_LOGI(TAG, "C5 %s already matches release", segment->name);
        } else {
            release_changed = true;
        }
    }

    for (size_t index = 0; index < C5_RELEASE_SEGMENT_COUNT; ++index) {
        const c5_release_segment_t *segment = &c5_release_segments[index];
        if (segment->runtime_mutable && !release_changed) {
            ESP_LOGI(TAG, "C5 %s retained (bootloader-managed runtime state)",
                     segment->name);
            continue;
        }
        if (!segment->runtime_mutable && segment_matches[index]) {
            continue;
        }

        ESP_LOGW(TAG, "C5 %s differs; flashing ESP-Hosted-MCU image",
                 segment->name);
        loader_error = c5_flash_release_segment(&loader, segment);
        if (loader_error != ESP_LOADER_SUCCESS) {
            ESP_LOGE(TAG, "C5 %s flash/verify failed: loader error %d",
                     segment->name, loader_error);
            goto cleanup;
        }
    }

    ESP_LOGI(TAG, "C5 ESP-Hosted-MCU release artifacts verified");
    result = ESP_OK;

cleanup:
    if (target_connected) {
        /* The flasher port released BOOT high after entering download mode,
         * so this reset selects the newly programmed normal flash boot. */
        esp_loader_reset_target(&loader);
    }
    if (loader_initialized) {
        esp_loader_deinit(&loader);
    }

    esp_err_t boot_error = c5_prepare_normal_boot();
    if (result == ESP_OK && boot_error != ESP_OK) {
        result = boot_error;
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    return result;
}

static void c5_stop_bt_controller(void)
{
    if (s_bt_controller_enabled) {
        esp_err_t error = esp_hosted_bt_controller_disable();
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "C5 BLE controller disable failed: %s",
                     esp_err_to_name(error));
        } else {
            s_bt_controller_enabled = false;
        }
    }
    if (s_bt_controller_initialized && !s_bt_controller_enabled) {
        esp_err_t error = esp_hosted_bt_controller_deinit(false);
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "C5 BLE controller deinit failed: %s",
                     esp_err_to_name(error));
        } else {
            s_bt_controller_initialized = false;
        }
    }
}

/* Firmware before the saved-network list (0.5.6 and older) reads one
 * wifi_sta_config_t under sta_cfg. Keep there a copy of the last network the
 * station joined, so an update to such a release still joins a network
 * instead of starting BLE setup. The copy names only a network in the saved
 * list: the caller does not hold s_networks_lock, and the write checks the
 * list under it, so a removal (c5_save_networks_locked erases the copy)
 * cannot be undone. A network joined before it is saved gets its copy when
 * c5_store_network saves it. */
static void c5_legacy_copy_write(const uint8_t *ssid, size_t ssid_len,
                                 const uint8_t *password, size_t password_len)
{
    wifi_sta_config_t copy = {0};
    wifi_sta_config_t stored;
    memcpy(copy.ssid, ssid, ssid_len < sizeof(copy.ssid) ? ssid_len : sizeof(copy.ssid));
    memcpy(copy.password, password,
           password_len < sizeof(copy.password) ? password_len : sizeof(copy.password));
    /* Older firmware applies the record as is; like c5_join, a network with
     * a password must not be joined through an open AP of the same name. */
    copy.threshold.authmode = password_len > 0U ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    xSemaphoreTake(s_networks_lock, portMAX_DELAY);
    nvs_handle_t handle;
    esp_err_t error = ESP_OK;
    if (wifi_network_list_find(&s_networks, ssid, ssid_len) < 0) {
        xSemaphoreGive(s_networks_lock);
        memset(&copy, 0, sizeof(copy));
        return;
    }
    error = nvs_open(C5_PROV_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        size_t length = sizeof(stored);
        /* Write only a change, not at every join. */
        if (nvs_get_blob(handle, C5_PROV_NVS_STA_KEY, &stored, &length) != ESP_OK ||
            length != sizeof(stored) || memcmp(&stored, &copy, sizeof(copy)) != 0) {
            error = nvs_set_blob(handle, C5_PROV_NVS_STA_KEY, &copy, sizeof(copy));
            if (error == ESP_OK) error = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    xSemaphoreGive(s_networks_lock);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi copy for older firmware not saved: %s", esp_err_to_name(error));
    }
    memset(&copy, 0, sizeof(copy));
    memset(&stored, 0, sizeof(stored));
}

static esp_err_t c5_save_networks_locked(const wifi_network_list_t *list)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(C5_PROV_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_blob(handle, C5_PROV_NVS_NETWORKS_KEY, list, sizeof(*list));
    if (error == ESP_OK) {
        /* A removed network's password does not stay in the copy for older
         * firmware. */
        wifi_sta_config_t legacy;
        size_t length = sizeof(legacy);
        if (nvs_get_blob(handle, C5_PROV_NVS_STA_KEY, &legacy, &length) == ESP_OK &&
            length == sizeof(legacy) &&
            wifi_network_list_find(list, legacy.ssid,
                                   strnlen((const char *)legacy.ssid,
                                           sizeof(legacy.ssid))) < 0) {
            error = nvs_erase_key(handle, C5_PROV_NVS_STA_KEY);
        }
        memset(&legacy, 0, sizeof(legacy));
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

/* Add or update a network in the saved list and NVS. A full list drops its
 * oldest entry only when `replace_oldest` is set (a BLE setup at a new place). */
static wifi_network_list_result_t c5_store_network(
    const uint8_t *ssid, size_t ssid_len, const uint8_t *password,
    size_t password_len, bool replace_oldest, esp_err_t *save_error)
{
    *save_error = ESP_OK;
    wifi_network_list_t list;
    xSemaphoreTake(s_networks_lock, portMAX_DELAY);
    list = s_networks;
    wifi_network_list_result_t result = wifi_network_list_upsert(
        &list, ssid, ssid_len, password, password_len);
    if (result == WIFI_NETWORK_LIST_FULL && replace_oldest) {
        ESP_LOGW(TAG, "Saved Wi-Fi list is full; dropping %.*s",
                 list.items[0].ssid_len, (const char *)list.items[0].ssid);
        (void)wifi_network_list_remove(&list, list.items[0].ssid, list.items[0].ssid_len);
        result = wifi_network_list_upsert(&list, ssid, ssid_len, password,
                                          password_len);
    }
    if (result == WIFI_NETWORK_LIST_OK) {
        *save_error = c5_save_networks_locked(&list);
        if (*save_error == ESP_OK) {
            s_networks = list;
        }
    }
    xSemaphoreGive(s_networks_lock);
    memset(&list, 0, sizeof(list));
    if (result == WIFI_NETWORK_LIST_OK && *save_error == ESP_OK) {
        /* A saved password is worth trying at the next rescan. */
        c5_auth_failure_set(ssid, ssid_len, false);
        /* A network joined before it was saved (connect-now, BLE setup)
         * gets its copy now. */
        portENTER_CRITICAL(&s_settings_lock);
        bool current = strlen(s_current_ssid) == ssid_len &&
                       memcmp(s_current_ssid, ssid, ssid_len) == 0;
        portEXIT_CRITICAL(&s_settings_lock);
        if (current) c5_legacy_copy_write(ssid, ssid_len, password, password_len);
    }
    return result;
}

/* Load the saved list. Older firmware stored one wifi_sta_config_t under
 * sta_cfg; move it into an empty list once. The key stays as the copy for
 * older firmware (c5_legacy_copy_write). */
static esp_err_t c5_load_networks(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(C5_PROV_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }

    wifi_network_list_t list;
    wifi_network_list_init(&list);
    size_t length = 0;
    error = nvs_get_blob(handle, C5_PROV_NVS_NETWORKS_KEY, NULL, &length);
    if (error == ESP_OK) {
        wifi_network_list_t stored;
        size_t read = sizeof(stored);
        if (length != sizeof(stored) ||
            nvs_get_blob(handle, C5_PROV_NVS_NETWORKS_KEY, &stored, &read) != ESP_OK ||
            !wifi_network_list_decode(&list, &stored, read)) {
            ESP_LOGW(TAG, "Discarding invalid saved Wi-Fi network list");
            error = nvs_erase_key(handle, C5_PROV_NVS_NETWORKS_KEY);
            if (error == ESP_OK) {
                error = nvs_commit(handle);
            }
        }
        memset(&stored, 0, sizeof(stored));
    } else if (error == ESP_ERR_NVS_NOT_FOUND) {
        error = ESP_OK;
    }

    if (error == ESP_OK) {
        wifi_sta_config_t legacy;
        size_t legacy_length = sizeof(legacy);
        esp_err_t legacy_error = nvs_get_blob(handle, C5_PROV_NVS_STA_KEY,
                                              &legacy, &legacy_length);
        bool valid = legacy_error == ESP_OK && legacy_length == sizeof(legacy);
        bool changed = false;
        if (valid && list.count == 0U &&
            wifi_network_list_from_legacy(&list, legacy.ssid, legacy.password)) {
            /* Static IPv4 settings were made for this network. Bind them
             * first. If that fails, the move is retried on the next boot, and
             * the network is used from RAM now. */
            char ssid[WIFI_NETWORK_SSID_MAX + 1U] = {0};
            memcpy(ssid, list.items[0].ssid, list.items[0].ssid_len);
            if (jetkvm_network_settings_bind_static_if_unbound(ssid) != ESP_OK) {
                ESP_LOGE(TAG, "Static IPv4 binding failed; keeping the legacy Wi-Fi record");
            } else {
                error = nvs_set_blob(handle, C5_PROV_NVS_NETWORKS_KEY, &list,
                                     sizeof(list));
                changed = true;
                if (error == ESP_OK) {
                    ESP_LOGI(TAG, "Moved the saved Wi-Fi network into the network list");
                }
            }
        } else if ((legacy_error == ESP_OK || legacy_error == ESP_ERR_NVS_INVALID_LENGTH) &&
                   (!valid || wifi_network_list_find(
                                  &list, legacy.ssid,
                                  strnlen((const char *)legacy.ssid, sizeof(legacy.ssid))) < 0)) {
            /* A damaged record, or one for a network that is not in the list
             * (older firmware wrote it), is erased, not merged. */
            error = nvs_erase_key(handle, C5_PROV_NVS_STA_KEY);
            changed = true;
        }
        if (error == ESP_OK && changed) {
            error = nvs_commit(handle);
        }
        memset(&legacy, 0, sizeof(legacy));
    }
    nvs_close(handle);

    if (error == ESP_OK) {
        xSemaphoreTake(s_networks_lock, portMAX_DELAY);
        s_networks = list;
        xSemaphoreGive(s_networks_lock);
        ESP_LOGI(TAG, "%u saved Wi-Fi network(s)", list.count);
    }
    memset(&list, 0, sizeof(list));
    return error;
}

static esp_err_t c5_clear_legacy_credentials_once(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(C5_PROV_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }

    uint8_t cleared = 0;
    error = nvs_get_u8(handle, C5_PROV_NVS_C5_CLEARED_KEY, &cleared);
    if (error == ESP_OK && cleared == 1U) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return error;
    }

    /* esp_wifi_restore() is transported over ESP-Hosted and clears the
     * legacy persistent copy in the C5. The marker lives in P4 NVS, so a P4
     * NVS erase deliberately repeats this cleanup and returns to provisioning. */
    error = esp_wifi_restore();
    if (error == ESP_OK) {
        error = nvs_set_u8(handle, C5_PROV_NVS_C5_CLEARED_KEY, 1U);
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);

    if (error == ESP_OK) {
        ESP_LOGI(TAG, "Removed legacy Wi-Fi credentials from C5 flash");
    }
    return error;
}

static void c5_provisioning_app_event_handler(
    void *user_data, network_prov_cb_event_t event, void *event_data)
{
    (void)user_data;

    if (event == NETWORK_PROV_WIFI_CRED_RECV && event_data != NULL) {
        memcpy(&s_pending_station_config, event_data,
               sizeof(s_pending_station_config));
        s_pending_station_config_valid = s_pending_station_config.ssid[0] != 0U;
        if (s_pending_station_config_valid) {
            size_t ssid_len = strnlen((const char *)s_pending_station_config.ssid,
                                      sizeof(s_pending_station_config.ssid));
            c5_set_current_ssid(s_pending_station_config.ssid, ssid_len);
            /* The provisioning manager joins this network itself; set its
             * IPv4 mode first (static settings belong to another network). */
            char ssid[WIFI_NETWORK_SSID_MAX + 1U] = {0};
            memcpy(ssid, s_pending_station_config.ssid, ssid_len);
            jetkvm_network_settings_set_wifi_network(ssid);
        }
        return;
    }

    if (event == NETWORK_PROV_WIFI_CRED_SUCCESS) {
        if (!s_pending_station_config_valid) {
            ESP_LOGE(TAG, "Provisioning succeeded without pending credentials");
            return;
        }
        const wifi_sta_config_t *config = &s_pending_station_config;
        esp_err_t save_error;
        wifi_network_list_result_t result = c5_store_network(
            config->ssid, strnlen((const char *)config->ssid, sizeof(config->ssid)),
            config->password,
            strnlen((const char *)config->password, sizeof(config->password)),
            true, &save_error);
        if (result != WIFI_NETWORK_LIST_OK || save_error != ESP_OK) {
            ESP_LOGE(TAG, "Saving the provisioned Wi-Fi network failed: result=%d %s",
                     (int)result, esp_err_to_name(save_error));
        } else {
            ESP_LOGI(TAG, "Provisioned Wi-Fi network saved in P4 NVS");
        }
        s_pending_station_config_valid = false;
        memset(&s_pending_station_config, 0,
               sizeof(s_pending_station_config));
    }
}

static void c5_wifi_event_handler(void *argument, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)argument;

    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
        case NETWORK_PROV_START:
            ESP_LOGI(TAG, "BLE Wi-Fi provisioning started");
            break;
        case NETWORK_PROV_WIFI_CRED_RECV: {
            const wifi_sta_config_t *config = event_data;
            if (config != NULL) {
                char ssid[sizeof(config->ssid) + 1U] = {0};
                memcpy(ssid, config->ssid, sizeof(config->ssid));
                ESP_LOGI(TAG, "Provisioning credentials received for SSID=%s", ssid);
                jetkvm_board_network_set_wifi_setup_phone(
                    JETKVM_SCREEN_WIFI_SETUP_JOINING, ssid);
            }
            break;
        }
        case NETWORK_PROV_WIFI_CRED_FAIL: {
            const network_prov_wifi_sta_fail_reason_t *reason = event_data;
            const char *description =
                reason != NULL && *reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR ?
                "authentication failed" : "access point not found";
            ESP_LOGW(TAG, "Provisioning connection failed: %s; retry allowed",
                     description);
            /* The manager reports two reasons; the Accepted screen shows
             * them with the texts of a saved network's join. */
            const wifi_join_failure_t failure = {
                .kind = reason != NULL && *reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR ?
                            WIFI_JOIN_WRONG_PASSWORD : WIFI_JOIN_NOT_FOUND,
                .reason = 0,
            };
            if (__atomic_load_n(&s_pending_station_config_valid, __ATOMIC_ACQUIRE)) {
                c5_show_failure(s_pending_station_config.ssid,
                                strnlen((const char *)s_pending_station_config.ssid,
                                        sizeof(s_pending_station_config.ssid)),
                                failure);
            } else {
                c5_show_failure(NULL, 0, failure);
            }
            /* The setup device shows why and can send a network again. */
            jetkvm_board_network_set_wifi_setup_phone(
                JETKVM_SCREEN_WIFI_SETUP_ACCEPTED, NULL);
            /* The join with these credentials has ended. */
            __atomic_store_n(&s_pending_station_config_valid, false, __ATOMIC_RELEASE);
            esp_err_t error =
                network_prov_mgr_reset_wifi_sm_state_on_failure();
            if (error != ESP_OK) {
                ESP_LOGE(TAG, "Provisioning retry reset failed: %s",
                         esp_err_to_name(error));
            }
            break;
        }
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            ESP_LOGI(TAG, "Wi-Fi provisioning succeeded");
            break;
        case NETWORK_PROV_END: {
            __atomic_store_n(&s_provisioning_active, false, __ATOMIC_RELEASE);
            __atomic_store_n(&s_ble_central_connected, false, __ATOMIC_RELEASE);
            /* The code stays for a restart in the same offline period; one
             * that is no longer issued is erased once the service stops. */
            if (!__atomic_load_n(&s_pop_issued, __ATOMIC_ACQUIRE)) {
                memset(s_pop, 0, sizeof(s_pop));
            }
            jetkvm_board_network_set_wifi_setup(false, NULL);
            ESP_LOGI(TAG, "BLE Wi-Fi provisioning stopped");
            /* The scheme has already stopped NimBLE here; ble_gap_init clears
             * the listener list on the next start, so no unregister. */
            esp_err_t error = network_prov_mgr_deinit();
            if (error != ESP_OK) {
                ESP_LOGW(TAG, "Provisioning manager deinit failed: %s",
                         esp_err_to_name(error));
            } else {
                s_provisioning_manager_initialized = false;
            }
            c5_stop_bt_controller();
            c5_conn_notify(C5_NOTIFY_PROV_END);
            break;
        }
        default:
            break;
        }
        return;
    }

    if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        if (event_id == PROTOCOMM_SECURITY_SESSION_SETUP_OK) {
            /* The phone has the code; the screen moves on at once. */
            jetkvm_board_network_set_wifi_setup_phone(
                JETKVM_SCREEN_WIFI_SETUP_ACCEPTED, NULL);
        } else if (event_id == PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH) {
            /* A failed session after an accepted one on the same link
             * needs the code on the screen again. */
            jetkvm_board_network_set_wifi_setup_phone(
                JETKVM_SCREEN_WIFI_SETUP_CODE, NULL);
            unsigned misses = __atomic_add_fetch(&s_pop_mismatches, 1U, __ATOMIC_ACQ_REL);
            ESP_LOGW(TAG, "Wrong Wi-Fi setup code (%u of %u)", misses,
                     C5_POP_MISMATCH_LIMIT);
            if (misses == C5_POP_MISMATCH_LIMIT) {
                c5_conn_notify(C5_NOTIFY_POP_ROTATE);
            }
        }
        return;
    }

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_SCAN_DONE) {
            const wifi_event_sta_scan_done_t *done = event_data;
            c5_conn_notify(C5_NOTIFY_SCAN_DONE);
            ESP_LOGI(TAG, "Wi-Fi scan done: status=%lu APs=%u id=%u",
                     done != NULL ? (unsigned long)done->status : 0UL,
                     done != NULL ? done->number : 0U,
                     done != NULL ? done->scan_id : 0U);
            return;
        }
        if (event_id == WIFI_EVENT_STA_CONNECTED) {
            __atomic_store_n(&s_station_associated, true, __ATOMIC_RELEASE);
        } else if (event_id == WIFI_EVENT_STA_STOP) {
            __atomic_store_n(&s_station_associated, false, __ATOMIC_RELEASE);
            __atomic_store_n(&s_station_connected, false, __ATOMIC_RELEASE);
            __atomic_store_n(&s_station_stopped, true, __ATOMIC_RELEASE);
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            const wifi_event_sta_disconnected_t *disconnected = event_data;
            __atomic_store_n(&s_station_associated, false, __ATOMIC_RELEASE);
            const bool was_connected =
                __atomic_exchange_n(&s_station_connected, false, __ATOMIC_ACQ_REL);
            /* The next network's channel and signal come with the next link
             * refresh; do not show the old ones next to its name. */
            portENTER_CRITICAL(&s_settings_lock);
            s_wifi_settings.channel = 0;
            s_wifi_settings.rssi = 0;
            portEXIT_CRITICAL(&s_settings_lock);
            /* After a link drop the connection task tries again at once or
             * after a short backoff; keep the status screen instead of
             * flipping to the offline HDMI preview in between. It clears this
             * when the device goes offline. A disconnect that ends a join
             * leaves the screen to the connection task, which has already
             * set it and may have reported the failure since. */
            if (was_connected && !__atomic_load_n(&s_provisioning_active, __ATOMIC_ACQUIRE)) {
                char ssid[sizeof(s_current_ssid)];
                portENTER_CRITICAL(&s_settings_lock);
                memcpy(ssid, s_current_ssid, sizeof(ssid));
                portEXIT_CRITICAL(&s_settings_lock);
                jetkvm_board_network_set_wifi_joining(ssid);
            }
            jetkvm_board_network_set_wifi_state(false, NULL, NULL);
            ESP_LOGI(TAG, "Wi-Fi disconnected reason=%u rssi=%d",
                     disconnected != NULL ? disconnected->reason : 0U,
                     disconnected != NULL ? disconnected->rssi : 0);
            __atomic_store_n(&s_disconnect_reason,
                             disconnected != NULL ? disconnected->reason : 0U,
                             __ATOMIC_RELEASE);
            /* The connection task decides whether and where to reconnect. */
            c5_conn_notify(C5_NOTIFY_DISCONNECTED);
        }
        return;
    }

    if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP && event_data != NULL) {
            const ip_event_got_ip_t *event = event_data;
            uint8_t mac[6] = {0};
            const uint8_t *mac_pointer =
                esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK ? mac : NULL;
            jetkvm_board_network_set_wifi_state(
                true, mac_pointer, &event->ip_info.ip);
            __atomic_store_n(&s_station_connected, true, __ATOMIC_RELEASE);
            /* The next offline period gets a new setup code. */
            c5_pop_retire();
            /* Online again, whoever brought the link back. */
            jetkvm_board_network_set_wifi_joining(NULL);
            c5_conn_notify(C5_NOTIFY_GOT_IP);
            c5_start_status_task_if_needed();
            esp_netif_dns_info_t dns = {0};
            const esp_err_t dns_error = esp_netif_get_dns_info(
                event->esp_netif, ESP_NETIF_DNS_MAIN, &dns);
            if (dns_error == ESP_OK &&
                dns.ip.type == ESP_IPADDR_TYPE_V4) {
                ESP_LOGI(TAG,
                         "Wi-Fi connected: " IPSTR " gateway " IPSTR
                         " DNS " IPSTR,
                         IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw),
                         IP2STR(&dns.ip.u_addr.ip4));
            } else {
                ESP_LOGW(TAG,
                         "Wi-Fi connected: " IPSTR " gateway " IPSTR
                         "; DNS unavailable: %s",
                         IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.gw),
                         esp_err_to_name(dns_error));
            }
        } else if (event_id == IP_EVENT_STA_LOST_IP) {
            __atomic_store_n(&s_station_connected, false, __ATOMIC_RELEASE);
            jetkvm_board_network_set_wifi_state(false, NULL, NULL);
        }
    }
}

static void c5_refresh_link_details(void)
{
    static uint8_t last_channel;
    if (__atomic_load_n(&s_scan_active, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (!__atomic_load_n(&s_station_connected, __ATOMIC_ACQUIRE)) {
        last_channel = 0;
        return;
    }

    wifi_ap_record_t access_point = {0};
    if (esp_wifi_sta_get_ap_info(&access_point) != ESP_OK ||
        !__atomic_load_n(&s_station_connected, __ATOMIC_ACQUIRE)) {
        return;
    }
    portENTER_CRITICAL(&s_settings_lock);
    s_wifi_settings.channel = access_point.primary;
    s_wifi_settings.rssi = access_point.rssi;
    memcpy(s_wifi_settings.ssid, access_point.ssid, 32);
    s_wifi_settings.ssid[32] = '\0';
    portEXIT_CRITICAL(&s_settings_lock);
    if (access_point.primary != last_channel) {
        int8_t tx_power = 0;
        esp_err_t power_error = esp_wifi_get_max_tx_power(&tx_power);
        ESP_LOGI(TAG, "Wi-Fi TX power limit: raw=%d (0.25 dBm units), %s",
                 tx_power, esp_err_to_name(power_error));
        ESP_LOGI(TAG, "Wi-Fi link: %s channel=%u RSSI=%d dBm",
                 access_point.primary > 14 ? "5 GHz" : "2.4 GHz",
                 access_point.primary, access_point.rssi);
        last_channel = access_point.primary;
    }
    jetkvm_board_network_set_wifi_details(
        access_point.ssid,
        strnlen((const char *)access_point.ssid, sizeof(access_point.ssid)),
        access_point.rssi);
}

static void c5_status_task(void *argument)
{
    (void)argument;
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        c5_refresh_link_details();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(C5_STATUS_UPDATE_MS));
    }
}

static void c5_start_status_task_if_needed(void)
{
    if (s_status_task != NULL) {
        return;
    }

    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        c5_status_task, "c5_wifi_status", C5_STATUS_TASK_STACK_BYTES, NULL,
        tskIDLE_PRIORITY + 1, &s_status_task, C5_STATUS_TASK_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        s_status_task = NULL;
        ESP_LOGW(TAG, "Wi-Fi signal monitor unavailable");
    }
}

static esp_err_t c5_register_network_events(void)
{
    esp_err_t error = esp_event_handler_register(
        NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, c5_wifi_event_handler, NULL);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                       c5_wifi_event_handler, NULL);
    if (error != ESP_OK) {
        return error;
    }
    error = esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT,
                                       ESP_EVENT_ANY_ID, c5_wifi_event_handler, NULL);
    if (error != ESP_OK) {
        return error;
    }
    return esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                      c5_wifi_event_handler, NULL);
}

static void c5_log_conn_params(const char *what, uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        ESP_LOGI(TAG, "BLE %s: handle=%u (no descriptor)", what, conn_handle);
        return;
    }
    ESP_LOGI(TAG, "BLE %s: interval=%u.%02u ms latency=%u timeout=%u ms mtu=%u",
             what, (unsigned)(desc.conn_itvl * 5 / 4),
             (unsigned)((desc.conn_itvl * 125) % 100), (unsigned)desc.conn_latency,
             (unsigned)(desc.supervision_timeout * 10), (unsigned)ble_att_mtu(conn_handle));
}

/* The C5 radio pauses the BLE link while it scans one Wi-Fi channel, up to
 * roughly 400 ms on a passive 5 GHz channel. macOS and iOS open with a 720 ms
 * supervision timeout; ask for 4 s so the link survives a scan or a connect
 * attempt. Apple grants peripheral requests between 2 s and 6 s. */
#define C5_BLE_INTERVAL_MIN_UNITS   24  /* 30 ms, in 1.25 ms units */
#define C5_BLE_INTERVAL_MAX_UNITS   36  /* 45 ms */
#define C5_BLE_SUPERVISION_UNITS    400 /* 4 s, in 10 ms units */

static void c5_request_link_params(uint16_t conn_handle)
{
    const struct ble_gap_upd_params params = {
        .itvl_min = C5_BLE_INTERVAL_MIN_UNITS,
        .itvl_max = C5_BLE_INTERVAL_MAX_UNITS,
        .latency = 0,
        .supervision_timeout = C5_BLE_SUPERVISION_UNITS,
    };
    int rc = ble_gap_update_params(conn_handle, &params);
    if (rc != 0) {
        ESP_LOGW(TAG, "BLE link parameter request failed: rc=%d", rc);
    }
}

static int c5_gap_event_listener(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            c5_log_conn_params("connected", event->connect.conn_handle);
            /* Show the setup code only while a setup device is connected.
             * The failure line now belongs to the networks it sends. */
            __atomic_store_n(&s_ble_central_connected, true, __ATOMIC_RELEASE);
            jetkvm_board_network_set_wifi_failure(NULL);
            /* The setup page scans next; an offline rescan must end first. */
            c5_conn_notify(C5_NOTIFY_BLE_CENTRAL);
            jetkvm_board_network_set_wifi_setup(true, s_pop);
        } else {
            ESP_LOGW(TAG, "BLE connect failed: status=%d", event->connect.status);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "BLE disconnected: reason=0x%x (%s)",
                 event->disconnect.reason,
                 event->disconnect.reason == BLE_HS_HCI_ERR(0x08) ? "supervision timeout" :
                 event->disconnect.reason == BLE_HS_HCI_ERR(0x13) ? "peer terminated" :
                 event->disconnect.reason == BLE_HS_HCI_ERR(0x16) ? "local terminated" :
                 event->disconnect.reason == BLE_HS_HCI_ERR(0x3e) ? "connection failed to establish" :
                 event->disconnect.reason == BLE_HS_HCI_ERR(0x22) ? "LMP response timeout" : "other");
        __atomic_store_n(&s_ble_central_left_at, xTaskGetTickCount(), __ATOMIC_RELEASE);
        __atomic_store_n(&s_ble_central_connected, false, __ATOMIC_RELEASE);
        jetkvm_board_network_set_wifi_setup(
            __atomic_load_n(&s_provisioning_active, __ATOMIC_ACQUIRE), NULL);
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        if (event->conn_update.status != 0) {
            ESP_LOGW(TAG, "BLE connection update rejected: status=%d",
                     event->conn_update.status);
        }
        c5_log_conn_params("connection updated", event->conn_update.conn_handle);
        break;
    case BLE_GAP_EVENT_MTU:
        /* The central has finished its own setup by the MTU exchange. */
        ESP_LOGI(TAG, "BLE MTU: %u", event->mtu.value);
        c5_request_link_params(event->mtu.conn_handle);
        break;
    default:
        break;
    }
    return 0;
}

static esp_err_t c5_start_ble_provisioning(void)
{
    s_pending_station_config_valid = false;
    memset(&s_pending_station_config, 0, sizeof(s_pending_station_config));

    /* The controller reads ESP_MAC_BT when it initializes. */
    if (s_factory_mac_in_use) {
        esp_err_t mac_error = esp_hosted_iface_mac_addr_set(s_factory_mac, sizeof(s_factory_mac), ESP_MAC_BT);
        if (mac_error != ESP_OK) {
            ESP_LOGW(TAG, "Setting the factory MAC for BLE failed (%s); BLE uses the C5's address",
                     esp_err_to_name(mac_error));
        }
    }
    esp_err_t error = esp_hosted_bt_controller_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "C5 BLE controller init failed: %s",
                 esp_err_to_name(error));
        return error;
    }
    s_bt_controller_initialized = true;

    error = esp_hosted_bt_controller_enable();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "C5 BLE controller enable failed: %s",
                 esp_err_to_name(error));
        c5_stop_bt_controller();
        return error;
    }
    s_bt_controller_enabled = true;

    uint8_t mac[6] = {0};
    error = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (error != ESP_OK) {
        c5_stop_bt_controller();
        return error;
    }
    (void)snprintf(s_provisioning_name, sizeof(s_provisioning_name),
                   "JetKVM_%02X%02X%02X", mac[3], mac[4], mac[5]);

    /* IDF's Security 1 logs the session key and handshake values at INFO. */
    esp_log_level_set("security1", ESP_LOG_WARN);

    /* A random proof of possession per offline period (see s_pop_issued). It
     * stays in RAM and appears on the screen only while a phone is connected.
     * A bench build uses the fixed C5_BENCH_POP instead. */
    if (!__atomic_load_n(&s_pop_issued, __ATOMIC_ACQUIRE)) {
#if JETKVM_WIFI_SETUP_LOG_CODE
        (void)snprintf(s_pop, sizeof(s_pop), "%s", C5_BENCH_POP);
#else
        (void)snprintf(s_pop, sizeof(s_pop), "%06lu",
                       (unsigned long)(esp_random() % 1000000UL));
#endif
        __atomic_store_n(&s_pop_mismatches, 0U, __ATOMIC_RELEASE);
        __atomic_store_n(&s_pop_issued, true, __ATOMIC_RELEASE);
    }

    __atomic_store_n(&s_provisioning_active, true, __ATOMIC_RELEASE);
    error = network_prov_mgr_start_provisioning(
        NETWORK_PROV_SECURITY_1, s_pop,
        s_provisioning_name, NULL);
    if (error != ESP_OK) {
        __atomic_store_n(&s_provisioning_active, false, __ATOMIC_RELEASE);
        c5_pop_forget();
        ESP_LOGE(TAG, "BLE provisioning service start failed: %s",
                 esp_err_to_name(error));
        c5_stop_bt_controller();
        return error;
    }
    s_wifi_started = true;
    int rc = ble_gap_event_listener_register(&s_gap_listener,
                                             c5_gap_event_listener, NULL);
    if (rc != 0) {
        /* The listener shows the setup code; without it nobody can pair.
         * The caller stops the service and the offline loop retries. */
        ESP_LOGE(TAG, "BLE GAP listener register failed: rc=%d", rc);
        __atomic_store_n(&s_provisioning_active, false, __ATOMIC_RELEASE);
        /* The service still runs until the caller deinitializes it. */
        c5_pop_retire();
        return ESP_FAIL;
    }

    jetkvm_board_network_set_wifi_setup(true, NULL);
    ESP_LOGI(TAG, "Wi-Fi setup: connect to BLE device %s", s_provisioning_name);
#if JETKVM_WIFI_SETUP_LOG_CODE
    /* Bench builds only: tools/e2e/p4_wifi_ble_setup_e2e.py reads the code
     * here because it cannot read the screen. */
    ESP_LOGI(TAG, "Wi-Fi setup code: %s", s_pop);
#endif
    return ESP_OK;
}

static network_prov_mgr_config_t c5_provisioning_config(void)
{
    return (network_prov_mgr_config_t) {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = {
            .event_cb = c5_provisioning_app_event_handler,
            .user_data = NULL,
        },
        .network_prov_wifi_conn_cfg = {
            .wifi_conn_attempts = C5_PROV_CONNECT_ATTEMPTS,
        },
        .wifi_storage = WIFI_STORAGE_RAM,
    };
}

/* ---- Connection task ------------------------------------------------- */

typedef enum {
    C5_CONN_SCAN,
    C5_CONN_CONNECTED,
    C5_CONN_OFFLINE,
} c5_conn_state_t;

typedef enum {
    C5_JOIN_OK,
    C5_JOIN_AUTH,
    C5_JOIN_NOT_FOUND,
    C5_JOIN_FAILED,
} c5_join_result_t;

/* Notification bits received but not yet handled; connection task only. */
static uint32_t s_conn_pending;

static void c5_conn_collect(TickType_t ticks)
{
    uint32_t bits = 0;
    if (xTaskNotifyWait(0, UINT32_MAX, &bits, ticks) == pdTRUE) {
        s_conn_pending |= bits;
    }
}

/* Wait until one of `wake` is pending or the timeout passes. The bits stay
 * pending; the caller clears the ones it handles. */
static uint32_t c5_conn_wait(uint32_t wake, uint32_t timeout_ms)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        c5_conn_collect(0);
        if ((s_conn_pending & wake) != 0U) {
            return s_conn_pending & wake;
        }
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (timeout_ms != UINT32_MAX && elapsed >= limit) {
            return 0;
        }
        c5_conn_collect(timeout_ms == UINT32_MAX ? portMAX_DELAY : limit - elapsed);
    }
}

static void c5_conn_clear(uint32_t bits)
{
    c5_conn_collect(0);
    s_conn_pending &= ~bits;
}

static void c5_conn_disconnect(void)
{
    /* An associated station (also one still waiting for DHCP) answers with
     * a disconnect event; wait for it, so it cannot end the next join. */
    if (!__atomic_load_n(&s_station_connected, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&s_station_associated, __ATOMIC_ACQUIRE)) {
        (void)esp_wifi_disconnect();
        return;
    }
    c5_conn_clear(C5_NOTIFY_DISCONNECTED);
    (void)esp_wifi_disconnect();
    (void)c5_conn_wait(C5_NOTIFY_DISCONNECTED, 3000U);
    c5_conn_clear(C5_NOTIFY_DISCONNECTED | C5_NOTIFY_GOT_IP);
}

_Static_assert(WIFI_JOIN_REASON_ASSOC_TOOMANY == WIFI_REASON_ASSOC_TOOMANY, "reason");
_Static_assert(WIFI_JOIN_REASON_MIC_FAILURE == WIFI_REASON_MIC_FAILURE, "reason");
_Static_assert(WIFI_JOIN_REASON_4WAY_HANDSHAKE_TIMEOUT == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT,
               "reason");
_Static_assert(WIFI_JOIN_REASON_802_1X_AUTH_FAILED == WIFI_REASON_802_1X_AUTH_FAILED, "reason");
_Static_assert(WIFI_JOIN_REASON_NO_AP_FOUND == WIFI_REASON_NO_AP_FOUND, "reason");
_Static_assert(WIFI_JOIN_REASON_AUTH_FAIL == WIFI_REASON_AUTH_FAIL, "reason");
_Static_assert(WIFI_JOIN_REASON_ASSOC_FAIL == WIFI_REASON_ASSOC_FAIL, "reason");
_Static_assert(WIFI_JOIN_REASON_HANDSHAKE_TIMEOUT == WIFI_REASON_HANDSHAKE_TIMEOUT, "reason");
_Static_assert(WIFI_JOIN_REASON_ASSOC_COMEBACK_TIME_TOO_LONG ==
                   WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG, "reason");
_Static_assert(WIFI_JOIN_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ==
                   WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY, "reason");
_Static_assert(WIFI_JOIN_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ==
                   WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD, "reason");
_Static_assert(WIFI_JOIN_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD ==
                   WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD, "reason");

/* Put the failed join of `ssid` on the display's Wi-Fi status screen as
 * the reason above the SSID ("<reason>\n<SSID>"), so a long SSID that the
 * screen cuts short does not hide the reason. The screen
 * keeps it until the device is online again. */
static void c5_show_failure(const uint8_t *ssid, size_t ssid_len, wifi_join_failure_t failure)
{
    char reason[WIFI_JOIN_FAILURE_TEXT_CAPACITY];
    (void)wifi_join_failure_text(failure, reason, sizeof(reason));
    char text[WIFI_NETWORK_SSID_MAX + 2U + sizeof(reason)];
    if (ssid_len > 0U) {
        (void)snprintf(text, sizeof(text), "%s\n%.*s", reason, (int)ssid_len, (const char *)ssid);
    } else {
        (void)snprintf(text, sizeof(text), "%s", reason);
    }
    jetkvm_board_network_set_wifi_failure(text);
}

/* Join one network and wait for an address. `channel` is where the last
 * scan saw its strongest AP (0 if unknown); the first attempt starts there
 * instead of scanning every channel again. A failure other than a wrong
 * password or a missing AP is tried once more with a full scan. `failure`
 * (may be NULL) receives why the join failed; a failure other than a
 * cancel also goes to the display. */
static c5_join_result_t c5_join(const wifi_saved_network_t *network, uint8_t channel,
                                wifi_join_failure_t *failure)
{
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, network->ssid, network->ssid_len);
    memcpy(config.sta.password, network->password, network->password_len);
    config.sta.channel = channel;
    config.sta.scan_method = channel != 0U ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.rssi_5g_adjustment = 0;
    /* A network saved with a password must not be joined through an open AP
     * that advertises the same SSID; WPA-PSK also admits WPA2 and WPA3. */
    config.sta.threshold.authmode = network->password_len > 0U ? WIFI_AUTH_WPA_PSK :
                                                                 WIFI_AUTH_OPEN;
    c5_set_current_ssid(network->ssid, network->ssid_len);
    ESP_LOGI(TAG, "Joining Wi-Fi %.*s", network->ssid_len, (const char *)network->ssid);
    char ssid[WIFI_NETWORK_SSID_MAX + 1U] = {0};
    memcpy(ssid, network->ssid, network->ssid_len);
    jetkvm_board_network_set_wifi_joining(ssid);
    /* Static IPv4 belongs to one network; set the IPv4 mode before joining. */
    jetkvm_network_settings_set_wifi_network(ssid);

    c5_join_result_t result = C5_JOIN_FAILED;
    wifi_join_failure_t why = {.kind = WIFI_JOIN_OTHER, .reason = 0};
    for (unsigned attempt = 0; attempt < 2U; ++attempt) {
        result = C5_JOIN_FAILED;
        why = (wifi_join_failure_t){.kind = WIFI_JOIN_OTHER, .reason = 0};
        c5_conn_clear(C5_NOTIFY_GOT_IP | C5_NOTIFY_DISCONNECTED);
        esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (error == ESP_OK) {
            error = esp_wifi_connect();
        }
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi connect request failed: %s", esp_err_to_name(error));
            break;
        }
        uint32_t bits = c5_conn_wait(C5_NOTIFY_GOT_IP | C5_NOTIFY_DISCONNECTED |
                                     C5_NOTIFY_COMMANDS, C5_JOIN_TIMEOUT_MS);
        c5_conn_clear(bits & (C5_NOTIFY_GOT_IP | C5_NOTIFY_DISCONNECTED));
        if ((bits & C5_NOTIFY_GOT_IP) != 0U &&
            __atomic_load_n(&s_station_connected, __ATOMIC_ACQUIRE)) {
            result = C5_JOIN_OK;
            why = (wifi_join_failure_t){.kind = WIFI_JOIN_OK, .reason = 0};
            break;
        }
        if ((bits & C5_NOTIFY_COMMANDS) != 0U) {
            /* The task loop handles the command; leave this network. */
            why.kind = WIFI_JOIN_CANCELLED;
            c5_conn_disconnect();
            break;
        }
        if (bits == 0U) {
            why = wifi_join_failure_timeout(
                __atomic_load_n(&s_station_associated, __ATOMIC_ACQUIRE));
            ESP_LOGW(TAG, "Wi-Fi join timed out%s",
                     why.kind == WIFI_JOIN_NO_ADDRESS ? " waiting for DHCP" : "");
            c5_conn_disconnect();
            break;
        }
        why = wifi_join_failure_from_reason(
            __atomic_load_n(&s_disconnect_reason, __ATOMIC_ACQUIRE));
        if (why.kind == WIFI_JOIN_WRONG_PASSWORD) {
            result = C5_JOIN_AUTH;
            if (attempt > 0U) break;
            /* One rejected handshake does not prove a wrong password: an AP
             * can reject a join with reason 202 at a strong signal (-42 dBm)
             * and accept the same password on a later attempt. Try it once
             * more. */
            if (c5_conn_wait(C5_NOTIFY_COMMANDS, C5_AUTH_RETRY_MS) != 0U) {
                result = C5_JOIN_FAILED;
                why.kind = WIFI_JOIN_CANCELLED;
                break;
            }
            continue;
        }
        if (why.kind == WIFI_JOIN_NOT_FOUND && config.sta.channel == 0U) {
            result = C5_JOIN_NOT_FOUND;
            break;
        }
        /* Scan every channel and prefer signal on the second attempt. */
        config.sta.channel = 0;
        config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    }
    memset(&config, 0, sizeof(config));
    if (result == C5_JOIN_OK) {
        jetkvm_board_network_set_wifi_joining(NULL);
        c5_legacy_copy_write(network->ssid, network->ssid_len, network->password,
                             network->password_len);
    } else if (why.kind != WIFI_JOIN_CANCELLED) {
        c5_show_failure(network->ssid, network->ssid_len, why);
    }
    if (failure != NULL) *failure = why;
    return result;
}

/* c5_join sends a PSK (WPA, WPA2 or WPA3 personal) or nothing; other
 * security modes cannot be joined with a saved network. OWE needs no password
 * and counts as open, as in the scan list. */
static wifi_network_security_t c5_security(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
    case WIFI_AUTH_OWE:
        return WIFI_NETWORK_SECURITY_OPEN;
    case WIFI_AUTH_WPA_PSK:
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return WIFI_NETWORK_SECURITY_PSK;
    default:
        return WIFI_NETWORK_SECURITY_OTHER;
    }
}

/* Start a scan and wait for WIFI_EVENT_SCAN_DONE. A blocking scan holds the
 * C5's request handler for the whole scan (about 10 s), so concurrent
 * requests time out and ESP-Hosted then hands their late replies to the
 * wrong caller.
 *
 * With `stop_for_phone`, a phone that connects over BLE stops the scan
 * (ESP_ERR_NOT_FINISHED): the radio runs one scan at a time, so the setup
 * page's scans would fail until this one ended, and the page's first scan
 * result would end this wait with a partial list. Setup started from the
 * menu also stops it, so the device leaves for setup at once. */
static esp_err_t c5_scan_start_and_wait(bool stop_for_phone)
{
    esp_err_t error = ESP_FAIL;
    c5_conn_clear(C5_NOTIFY_SCAN_DONE | C5_NOTIFY_BLE_CENTRAL);
    for (unsigned attempt = 1; attempt <= C5_SCAN_RETRY_COUNT; ++attempt) {
        error = esp_wifi_scan_start(NULL, false);
        if (error != ESP_ERR_WIFI_STATE || attempt == C5_SCAN_RETRY_COUNT) {
            break;
        }
        ESP_LOGW(TAG, "scan not ready, retry %u/%u", attempt, C5_SCAN_RETRY_COUNT);
        vTaskDelay(pdMS_TO_TICKS(C5_SCAN_RETRY_DELAY_MS));
    }
    if (error != ESP_OK) {
        return error;
    }
    uint32_t wake = C5_NOTIFY_SCAN_DONE | C5_NOTIFY_SETUP_START |
                    (stop_for_phone ? C5_NOTIFY_BLE_CENTRAL : 0U);
    /* The GAP handler sets the flag before it notifies; a phone that
     * connected before the clear above is seen here. */
    uint32_t bits = stop_for_phone &&
                            __atomic_load_n(&s_ble_central_connected, __ATOMIC_ACQUIRE)
                        ? C5_NOTIFY_BLE_CENTRAL
                        : c5_conn_wait(wake, C5_SCAN_TIMEOUT_MS);
    if ((bits & C5_NOTIFY_SCAN_DONE) == 0U) {
        (void)esp_wifi_scan_stop();
        if (bits == 0U) {
            error = ESP_ERR_TIMEOUT;
        } else {
            ESP_LOGI(TAG, "Wi-Fi scan stopped: %s",
                     (bits & C5_NOTIFY_SETUP_START) != 0U ? "setup started from the menu" :
                                                            "a phone connected for setup");
            /* Leave the radio idle before the page's first scan. */
            (void)c5_conn_wait(C5_NOTIFY_SCAN_DONE, 1000U);
            error = ESP_ERR_NOT_FINISHED;
        }
    }
    c5_conn_clear(C5_NOTIFY_SCAN_DONE | C5_NOTIFY_BLE_CENTRAL);
    return error;
}

/* Scan once, refresh the cache the web UI reads and return the saved
 * networks in range, strongest first. `snapshot` receives the list the
 * candidate indexes refer to. */
static size_t c5_scan(wifi_network_list_t *snapshot, wifi_network_candidate_t *candidates,
                      const wifi_saved_network_t *target, bool *target_seen,
                      bool stop_for_phone)
{
    if (target_seen != NULL) *target_seen = false;
    wifi_ap_record_t *records = NULL;
    wifi_network_seen_t *seen = NULL;
    jetkvm_wifi_scan_t *cache = calloc(1, sizeof(*cache));
    size_t candidate_count = 0;
    uint16_t count = 0;

    xSemaphoreTake(s_networks_lock, portMAX_DELAY);
    *snapshot = s_networks;
    s_scan_cache.scanning = true;
    xSemaphoreGive(s_networks_lock);

    __atomic_store_n(&s_scan_active, true, __ATOMIC_RELEASE);
    esp_err_t error = cache ? c5_scan_start_and_wait(stop_for_phone) : ESP_ERR_NO_MEM;
    if (error == ESP_OK) {
        error = esp_wifi_scan_get_ap_num(&count);
    }
    if (error == ESP_OK) {
        count = count < C5_SCAN_RECORDS_MAX ? count : C5_SCAN_RECORDS_MAX;
        records = count ? calloc(count, sizeof(*records)) : NULL;
        seen = count ? calloc(count, sizeof(*seen)) : NULL;
        if (count && (!records || !seen)) error = ESP_ERR_NO_MEM;
    }
    if (error == ESP_OK && count) {
        error = esp_wifi_scan_get_ap_records(&count, records);
    }
    if (error != ESP_OK || count == 0U) {
        /* Free the driver's list whenever it was not read in full. */
        (void)esp_wifi_clear_ap_list();
    }
    __atomic_store_n(&s_scan_active, false, __ATOMIC_RELEASE);
    s_scan_failed = error != ESP_OK;
    if (error != ESP_OK) {
        if (error != ESP_ERR_NOT_FINISHED) {
            ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(error));
        }
        count = 0;
    }

    for (uint16_t i = 0; i < count; ++i) {
        memcpy(seen[i].ssid, records[i].ssid, WIFI_NETWORK_SSID_MAX);
        seen[i].rssi = records[i].rssi;
        seen[i].channel = records[i].primary;
        seen[i].security = c5_security(records[i].authmode);
        size_t ssid_len = strnlen((const char *)records[i].ssid, WIFI_NETWORK_SSID_MAX);
        if (ssid_len == 0U) continue;
        /* In range only if an AP with the name can take the target's
         * password (or its absence); connect-now leaves the working network
         * only then. */
        if (target != NULL && target_seen != NULL && ssid_len == target->ssid_len &&
            memcmp(records[i].ssid, target->ssid, ssid_len) == 0 &&
            wifi_network_security_matches(seen[i].security, target->password_len > 0U)) {
            *target_seen = true;
        }
        /* One entry per SSID with its strongest AP, kept sorted by RSSI. */
        size_t at = 0;
        while (at < cache->count &&
               strcmp(cache->entries[at].ssid, (const char *)seen[i].ssid) != 0) ++at;
        if (at < cache->count) {
            if (cache->entries[at].rssi >= records[i].rssi) continue;
            memmove(&cache->entries[at], &cache->entries[at + 1],
                    (cache->count - at - 1U) * sizeof(cache->entries[0]));
            --cache->count;
        }
        jetkvm_wifi_scan_entry_t entry = {
            .rssi = records[i].rssi,
            .channel = records[i].primary,
            .secure = records[i].authmode != WIFI_AUTH_OPEN &&
                      records[i].authmode != WIFI_AUTH_OWE,
            .saved = wifi_network_list_find(snapshot, seen[i].ssid, ssid_len) >= 0,
        };
        memcpy(entry.ssid, seen[i].ssid, ssid_len);
        if (cache->count == JETKVM_WIFI_MAX_SCAN_RESULTS) {
            if (cache->entries[cache->count - 1U].rssi >= entry.rssi) continue;
            --cache->count; /* drop the weakest */
        }
        size_t insert = cache->count;
        while (insert > 0U && cache->entries[insert - 1U].rssi < entry.rssi) {
            cache->entries[insert] = cache->entries[insert - 1U];
            --insert;
        }
        cache->entries[insert] = entry;
        ++cache->count;
    }
    if (seen) {
        candidate_count = wifi_network_list_candidates(snapshot, seen, count, candidates,
                                                       WIFI_NETWORK_LIST_MAX);
    }
    ESP_LOGI(TAG, "Wi-Fi scan: %u APs, %u saved network(s) to try", count,
             (unsigned)candidate_count);

    xSemaphoreTake(s_networks_lock, portMAX_DELAY);
    if (cache && error == ESP_OK) {
        s_scan_cache = *cache;
    }
    s_scan_cache.scanning = false;
    xSemaphoreGive(s_networks_lock);
    free(records);
    free(seen);
    free(cache);
    return candidate_count;
}

/* The setup page's endpoint for the saved networks (see c5_saved_handler). */
#define C5_SAVED_ENDPOINT "jetkvm-saved"
/* Created with each manager init; a deinit discards it. */
static bool s_saved_endpoint_created;

static esp_err_t c5_saved_reply(cJSON *reply, uint8_t **outbuf, ssize_t *outlen)
{
    char *text = reply != NULL ? cJSON_PrintUnformatted(reply) : NULL;
    cJSON_Delete(reply);
    if (text == NULL) return ESP_ERR_NO_MEM;
    /* protocomm frees the reply with free(); cJSON may use its own allocator. */
    size_t len = strlen(text);
    uint8_t *out = malloc(len);
    if (out != NULL) memcpy(out, text, len);
    cJSON_free(text);
    if (out == NULL) return ESP_ERR_NO_MEM;
    *outbuf = out;
    *outlen = (ssize_t)len;
    return ESP_OK;
}

static esp_err_t c5_saved_error(const char *message, uint8_t **outbuf, ssize_t *outlen)
{
    cJSON *reply = cJSON_CreateObject();
    if (reply != NULL) cJSON_AddStringToObject(reply, "error", message);
    return c5_saved_reply(reply, outbuf, outlen);
}

/* Find `ssid` in a copy of the saved list; the index, or -1. */
static int c5_saved_find(const wifi_network_list_t *list, const char *ssid)
{
    size_t ssid_len = strnlen(ssid, WIFI_NETWORK_SSID_MAX + 1U);
    for (uint8_t i = 0; i < list->count && i < WIFI_NETWORK_LIST_MAX; ++i) {
        const wifi_saved_network_t *item = &list->items[i];
        if (item->ssid_len == ssid_len && memcmp(item->ssid, ssid, ssid_len) == 0)
            return (int)i;
    }
    return -1;
}

/* The setup page's requests about the saved networks, over the encrypted
 * session, so only a setup device that entered the code can make them. A
 * request is JSON with an "op":
 * - "list": {"saved":[names, oldest first],"max":8}. The oldest is the one a
 *   full list drops when BLE setup adds a network.
 * - "join" with "ssid": joins that saved network with its saved password,
 *   through the same manager path as credentials the setup device sends, so
 *   the page follows the join with the usual status requests.
 * - "forget" with "ssid": removes it, as Settings > Network does, but also
 *   the last one: the device is in setup, and with no network it stays there.
 * Joins and removals answer {"ok":true} or {"error":"..."}. */
static const char *c5_remove_network(const char *ssid, bool keep_last);
static bool c5_have_saved_networks(void);

static esp_err_t c5_saved_handler(uint32_t session_id, const uint8_t *inbuf,
                                  ssize_t inlen, uint8_t **outbuf, ssize_t *outlen,
                                  void *priv_data)
{
    (void)session_id;
    (void)priv_data;
    if (inbuf == NULL || inlen <= 0) return c5_saved_error("Empty request", outbuf, outlen);
    cJSON *request = cJSON_ParseWithLength((const char *)inbuf, (size_t)inlen);
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(request, "op");
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(request, "ssid");
    esp_err_t result;

    if (cJSON_IsString(op) && strcmp(op->valuestring, "list") == 0) {
        wifi_network_list_t list;
        xSemaphoreTake(s_networks_lock, portMAX_DELAY);
        list = s_networks;
        xSemaphoreGive(s_networks_lock);
        cJSON *reply = cJSON_CreateObject();
        cJSON *names = cJSON_AddArrayToObject(reply, "saved");
        for (uint8_t i = 0; names != NULL && i < list.count && i < WIFI_NETWORK_LIST_MAX; ++i) {
            char name[WIFI_NETWORK_SSID_MAX + 1U] = {0};
            memcpy(name, list.items[i].ssid, list.items[i].ssid_len);
            cJSON_AddItemToArray(names, cJSON_CreateString(name));
        }
        cJSON_AddNumberToObject(reply, "max", WIFI_NETWORK_LIST_MAX);
        memset(&list, 0, sizeof(list));
        if (names == NULL) {
            cJSON_Delete(reply);
            reply = NULL;
        }
        result = c5_saved_reply(reply, outbuf, outlen);
    } else if (cJSON_IsString(op) && strcmp(op->valuestring, "join") == 0 &&
               cJSON_IsString(ssid)) {
        wifi_network_list_t list;
        xSemaphoreTake(s_networks_lock, portMAX_DELAY);
        list = s_networks;
        xSemaphoreGive(s_networks_lock);
        int index = c5_saved_find(&list, ssid->valuestring);
        if (index < 0) {
            result = c5_saved_error("This network is not saved", outbuf, outlen);
        } else {
            const wifi_saved_network_t *item = &list.items[index];
            wifi_config_t config = {0};
            memcpy(config.sta.ssid, item->ssid, item->ssid_len);
            memcpy(config.sta.password, item->password, item->password_len);
            /* As in c5_join: a network saved with a password must not be
             * joined through an open AP that advertises the same SSID. */
            config.sta.threshold.authmode =
                item->password_len > 0U ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
            esp_err_t error = network_prov_mgr_configure_wifi_sta(&config);
            memset(&config, 0, sizeof(config));
            if (error == ESP_OK) {
                cJSON *reply = cJSON_CreateObject();
                if (reply != NULL) cJSON_AddBoolToObject(reply, "ok", true);
                result = c5_saved_reply(reply, outbuf, outlen);
            } else {
                ESP_LOGW(TAG, "Joining a saved network from setup failed: %s",
                         esp_err_to_name(error));
                result = c5_saved_error("The network could not be joined", outbuf, outlen);
            }
        }
        memset(&list, 0, sizeof(list));
    } else if (cJSON_IsString(op) && strcmp(op->valuestring, "forget") == 0 &&
               cJSON_IsString(ssid)) {
        /* A join from setup saves its network when it succeeds, so a
         * forget while one runs would be undone. */
        const char *error =
            __atomic_load_n(&s_pending_station_config_valid, __ATOMIC_ACQUIRE)
                ? "Wait until the device has finished joining a network"
                : c5_remove_network(ssid->valuestring, false);
        if (error != NULL) {
            result = c5_saved_error(error, outbuf, outlen);
        } else {
            /* Menu setup offers Close while a saved network can be returned
             * to; after the last is forgotten, there is none. The call is a
             * no-op once setup has ended. */
            jetkvm_board_network_set_wifi_setup_stoppable(c5_have_saved_networks());
            cJSON *reply = cJSON_CreateObject();
            if (reply != NULL) cJSON_AddBoolToObject(reply, "ok", true);
            result = c5_saved_reply(reply, outbuf, outlen);
        }
    } else {
        result = c5_saved_error("Unknown request", outbuf, outlen);
    }
    cJSON_Delete(request);
    return result;
}

/* The setup page (jetkvm.com/wireless) reads the station MAC from the
 * version reply, so a user can allow the Mini on a router before it joins.
 * The manager carries app data only as a "ver" string and a "cap" list under
 * a label, so the MAC is a "mac:XX:XX:XX:XX:XX:XX" capability under "jetkvm",
 * next to "saved_networks" when the jetkvm-saved endpoint exists. Both must
 * happen before provisioning starts and once per manager init (each call
 * adds an entry). A failure only hides a feature on the page, so it does not
 * stop provisioning. */
static void c5_prov_set_app_info(void)
{
    s_saved_endpoint_created = network_prov_mgr_endpoint_create(C5_SAVED_ENDPOINT) == ESP_OK;
    if (!s_saved_endpoint_created) {
        ESP_LOGW(TAG, "Setup page saved-networks endpoint not created");
    }
    uint8_t mac[6] = {0};
    esp_err_t error = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "No station MAC for the setup page: %s", esp_err_to_name(error));
        return;
    }
    char mac_capability[sizeof("mac:00:00:00:00:00:00")];
    (void)snprintf(mac_capability, sizeof(mac_capability),
                   "mac:%02X:%02X:%02X:%02X:%02X:%02X",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const char *capabilities[] = {mac_capability, "saved_networks"};
    error = network_prov_mgr_set_app_info("jetkvm", esp_app_get_description()->version,
                                          capabilities, s_saved_endpoint_created ? 2 : 1);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Setup page app info not set: %s", esp_err_to_name(error));
    }
}

static esp_err_t c5_prov_start(void)
{
    if (!s_provisioning_manager_initialized) {
        esp_err_t error = network_prov_mgr_init(c5_provisioning_config());
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "Provisioning manager init failed: %s", esp_err_to_name(error));
            return error;
        }
        s_provisioning_manager_initialized = true;
        c5_prov_set_app_info();
    }
    c5_conn_clear(C5_NOTIFY_PROV_END | C5_NOTIFY_POP_ROTATE);
    esp_err_t error = c5_start_ble_provisioning();
    if (error == ESP_OK && s_saved_endpoint_created &&
        network_prov_mgr_endpoint_register(C5_SAVED_ENDPOINT, c5_saved_handler, NULL) != ESP_OK) {
        /* The capability is already advertised; the page reports a failed
         * request, and setup with credentials still works. */
        ESP_LOGW(TAG, "Setup page saved-networks endpoint not registered");
    }
    if (error != ESP_OK) {
        /* Deinit also stops a service that started. */
        if (network_prov_mgr_deinit() == ESP_OK) s_provisioning_manager_initialized = false;
        c5_stop_bt_controller();
        jetkvm_board_network_set_wifi_setup(false, NULL);
    }
    return error;
}

static void c5_prov_stop(void)
{
    if (!__atomic_load_n(&s_provisioning_active, __ATOMIC_ACQUIRE)) {
        return;
    }
    c5_conn_clear(C5_NOTIFY_PROV_END);
    network_prov_mgr_stop_provisioning();
    if (c5_conn_wait(C5_NOTIFY_PROV_END, C5_PROV_STOP_TIMEOUT_MS) == 0U) {
        ESP_LOGW(TAG, "BLE Wi-Fi setup did not stop in time");
    }
    c5_conn_clear(C5_NOTIFY_PROV_END);
}

static esp_err_t c5_conn_restart_radio(void)
{
    __atomic_store_n(&s_station_connected, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_station_stopped, false, __ATOMIC_RELEASE);
    esp_err_t error = esp_wifi_stop();
    if (error == ESP_OK) error = c5_wait_station_stopped();
    if (error == ESP_OK) error = c5_station_start();
    if (error == ESP_OK) error = c5_apply_band();
    return error;
}

/* Try the candidates of one scan in order, strongest first. */
static c5_conn_state_t c5_conn_join_candidates(const wifi_network_list_t *snapshot,
                                               const wifi_network_candidate_t *candidates,
                                               size_t count, wifi_saved_network_t *current)
{
    c5_conn_state_t next = C5_CONN_OFFLINE;
    for (size_t i = 0; i < count && (s_conn_pending & C5_NOTIFY_COMMANDS) == 0U; ++i) {
        const wifi_saved_network_t *network = &snapshot->items[candidates[i].index];
        c5_join_result_t result = c5_join(network, candidates[i].channel, NULL);
        if (result == C5_JOIN_OK || result == C5_JOIN_AUTH) {
            c5_auth_failure_set(network->ssid, network->ssid_len, result == C5_JOIN_AUTH);
        }
        if (result == C5_JOIN_OK) {
            *current = *network;
            next = C5_CONN_CONNECTED;
            break;
        }
        ESP_LOGW(TAG, "Wi-Fi %.*s failed: %s", network->ssid_len,
                 (const char *)network->ssid,
                 result == C5_JOIN_AUTH ? "authentication" :
                 result == C5_JOIN_NOT_FOUND ? "not found" : "no connection");
    }
    if (next == C5_CONN_OFFLINE) {
        /* Offline: the display may fall back to the HDMI preview. */
        jetkvm_board_network_set_wifi_joining(NULL);
    }
    return next;
}

static c5_conn_state_t c5_conn_scan_and_join(wifi_saved_network_t *current)
{
    c5_conn_clear(C5_NOTIFY_RESCAN | C5_NOTIFY_SCAN_REQUEST);
    xSemaphoreTake(s_networks_lock, portMAX_DELAY);
    bool have_networks = s_networks.count > 0U;
    xSemaphoreGive(s_networks_lock);
    if (!have_networks) {
        /* First setup or after a reset: BLE setup is the only way in, so
         * start it without scanning first. */
        jetkvm_board_network_set_wifi_joining(NULL);
        return C5_CONN_OFFLINE;
    }
    jetkvm_board_network_set_wifi_joining("");
    wifi_network_list_t snapshot;
    wifi_network_candidate_t candidates[WIFI_NETWORK_LIST_MAX];
    size_t count = c5_scan(&snapshot, candidates, NULL, NULL, false);
    if (count == 0U && !s_scan_failed) {
        /* No saved network in range: name it when there is only one. */
        const wifi_join_failure_t missing = {.kind = WIFI_JOIN_NOT_FOUND, .reason = 0};
        c5_show_failure(snapshot.items[0].ssid,
                        snapshot.count == 1U ? snapshot.items[0].ssid_len : 0U, missing);
    }
    c5_conn_state_t next = c5_conn_join_candidates(&snapshot, candidates, count, current);
    memset(&snapshot, 0, sizeof(snapshot));
    return next;
}

static c5_conn_state_t c5_conn_connected(wifi_saved_network_t *current)
{
    if (__atomic_load_n(&s_station_connected, __ATOMIC_ACQUIRE)) {
        uint32_t bits = c5_conn_wait(C5_NOTIFY_DISCONNECTED | C5_NOTIFY_RESCAN |
                                     C5_NOTIFY_SCAN_REQUEST | C5_NOTIFY_COMMANDS,
                                     UINT32_MAX);
        if ((bits & C5_NOTIFY_COMMANDS) != 0U) {
            return C5_CONN_CONNECTED;
        }
        if ((bits & C5_NOTIFY_RESCAN) != 0U) {
            /* The current network was removed. Deliver the RPC
             * acknowledgement before leaving it. */
            vTaskDelay(pdMS_TO_TICKS(1000));
            c5_conn_disconnect();
            return C5_CONN_SCAN;
        }
        if ((bits & C5_NOTIFY_SCAN_REQUEST) != 0U) {
            c5_conn_clear(C5_NOTIFY_SCAN_REQUEST);
            wifi_network_list_t snapshot;
            wifi_network_candidate_t candidates[WIFI_NETWORK_LIST_MAX];
            (void)c5_scan(&snapshot, candidates, NULL, NULL, false);
            memset(&snapshot, 0, sizeof(snapshot));
            return C5_CONN_CONNECTED;
        }
        c5_conn_clear(C5_NOTIFY_DISCONNECTED);
        if (__atomic_load_n(&s_station_connected, __ATOMIC_ACQUIRE)) {
            return C5_CONN_CONNECTED;
        }
    }

    /* The link dropped: try the same network a few times before a full scan. */
    static const uint32_t backoff_ms[] = {2000U, 5000U, 10000U};
    for (size_t i = 0; i < sizeof(backoff_ms) / sizeof(backoff_ms[0]); ++i) {
        if (c5_conn_wait(C5_NOTIFY_COMMANDS | C5_NOTIFY_RESCAN, backoff_ms[i]) != 0U) {
            return C5_CONN_SCAN;
        }
        if (c5_join(current, 0U, NULL) == C5_JOIN_OK) {
            return C5_CONN_CONNECTED;
        }
    }
    return C5_CONN_SCAN;
}

static bool c5_find_saved(const char *ssid, wifi_saved_network_t *out)
{
    xSemaphoreTake(s_networks_lock, portMAX_DELAY);
    int index = wifi_network_list_find(&s_networks, (const uint8_t *)ssid, strlen(ssid));
    if (index >= 0) {
        *out = s_networks.items[index];
    }
    xSemaphoreGive(s_networks_lock);
    return index >= 0;
}

static bool c5_have_saved_networks(void)
{
    xSemaphoreTake(s_networks_lock, portMAX_DELAY);
    bool have = s_networks.count > 0U;
    xSemaphoreGive(s_networks_lock);
    return have;
}

/* Switch the radio to the Auto band that menu setup selected, if it has not
 * yet. The setup service stops for it; returns false when it did, so the
 * caller starts it again. */
static bool c5_menu_setup_apply_band(void)
{
    if (!s_menu_setup_band_pending) return true;
    s_menu_setup_band_pending = false;
    bool was_running = __atomic_load_n(&s_provisioning_active, __ATOMIC_ACQUIRE);
    /* A band change restarts the station under the setup service. */
    c5_prov_stop();
    esp_err_t error = c5_apply_band();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi setup: both bands unavailable: %s", esp_err_to_name(error));
    }
    return !was_running;
}

/* The radio may switch bands for menu setup now: no phone is connected, the
 * setup service holds no credentials it is joining with (from CRED_RECV until
 * CRED_SUCCESS or CRED_FAIL), no PROV_END waits and no network is up, so no
 * setup session or join is cut. Connection task only. */
static bool c5_menu_setup_band_can_switch(void)
{
    c5_conn_collect(0);
    return s_menu_setup_band_pending &&
           !__atomic_load_n(&s_ble_central_connected, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&s_pending_station_config_valid, __ATOMIC_ACQUIRE) &&
           (s_conn_pending & C5_NOTIFY_PROV_END) == 0U &&
           !__atomic_load_n(&s_station_connected, __ATOMIC_ACQUIRE);
}

/* Start BLE setup for the menu and select both bands, so the phone can set
 * up a network on either. From another state (`in_setup` false) the device
 * leaves its network first. During automatic setup the service, its BLE
 * session and any link stay: that link is the network a phone just sent, and
 * its PROV_END ends menu setup as a success; the radio switches bands later,
 * once c5_menu_setup_band_can_switch. Returns false when the service was
 * stopped for the band switch and must start again. */
static bool c5_menu_setup_begin(bool in_setup)
{
    /* Again, in case the previous menu setup ended after this start. */
    __atomic_store_n(&s_menu_setup, true, __ATOMIC_RELEASE);
    jetkvm_board_network_set_wifi_setup_menu(true, c5_have_saved_networks());
    s_menu_setup_started_at = xTaskGetTickCount();
    ESP_LOGI(TAG, "Wi-Fi setup started from the menu");
    portENTER_CRITICAL(&s_settings_lock);
    if (!s_menu_setup_band_changed && s_wifi_settings.band != JETKVM_WIFI_BAND_AUTO) {
        s_menu_setup_band = s_wifi_settings.band;
        s_menu_setup_band_changed = true;
        s_menu_setup_band_pending = true;
        s_wifi_settings.band = JETKVM_WIFI_BAND_AUTO;
    }
    portEXIT_CRITICAL(&s_settings_lock);
    if (!in_setup && __atomic_load_n(&s_station_connected, __ATOMIC_ACQUIRE)) {
        c5_conn_disconnect();
    }
    if (in_setup && !c5_menu_setup_band_can_switch()) return true;
    return c5_menu_setup_apply_band();
}

/* End menu setup. After a new network (`joined`) the band stays Auto; else
 * the previous band returns. The caller has stopped the setup service. */
static void c5_menu_setup_end(bool joined)
{
    __atomic_store_n(&s_menu_setup, false, __ATOMIC_RELEASE);
    jetkvm_board_network_set_wifi_setup_menu(false, false);
    s_menu_setup_band_pending = false;
    if (!s_menu_setup_band_changed) return;
    s_menu_setup_band_changed = false;
    esp_err_t error;
    if (joined) {
        /* The radio keeps the new link; a band switch put off until now
         * takes effect at its next start. */
        error = c5_save_band(JETKVM_WIFI_BAND_AUTO);
    } else {
        portENTER_CRITICAL(&s_settings_lock);
        s_wifi_settings.band = s_menu_setup_band;
        portEXIT_CRITICAL(&s_settings_lock);
        error = c5_apply_band();
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi band after setup: %s", esp_err_to_name(error));
    }
}

/* How long menu setup has had no phone connected. */
static TickType_t c5_menu_setup_idle(void)
{
    if (__atomic_load_n(&s_ble_central_connected, __ATOMIC_ACQUIRE)) return 0;
    TickType_t now = xTaskGetTickCount();
    TickType_t idle = now - s_menu_setup_started_at;
    TickType_t since_phone = now - __atomic_load_n(&s_ble_central_left_at, __ATOMIC_ACQUIRE);
    return since_phone < idle ? since_phone : idle;
}

static c5_conn_state_t c5_conn_offline(wifi_saved_network_t *current)
{
    if (!__atomic_load_n(&s_station_connected, __ATOMIC_ACQUIRE)) {
        jetkvm_board_network_set_wifi_joining(NULL);
    }
    bool setup_running = true;
    if (!__atomic_load_n(&s_provisioning_active, __ATOMIC_ACQUIRE)) {
        esp_err_t error = c5_prov_start();
        if (error != ESP_OK) {
            ESP_LOGE(TAG, "BLE Wi-Fi setup unavailable: %s", esp_err_to_name(error));
            setup_running = false;
        }
    }
    const TickType_t menu_limit = pdMS_TO_TICKS(C5_MENU_SETUP_IDLE_MS);
    /* After a failed start, the manager's deinit may still post PROV_END;
     * wait the full interval before trying again. */
    uint32_t wake = C5_NOTIFY_RESCAN | C5_NOTIFY_COMMANDS | C5_NOTIFY_SETUP_STOP;
    if (setup_running) wake |= C5_NOTIFY_PROV_END | C5_NOTIFY_POP_ROTATE;
    uint32_t bits;
    for (;;) {
        /* The band switch that menu setup put off while a phone was
         * connected or a network was up. */
        if (setup_running && c5_menu_setup_band_can_switch() && !c5_menu_setup_apply_band()) {
            return C5_CONN_OFFLINE;
        }
        uint32_t wait_ms = C5_OFFLINE_RESCAN_MS;
        if (__atomic_load_n(&s_menu_setup, __ATOMIC_ACQUIRE)) {
            TickType_t idle = c5_menu_setup_idle();
            TickType_t left = idle < menu_limit ? menu_limit - idle : 0U;
            if (pdTICKS_TO_MS(left) < wait_ms) wait_ms = pdTICKS_TO_MS(left);
        }
        bits = c5_conn_wait(wake, wait_ms);
        /* Left over from a service start that failed. */
        if (!setup_running) c5_conn_clear(C5_NOTIFY_PROV_END | C5_NOTIFY_POP_ROTATE);
        /* Offline, menu setup starts here, not in the task loop: leaving
         * this wait would lose a PROV_END that is on its way (the next pass
         * restarts the service and clears it). */
        if ((bits & C5_NOTIFY_SETUP_START) == 0U ||
            (bits & (C5_NOTIFY_BAND | C5_NOTIFY_CONNECT_NOW)) != 0U) {
            break;
        }
        c5_conn_clear(C5_NOTIFY_SETUP_START);
        bits &= ~C5_NOTIFY_SETUP_START;
        if (!c5_menu_setup_begin(setup_running) || !setup_running) return C5_CONN_OFFLINE;
        if (bits != 0U) break;
    }
    const bool menu = __atomic_load_n(&s_menu_setup, __ATOMIC_ACQUIRE);
    if ((bits & C5_NOTIFY_COMMANDS) != 0U) {
        /* A band change or a connect-now needs the radio. */
        if ((bits & (C5_NOTIFY_BAND | C5_NOTIFY_CONNECT_NOW)) != 0U) c5_prov_stop();
        return C5_CONN_OFFLINE;
    }
    if ((bits & C5_NOTIFY_SETUP_STOP) != 0U) {
        c5_conn_clear(C5_NOTIFY_SETUP_STOP);
        if (!menu || __atomic_load_n(&s_menu_setup_stop_generation, __ATOMIC_ACQUIRE) !=
                         __atomic_load_n(&s_menu_setup_generation, __ATOMIC_ACQUIRE)) {
            return C5_CONN_OFFLINE;
        }
        ESP_LOGI(TAG, "Wi-Fi setup ended from the menu");
        c5_prov_stop();
        c5_menu_setup_end(false);
        return C5_CONN_SCAN;
    }
    if ((bits & C5_NOTIFY_PROV_END) != 0U) {
        c5_conn_clear(C5_NOTIFY_PROV_END);
        char ssid[sizeof(s_current_ssid)];
        portENTER_CRITICAL(&s_settings_lock);
        memcpy(ssid, s_current_ssid, sizeof(ssid));
        portEXIT_CRITICAL(&s_settings_lock);
        /* A phone set up a network; the provisioning manager joined it. */
        if (__atomic_load_n(&s_station_connected, __ATOMIC_ACQUIRE)) {
            if (menu) c5_menu_setup_end(true);
            if (c5_find_saved(ssid, current)) return C5_CONN_CONNECTED;
            return C5_CONN_SCAN;
        }
        /* Menu setup starts the service again instead of joining a saved
         * network. */
        return menu ? C5_CONN_OFFLINE : C5_CONN_SCAN;
    }
    if ((bits & C5_NOTIFY_POP_ROTATE) != 0U) {
        /* Three wrong codes: restart setup with a new code. */
        c5_conn_clear(C5_NOTIFY_POP_ROTATE);
        c5_pop_retire();
        c5_prov_stop();
        return C5_CONN_OFFLINE;
    }
    c5_conn_clear(C5_NOTIFY_RESCAN | C5_NOTIFY_SCAN_REQUEST);
    /* A menu setup start that came after the wait: take it in the next wait
     * instead of timing out or rescanning for saved networks. */
    if ((s_conn_pending & C5_NOTIFY_SETUP_START) != 0U) return C5_CONN_OFFLINE;

    if (menu) {
        /* Menu setup joins no saved network until it ends. */
        if (c5_menu_setup_idle() < menu_limit) return C5_CONN_OFFLINE;
        ESP_LOGI(TAG, "Wi-Fi setup from the menu ended: no phone for %u minutes",
                 (unsigned)(C5_MENU_SETUP_IDLE_MS / 60000U));
        c5_prov_stop();
        c5_menu_setup_end(false);
        return C5_CONN_SCAN;
    }

    /* Look for a saved network again, but not while a phone is connected
     * (a scan pauses the BLE link) and not after a phone set up a network
     * that the provisioning manager has joined. */
    xSemaphoreTake(s_networks_lock, portMAX_DELAY);
    bool have_networks = s_networks.count > 0U;
    s_scan_cache.scanning = false;
    xSemaphoreGive(s_networks_lock);
    if (!have_networks || __atomic_load_n(&s_ble_central_connected, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&s_station_connected, __ATOMIC_ACQUIRE)) {
        return C5_CONN_OFFLINE;
    }
    wifi_network_list_t snapshot;
    wifi_network_candidate_t candidates[WIFI_NETWORK_LIST_MAX];
    size_t count = c5_scan(&snapshot, candidates, NULL, NULL, true);
    /* Keep BLE setup running rather than stop it for a network that just
     * rejected its saved password; try that one again later. */
    size_t kept = 0;
    for (size_t i = 0; i < count; ++i) {
        const wifi_saved_network_t *network = &snapshot.items[candidates[i].index];
        if (c5_auth_failed_recently(network->ssid, network->ssid_len)) {
            ESP_LOGI(TAG, "Wi-Fi %.*s: skipped, its password failed recently",
                     network->ssid_len, (const char *)network->ssid);
            continue;
        }
        candidates[kept++] = candidates[i];
    }
    count = kept;
    if (count == 0U) {
        memset(&snapshot, 0, sizeof(snapshot));
        return C5_CONN_OFFLINE;
    }
    /* Join from this scan rather than scanning again. */
    c5_prov_stop();
    jetkvm_board_network_set_wifi_joining("");
    c5_conn_state_t next = c5_conn_join_candidates(&snapshot, candidates, count, current);
    memset(&snapshot, 0, sizeof(snapshot));
    return next;
}

static c5_conn_state_t c5_conn_connect_now(wifi_saved_network_t *current)
{
    /* Deliver the RPC acknowledgement before the scan interrupts the current
     * network. If its packet is lost, the retransmission comes after about
     * 1 s; 3 s lets it arrive before the scan starts. */
    vTaskDelay(pdMS_TO_TICKS(C5_CONNECT_ACK_MS));
    wifi_saved_network_t request;
    xSemaphoreTake(s_networks_lock, portMAX_DELAY);
    request = s_connect_request;
    memset(&s_connect_request, 0, sizeof(s_connect_request));
    xSemaphoreGive(s_networks_lock);

    /* Before leaving a working network for another one, check that the other
     * one is in range, so a network that is not there costs no outage. */
    char active[sizeof(s_current_ssid)];
    portENTER_CRITICAL(&s_settings_lock);
    memcpy(active, s_current_ssid, sizeof(active));
    portEXIT_CRITICAL(&s_settings_lock);
    bool switching = __atomic_load_n(&s_station_connected, __ATOMIC_ACQUIRE) &&
                     (strlen(active) != request.ssid_len ||
                      memcmp(active, request.ssid, request.ssid_len) != 0);
    if (switching) {
        wifi_network_list_t snapshot;
        wifi_network_candidate_t candidates[WIFI_NETWORK_LIST_MAX];
        bool visible = false;
        /* The target may not be saved yet (it is saved after it connects),
         * so look for it among all scanned APs. */
        (void)c5_scan(&snapshot, candidates, &request, &visible, false);
        memset(&snapshot, 0, sizeof(snapshot));
        if (!visible) {
            ESP_LOGW(TAG, "Wi-Fi %.*s is not in range; staying on %s",
                     request.ssid_len, (const char *)request.ssid, active);
            c5_set_connect_result("Network not in range");
            memset(&request, 0, sizeof(request));
            return C5_CONN_CONNECTED;
        }
    }

    /* Tell the web UI the device is about to leave this network, and give
     * it time to close its session cleanly first. */
    portENTER_CRITICAL(&s_settings_lock);
    s_wifi_settings.switching = true;
    portEXIT_CRITICAL(&s_settings_lock);
    vTaskDelay(pdMS_TO_TICKS(C5_SWITCH_NOTICE_MS));

    c5_conn_disconnect();
    wifi_join_failure_t failure;
    c5_join_result_t result = c5_join(&request, 0U, &failure);
    char failure_text[WIFI_JOIN_FAILURE_TEXT_CAPACITY];
    const char *error = NULL;
    if (result == C5_JOIN_OK) {
        esp_err_t save_error;
        wifi_network_list_result_t stored = c5_store_network(
            request.ssid, request.ssid_len, request.password, request.password_len,
            false, &save_error);
        if (stored == WIFI_NETWORK_LIST_FULL) {
            error = "Connected, but the saved network list is full";
        } else if (stored != WIFI_NETWORK_LIST_OK || save_error != ESP_OK) {
            error = "Connected, but the network could not be saved";
        }
        *current = request;
    } else {
        (void)wifi_join_failure_text(failure, failure_text, sizeof(failure_text));
        error = failure_text;
    }
    c5_set_connect_result(error);
    memset(&request, 0, sizeof(request));
    return result == C5_JOIN_OK ? C5_CONN_CONNECTED : C5_CONN_SCAN;
}

static void c5_conn_task(void *argument)
{
    (void)argument;
    wifi_saved_network_t current = {0};
    c5_conn_state_t state = C5_CONN_SCAN;
    /* A band request whose first search on the new band is still to come. */
    uint32_t band_search = 0;
    for (;;) {
        c5_conn_collect(0);
        if ((s_conn_pending & C5_NOTIFY_BAND) != 0U) {
            c5_conn_clear(C5_NOTIFY_BAND);
            uint32_t request = __atomic_load_n(&s_band_requested, __ATOMIC_ACQUIRE);
            c5_prov_stop();
            esp_err_t error = c5_conn_restart_radio();
            if (error != ESP_OK) {
                /* The band worker restores the previous band. */
                ESP_LOGE(TAG, "Wi-Fi band restart failed: %s", esp_err_to_name(error));
                __atomic_store_n(&s_band_searched, request, __ATOMIC_RELEASE);
                band_search = 0;
            } else {
                __atomic_store_n(&s_band_applied, request, __ATOMIC_RELEASE);
                band_search = request;
            }
            state = C5_CONN_SCAN;
        }
        if ((s_conn_pending & C5_NOTIFY_CONNECT_NOW) != 0U) {
            c5_conn_clear(C5_NOTIFY_CONNECT_NOW);
            state = c5_conn_connect_now(&current);
        }
        /* Offline, c5_conn_offline takes the start in its wait. */
        if ((s_conn_pending & C5_NOTIFY_SETUP_START) != 0U && state != C5_CONN_OFFLINE) {
            c5_conn_clear(C5_NOTIFY_SETUP_START);
            (void)c5_menu_setup_begin(false);
            state = C5_CONN_OFFLINE;
        }
        switch (state) {
        case C5_CONN_SCAN:
            state = c5_conn_scan_and_join(&current);
            if (band_search != 0U) {
                __atomic_store_n(&s_band_searched, band_search, __ATOMIC_RELEASE);
                band_search = 0;
            }
            break;
        case C5_CONN_CONNECTED:
            state = c5_conn_connected(&current);
            break;
        case C5_CONN_OFFLINE:
            state = c5_conn_offline(&current);
            break;
        }
    }
}

/* The station takes the factory MAC of eFuse unless the MAC address setting
 * selects the C5's Espressif MAC. Call in station mode before esp_wifi_start.
 * Any failure keeps the Espressif MAC. */
static void c5_apply_factory_mac(void)
{
    uint8_t espressif[6], mac[6];
    s_factory_mac_in_use = false;
    esp_err_t error = esp_wifi_get_mac(WIFI_IF_STA, espressif);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "No station MAC (%s); the MAC stays as it is", esp_err_to_name(error));
        return;
    }
    if (!jetkvm_mac_address_for_interface(espressif, mac)) return;
    error = esp_wifi_set_mac(WIFI_IF_STA, mac);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Setting the factory MAC on the station failed (%s); Wi-Fi uses the Espressif MAC",
                 esp_err_to_name(error));
        return;
    }
    memcpy(s_factory_mac, mac, sizeof(s_factory_mac));
    s_factory_mac_in_use = true;
}

static esp_err_t c5_wifi_start_impl(void)
{
    esp_err_t error;

    if (s_c5_wifi_ready) {
        return ESP_OK;
    }
    if (s_networks_lock == NULL) {
        s_networks_lock = xSemaphoreCreateMutex();
        if (s_networks_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    nvs_handle_t band_nvs;
    uint8_t saved_band = 0;
    if (nvs_open(C5_PROV_NVS_NAMESPACE, NVS_READONLY, &band_nvs) == ESP_OK) {
        (void)nvs_get_u8(band_nvs, "band", &saved_band);
        nvs_close(band_nvs);
    }
    portENTER_CRITICAL(&s_settings_lock);
    s_wifi_settings.band = saved_band <= JETKVM_WIFI_BAND_5G ? saved_band : JETKVM_WIFI_BAND_AUTO;
    portEXIT_CRITICAL(&s_settings_lock);

    error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(error));
        return error;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop init failed: %s", esp_err_to_name(error));
        return error;
    }

    error = c5_provision_hosted_mcu();
    if (error != ESP_OK) {
        ESP_LOGW(TAG,
                 "C5 BootROM provisioning unavailable; trying normal Hosted boot");
    }

    ESP_LOGI(TAG,
             "Starting ESP-Hosted SDIO: CMD=%d CLK=%d D0=%d D1=%d D2=%d D3=%d",
             JETKVM_C5_PIN_SDIO_CMD, JETKVM_C5_PIN_SDIO_CLK,
             JETKVM_C5_PIN_SDIO_D0, JETKVM_C5_PIN_SDIO_D1,
             JETKVM_C5_PIN_SDIO_D2, JETKVM_C5_PIN_SDIO_D3);
    error = esp_hosted_init();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_init failed: %s", esp_err_to_name(error));
        goto cleanup;
    }
    s_hosted_initialized = true;

    error = esp_hosted_connect_to_slave();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "C5 did not answer ESP-Hosted: %s",
                 esp_err_to_name(error));
        goto cleanup;
    }

    esp_hosted_coprocessor_fwver_t firmware_version = {0};
    error = esp_hosted_get_coprocessor_fwversion(&firmware_version);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "C5 firmware version query failed: %s",
                 esp_err_to_name(error));
        goto cleanup;
    }
    ESP_LOGI(TAG, "C5 ESP-Hosted firmware %lu.%lu.%lu",
             (unsigned long)firmware_version.major1,
             (unsigned long)firmware_version.minor1,
             (unsigned long)firmware_version.patch1);

    s_station = esp_netif_create_default_wifi_sta();
    if (s_station == NULL) {
        error = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    /* IDF's connect action preserves the STOPPED DHCP state for static mode. */
    error = jetkvm_network_settings_init(s_station);
    if (error != ESP_OK) goto cleanup;

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    /* The C5 takes its TX buffers from this config. esp_wifi_remote derives
     * them from the P4's Kconfig, where SPIRAM_TRY_ALLOCATE_WIFI_LWIP forces
     * 16 static buffers; a 1080p keyframe (~128 packets) then overflows the
     * pool after 16 packets. Use the C5 slave's own default, 32 dynamic
     * buffers: the C5 has no PSRAM, and 32 static ones leave it ~29 KB. */
    wifi_config.tx_buf_type = 1;
    wifi_config.dynamic_tx_buf_num = 32;
    error = esp_wifi_init(&wifi_config);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(error));
        goto cleanup;
    }
    s_wifi_initialized = true;

    error = c5_clear_legacy_credentials_once();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "C5 credential cleanup failed: %s",
                 esp_err_to_name(error));
        goto cleanup;
    }

    /* A local wipe removes c5_clean. Complete the durable reset only after
     * that radio erase succeeds, before accepting any new credentials. */
    error = jetkvm_factory_reset_finish(ESP_OK);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Factory reset completion failed: %s", esp_err_to_name(error));
        goto cleanup;
    }

    /* KVM traffic needs prompt downlink delivery after idle periods. */
    error = esp_wifi_set_ps(WIFI_PS_NONE);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Disabling Wi-Fi power save failed: %s",
                 esp_err_to_name(error));
        goto cleanup;
    }
    ESP_LOGI(TAG, "Wi-Fi power save disabled");

    error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_storage failed: %s",
                 esp_err_to_name(error));
        goto cleanup;
    }

    error = c5_register_network_events();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi event registration failed: %s",
                 esp_err_to_name(error));
        goto cleanup;
    }

    /* An 802.11ax association on 5 GHz never completes the 4-way handshake
     * with an iPhone hotspot in WPA2/WPA3 transition mode; 802.11ac does.
     * esp_wifi_set_protocols needs the station interface, so select station
     * mode first; both start paths below use station mode anyway. */
    error = esp_wifi_set_mode(WIFI_MODE_STA);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Station mode select failed: %s", esp_err_to_name(error));
        goto cleanup;
    }
    wifi_protocols_t protocols = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N |
                  WIFI_PROTOCOL_11AX,
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC,
    };
    error = esp_wifi_set_protocols(WIFI_IF_STA, &protocols);
    if (error != ESP_OK) {
        /* Provisioning and 2.4 GHz joins still work; only the 5 GHz
         * transition-mode join depends on the mask. */
        ESP_LOGW(TAG, "5 GHz protocol mask failed: %s", esp_err_to_name(error));
    } else {
        ESP_LOGI(TAG, "5 GHz protocols: 11a/n/ac");
    }

    error = c5_load_networks();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Saved Wi-Fi network load failed: %s", esp_err_to_name(error));
        goto cleanup;
    }

    /* Before the start: STA_START gives the P4's netif the station MAC. */
    c5_apply_factory_mac();

    error = esp_wifi_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(error));
        goto cleanup;
    }
    s_wifi_started = true;
    error = c5_apply_band();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Applying saved Wi-Fi band failed: %s", esp_err_to_name(error));
        goto cleanup;
    }

    /* The task scans, joins the strongest saved network and starts BLE
     * setup when none connects. An empty list goes straight to BLE setup.
     * Its stack is in LP (RTC) RAM, which is slow but stays usable while an
     * NVS write disables the cache: the task runs only to join or set up,
     * and ESP-Hosted needs the internal DMA RAM for its SDIO buffers. */
    TaskHandle_t conn_task = NULL;
    if (xTaskCreateWithCaps(c5_conn_task, "c5_wifi_conn", C5_CONN_TASK_STACK_BYTES, NULL,
                            tskIDLE_PRIORITY + 2, &conn_task,
                            MALLOC_CAP_RTCRAM | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        error = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    __atomic_store_n(&s_conn_task, conn_task, __ATOMIC_RELEASE);

    s_c5_wifi_ready = true;
    ESP_LOGI(TAG, "C5 Wi-Fi ready; ESP-Hosted remains active on SDMMC slot 1");
    return ESP_OK;

cleanup:
    if (s_provisioning_manager_initialized) {
        (void)network_prov_mgr_deinit();
        s_provisioning_manager_initialized = false;
    }
    s_provisioning_active = false;
    c5_stop_bt_controller();
    if (s_wifi_started) {
        (void)esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_wifi_initialized) {
        (void)esp_wifi_deinit();
        s_wifi_initialized = false;
    }
    if (s_station != NULL) {
        esp_netif_destroy_default_wifi(s_station);
        s_station = NULL;
    }
    if (s_hosted_initialized) {
        (void)esp_hosted_deinit();
        s_hosted_initialized = false;
    }
    return error;
}

static void c5_worker_task(void *argument)
{
    c5_task_context_t *context = argument;
    context->result = context->worker();
    xTaskNotifyGive(context->waiter);
    vTaskDelete(NULL);
}

static esp_err_t c5_run_worker(esp_err_t (*worker)(void))
{
    c5_task_context_t context = {
        .waiter = xTaskGetCurrentTaskHandle(),
        .worker = worker,
        .result = ESP_FAIL,
    };

    BaseType_t created = xTaskCreate(c5_worker_task, "c5_wifi_worker",
                                     C5_START_TASK_STACK_BYTES, &context,
                                     tskIDLE_PRIORITY + 1, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return context.result;
}

esp_err_t jetkvm_c5_wifi_start(void)
{
    return c5_run_worker(c5_wifi_start_impl);
}

bool jetkvm_c5_wifi_scan_networks(bool refresh, jetkvm_wifi_scan_t *scan, void *ctx)
{
    (void)ctx;
    if (scan == NULL) return false;
    *scan = (jetkvm_wifi_scan_t){0};
    if (!s_c5_wifi_ready) return true;
    if (xSemaphoreTake(s_networks_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    /* No scan during BLE setup: the web UI cannot reach the device then. */
    bool start = refresh && !__atomic_load_n(&s_provisioning_active, __ATOMIC_ACQUIRE);
    if (start) s_scan_cache.scanning = true;
    *scan = s_scan_cache;
    xSemaphoreGive(s_networks_lock);
    if (start) c5_conn_notify(C5_NOTIFY_SCAN_REQUEST);
    return true;
}

static const char *c5_begin_connect_locked(const wifi_saved_network_t *network);

const char *jetkvm_c5_wifi_save_network(const jetkvm_wifi_network_request_t *request,
                                        bool *connecting, void *ctx)
{
    (void)ctx;
    if (connecting != NULL) *connecting = false;
    if (request == NULL) return "Invalid network";
    if (!s_c5_wifi_ready) return "Wi-Fi is not ready";
    const uint8_t *ssid = (const uint8_t *)request->ssid;
    const uint8_t *password = (const uint8_t *)request->password;
    size_t ssid_len = strnlen(request->ssid, sizeof(request->ssid));
    size_t password_len = strnlen(request->password, sizeof(request->password));
    if (ssid_len == 0U || ssid_len > WIFI_NETWORK_SSID_MAX) return "SSID must be 1 to 32 bytes";
    if (!wifi_network_password_valid(password, password_len))
        return "Password must be 8 to 63 characters or 64 hex digits";

    /* A new password for the network the station uses (or is retrying) is
     * always tested by joining with it, so a typo is not stored untested
     * while the old password keeps working until the next reboot. */
    portENTER_CRITICAL(&s_settings_lock);
    bool current = strcmp(s_current_ssid, request->ssid) == 0;
    portEXIT_CRITICAL(&s_settings_lock);
    bool connect = request->connect || current;

    if (!connect) {
        esp_err_t save_error;
        wifi_network_list_result_t result = c5_store_network(
            ssid, ssid_len, password, password_len, false, &save_error);
        if (result == WIFI_NETWORK_LIST_FULL) return "The saved network list is full";
        if (result != WIFI_NETWORK_LIST_OK) return "Invalid network";
        return save_error == ESP_OK ? NULL : "The network could not be saved";
    }

    /* Connect now: the connection task joins it and saves it on success. */
    const char *error = NULL;
    xSemaphoreTake(s_networks_lock, portMAX_DELAY);
    if (wifi_network_list_find(&s_networks, ssid, ssid_len) < 0 &&
        s_networks.count >= WIFI_NETWORK_LIST_MAX) {
        error = "The saved network list is full";
    }
    if (error == NULL) {
        wifi_saved_network_t request = {.ssid_len = (uint8_t)ssid_len,
                                        .password_len = (uint8_t)password_len};
        memcpy(request.ssid, ssid, ssid_len);
        memcpy(request.password, password, password_len);
        error = c5_begin_connect_locked(&request);
        memset(&request, 0, sizeof(request));
    }
    xSemaphoreGive(s_networks_lock);
    if (error == NULL) {
        if (connecting != NULL) *connecting = true;
        c5_conn_notify(C5_NOTIFY_CONNECT_NOW);
    }
    return error;
}

/* Reserve the connect-now slot; the caller holds s_networks_lock. */
static const char *c5_begin_connect_locked(const wifi_saved_network_t *network)
{
    const char *error = NULL;
    portENTER_CRITICAL(&s_settings_lock);
    if (s_wifi_settings.applying) {
        error = "Wi-Fi settings are still being applied";
    } else if (s_menu_setup) {
        error = "Wi-Fi setup is running";
    } else {
        s_wifi_settings.applying = true;
        s_wifi_settings.connect_error[0] = '\0';
    }
    portEXIT_CRITICAL(&s_settings_lock);
    if (error == NULL) s_connect_request = *network;
    return error;
}

const char *jetkvm_c5_wifi_connect_network(const char *ssid, void *ctx)
{
    (void)ctx;
    if (ssid == NULL) return "Invalid network";
    if (!s_c5_wifi_ready) return "Wi-Fi is not ready";
    size_t ssid_len = strnlen(ssid, WIFI_NETWORK_SSID_MAX + 1U);
    portENTER_CRITICAL(&s_settings_lock);
    bool current = strcmp(s_current_ssid, ssid) == 0;
    portEXIT_CRITICAL(&s_settings_lock);
    if (current && __atomic_load_n(&s_station_connected, __ATOMIC_ACQUIRE)) {
        return "The device already uses this network";
    }
    const char *error;
    xSemaphoreTake(s_networks_lock, portMAX_DELAY);
    int index = wifi_network_list_find(&s_networks, (const uint8_t *)ssid, ssid_len);
    error = index < 0 ? "This network is not saved" :
            c5_begin_connect_locked(&s_networks.items[index]);
    xSemaphoreGive(s_networks_lock);
    if (error == NULL) c5_conn_notify(C5_NOTIFY_CONNECT_NOW);
    return error;
}

/* Remove a saved network. Settings > Network keeps the last one, so an
 * online device cannot remove the network it is reached through; BLE setup
 * may remove it. */
static const char *c5_remove_network(const char *ssid, bool keep_last)
{
    if (ssid == NULL) return "Invalid network";
    if (!s_c5_wifi_ready) return "Wi-Fi is not ready";
    size_t ssid_len = strnlen(ssid, WIFI_NETWORK_SSID_MAX + 1U);
    wifi_network_list_t list;
    xSemaphoreTake(s_networks_lock, portMAX_DELAY);
    list = s_networks;
    wifi_network_list_result_t result =
        keep_last ? wifi_network_list_remove(&list, (const uint8_t *)ssid, ssid_len)
                  : wifi_network_list_forget(&list, (const uint8_t *)ssid, ssid_len);
    esp_err_t save_error = ESP_OK;
    if (result == WIFI_NETWORK_LIST_OK) {
        save_error = c5_save_networks_locked(&list);
        if (save_error == ESP_OK) s_networks = list;
    }
    xSemaphoreGive(s_networks_lock);
    memset(&list, 0, sizeof(list));
    if (result == WIFI_NETWORK_LIST_NOT_FOUND) return "This network is not saved";
    if (result == WIFI_NETWORK_LIST_LAST_NETWORK) return "The last saved network cannot be removed";
    if (result != WIFI_NETWORK_LIST_OK) return "Invalid network";
    if (save_error != ESP_OK) return "The network could not be removed";

    portENTER_CRITICAL(&s_settings_lock);
    bool current = strcmp(s_current_ssid, ssid) == 0;
    portEXIT_CRITICAL(&s_settings_lock);
    /* Leave the removed network and join the strongest remaining one. */
    if (current) c5_conn_notify(C5_NOTIFY_RESCAN);
    return NULL;
}

const char *jetkvm_c5_wifi_remove_network(const char *ssid, void *ctx)
{
    (void)ctx;
    return c5_remove_network(ssid, true);
}

const char *jetkvm_c5_wifi_start_setup(void)
{
    if (!s_c5_wifi_ready) return "Wi-Fi is not ready";
    const char *error = NULL;
    portENTER_CRITICAL(&s_settings_lock);
    if (s_wifi_settings.applying) {
        error = "Wi-Fi settings are still being applied";
    } else {
        __atomic_store_n(&s_menu_setup, true, __ATOMIC_RELEASE);
        __atomic_add_fetch(&s_menu_setup_generation, 1U, __ATOMIC_ACQ_REL);
    }
    portEXIT_CRITICAL(&s_settings_lock);
    if (error != NULL) return error;
    /* Before this returns, so the screen shows setup, not the home screen of
     * the network the device is about to leave. */
    jetkvm_board_network_set_wifi_setup_menu(true, c5_have_saved_networks());
    c5_conn_notify(C5_NOTIFY_SETUP_START);
    return NULL;
}

const char *jetkvm_c5_wifi_stop_setup(void)
{
    if (!__atomic_load_n(&s_menu_setup, __ATOMIC_ACQUIRE)) {
        return "Wi-Fi setup did not start from the menu";
    }
    /* With no saved network, setup would start again at once. */
    if (!c5_have_saved_networks()) return "No saved Wi-Fi network";
    __atomic_store_n(&s_menu_setup_stop_generation,
                     __atomic_load_n(&s_menu_setup_generation, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    c5_conn_notify(C5_NOTIFY_SETUP_STOP);
    return NULL;
}
#else
bool jetkvm_c5_wifi_get_settings(jetkvm_wifi_settings_t *settings, void *ctx)
{
    (void)ctx;
    if (!settings) return false;
    *settings = (jetkvm_wifi_settings_t){0};
    return true;
}
const char *jetkvm_c5_wifi_set_band(jetkvm_wifi_band_t band, void *ctx)
{
    (void)band; (void)ctx;
    return "This board has no Wi-Fi";
}
esp_err_t jetkvm_c5_wifi_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool jetkvm_c5_wifi_scan_networks(bool refresh, jetkvm_wifi_scan_t *scan, void *ctx)
{
    (void)refresh; (void)ctx;
    if (!scan) return false;
    *scan = (jetkvm_wifi_scan_t){0};
    return true;
}
const char *jetkvm_c5_wifi_save_network(const jetkvm_wifi_network_request_t *request,
                                        bool *connecting, void *ctx)
{
    (void)request; (void)connecting; (void)ctx;
    return "This board has no Wi-Fi";
}
const char *jetkvm_c5_wifi_remove_network(const char *ssid, void *ctx)
{
    (void)ssid; (void)ctx;
    return "This board has no Wi-Fi";
}
const char *jetkvm_c5_wifi_connect_network(const char *ssid, void *ctx)
{
    (void)ssid; (void)ctx;
    return "This board has no Wi-Fi";
}
const char *jetkvm_c5_wifi_start_setup(void)
{
    return "This board has no Wi-Fi";
}
const char *jetkvm_c5_wifi_stop_setup(void)
{
    return "This board has no Wi-Fi";
}
#endif

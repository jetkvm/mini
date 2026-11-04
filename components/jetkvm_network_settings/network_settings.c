#include "jetkvm_network_settings.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_netif_net_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "nvs.h"
#include "jetkvm_identity.h"
#include "jetkvm_time_sync.h"

static const char *TAG = "network_settings";
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static jetkvm_network_settings_t s_settings;
static jetkvm_network_settings_t s_previous;
static bool s_pending;
static bool s_apply_failed;
static char s_device_id[JETKVM_DEVICE_ID_CAPACITY];
/* Wi-Fi only: the network the station joins, and the network the static
 * IPv4 settings belong to (NVS jk_network/static_ssid). */
static bool s_wifi_known;
static char s_wifi_ssid[JETKVM_WIFI_SSID_CAPACITY];
static char s_static_ssid[JETKVM_WIFI_SSID_CAPACITY];
static char s_previous_static_ssid[JETKVM_WIFI_SSID_CAPACITY];

static esp_netif_t *active_netif(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    return netif ? netif : esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}
static esp_err_t save_static_ssid(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("jk_network", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = s_static_ssid[0] ? nvs_set_str(nvs, "static_ssid", s_static_ssid) :
                             nvs_erase_key(nvs, "static_ssid");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
/*
 * The binding and the settings are separate NVS keys and cannot be written
 * together. "static_pend" is set before a change writes both and erased once
 * they agree again; a boot that finds it does not trust the binding, and
 * static IPv4 then applies to no Wi-Fi network (DHCP) until a save succeeds.
 */
static bool s_binding_untrusted;
static esp_err_t set_binding_pending(bool pending)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("jk_network", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = pending ? nvs_set_u8(nvs, "static_pend", 1U) : nvs_erase_key(nvs, "static_pend");
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
static void end_binding_update(void)
{
    if (set_binding_pending(false) == ESP_OK) s_binding_untrusted = false;
    else ESP_LOGW(TAG, "Could not clear the static IPv4 update marker");
}
static bool static_applies(const jetkvm_network_settings_t *settings)
{
    if (s_binding_untrusted && s_wifi_known) return false;
    return jetkvm_network_settings_static_applies(settings, s_static_ssid,
                                                  s_wifi_known ? s_wifi_ssid : NULL);
}
static esp_err_t save(const jetkvm_network_settings_t *settings)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("jk_network", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, "config", settings, sizeof(*settings));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}
/* User servers first, then the public fallback unless the user disabled it. */
static esp_err_t apply_time_sync(const jetkvm_network_settings_t *settings)
{
    jetkvm_time_sync_config_t config = {0};
    for (size_t i = 0; i < JETKVM_TIME_SYNC_MAX_SERVERS; ++i) {
        if (settings->time_sync_custom && settings->time_sync_ntp_servers[i][0]) {
            config.servers[config.count++] = settings->time_sync_ntp_servers[i];
        }
    }
    config.fallback = !settings->time_sync_custom || !settings->time_sync_disable_fallback;
    esp_err_t err = jetkvm_time_sync_configure(&config);
    if (err != ESP_OK) ESP_LOGW(TAG, "Time sync reconfiguration failed: %s", esp_err_to_name(err));
    return err;
}
static void apply_dns(void *arg)
{
    const jetkvm_network_settings_t *settings = arg;
    for (unsigned i = 0; i < 3; ++i) {
        ip_addr_t dns;
        ip_addr_set_zero(&dns);
        if (!settings->dhcp && settings->dns[i][0]) ipaddr_aton(settings->dns[i], &dns);
        /* esp_netif_set_dns_info rejects 0.0.0.0; lwIP's setter clears a slot. */
        dns_setserver(i, &dns);
    }
}
static esp_err_t apply(esp_netif_t *netif, const jetkvm_network_settings_t *requested)
{
    if (!netif) return ESP_ERR_INVALID_STATE;
    /* Static settings saved for another Wi-Fi network do not apply here. */
    jetkvm_network_settings_t effective = *requested;
    if (!requested->dhcp && !static_applies(requested)) effective.dhcp = true;
    const jetkvm_network_settings_t *settings = &effective;
    esp_err_t err = esp_netif_set_hostname(netif, settings->hostname);
    if (err != ESP_OK) return err;
    err = esp_netif_dhcpc_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return err;
    if (settings->dhcp) {
        if (tcpip_callback_wait(apply_dns, (void *)settings) != ERR_OK) return ESP_FAIL;
        err = esp_netif_dhcpc_start(netif);
        return err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED ? ESP_OK : err;
    }
    esp_netif_ip_info_t info = {0};
    esp_netif_str_to_ip4(settings->address, &info.ip);
    esp_netif_str_to_ip4(settings->netmask, &info.netmask);
    if (settings->gateway[0]) esp_netif_str_to_ip4(settings->gateway, &info.gw);
    err = esp_netif_set_ip_info(netif, &info);
    /* Setting an IPv4 address clears lwIP main/backup DNS on this netif. */
    if (err == ESP_OK && tcpip_callback_wait(apply_dns, (void *)settings) != ERR_OK) err = ESP_FAIL;
    return err;
}
static void apply_worker(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    /* Give the RPC reply and the UI's immediate refresh time to leave. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = apply(active_netif(), &s_settings);
    s_apply_failed = err != ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Applying network settings failed: %s", esp_err_to_name(err));
        s_settings = s_previous;
        memcpy(s_static_ssid, s_previous_static_ssid, sizeof(s_static_ssid));
        bool marked = set_binding_pending(true) == ESP_OK;
        esp_err_t bound = save_static_ssid();
        if (bound != ESP_OK) ESP_LOGW(TAG, "Could not restore the static IPv4 network");
        esp_err_t persisted = save(&s_settings);
        if (marked && bound == ESP_OK && persisted == ESP_OK) end_binding_update();
        esp_err_t restored = apply(active_netif(), &s_settings);
        s_apply_failed = persisted != ESP_OK || restored != ESP_OK;
        if (s_apply_failed) ESP_LOGE(TAG, "Network rollback failed: save=%s apply=%s",
            esp_err_to_name(persisted), esp_err_to_name(restored));
    }
    if (apply_time_sync(&s_settings) != ESP_OK && !s_apply_failed) {
        /* Same outcome as the time-sync-only path in _set: the interface keeps
         * its new settings, the servers go back to the ones that were running. */
        jetkvm_network_settings_t rollback = s_previous;
        memcpy(&rollback, &s_settings, JETKVM_NETWORK_SETTINGS_LEGACY_SIZE);
        s_settings = rollback;
        s_apply_failed = save(&s_settings) != ESP_OK || apply_time_sync(&s_settings) != ESP_OK;
    }
    s_pending = false;
    s_task = NULL;
    xSemaphoreGive(s_lock);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t jetkvm_network_settings_init(esp_netif_t *netif)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
        if (jetkvm_device_id_get(s_device_id, sizeof(s_device_id)) != ESP_OK) s_device_id[0] = '\0';
        jetkvm_network_settings_defaults(&s_settings, s_device_id);
        nvs_handle_t nvs;
        if (nvs_open("jk_network", NVS_READONLY, &nvs) == ESP_OK) {
            unsigned char blob[sizeof(jetkvm_network_settings_t)];
            size_t size = sizeof(blob);
            esp_err_t err = nvs_get_blob(nvs, "config", blob, &size);
            size_t ssid_size = sizeof(s_static_ssid);
            if (nvs_get_str(nvs, "static_ssid", s_static_ssid, &ssid_size) != ESP_OK)
                s_static_ssid[0] = '\0';
            uint8_t pending = 0;
            if (nvs_get_u8(nvs, "static_pend", &pending) == ESP_OK && pending) {
                s_binding_untrusted = true;
                ESP_LOGW(TAG, "A static IPv4 update did not finish; Wi-Fi uses DHCP until the next save");
            }
            nvs_close(nvs);
            if (err != ESP_OK || !jetkvm_network_settings_from_blob(blob, size, &s_settings)) {
                jetkvm_network_settings_defaults(&s_settings, s_device_id);
                if (err != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "Invalid saved network settings; using DHCP");
            } else if (jetkvm_network_settings_migrate_hostname(&s_settings, s_device_id)) {
                ESP_LOGI(TAG, "Replacing the old default hostname esp32p4 with %s", s_settings.hostname);
            }
        }
        (void)apply_time_sync(&s_settings);
    }
    return apply(netif, &s_settings);
}
void jetkvm_network_settings_set_wifi_network(const char *ssid)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool before = static_applies(&s_settings);
    s_wifi_known = ssid != NULL;
    snprintf(s_wifi_ssid, sizeof(s_wifi_ssid), "%s", ssid ? ssid : "");
    bool after = static_applies(&s_settings);
    /* A pending change applies itself with the new network in effect. */
    if (before != after && !s_pending) {
        esp_err_t err = apply(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &s_settings);
        ESP_LOGI(TAG, "IPv4 for %s: %s%s", s_wifi_ssid, after ? "static" : "DHCP",
                 err == ESP_OK ? "" : " (apply failed)");
    }
    xSemaphoreGive(s_lock);
}
void jetkvm_network_settings_static_network(char *ssid, size_t capacity)
{
    if (!ssid || !capacity) return;
    ssid[0] = '\0';
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_settings.dhcp) snprintf(ssid, capacity, "%s", s_static_ssid);
    xSemaphoreGive(s_lock);
}
esp_err_t jetkvm_network_settings_bind_static_if_unbound(const char *ssid)
{
    if (!s_lock || !ssid || !ssid[0]) return ESP_OK;
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_settings.dhcp && !s_static_ssid[0]) {
        snprintf(s_static_ssid, sizeof(s_static_ssid), "%s", ssid);
        err = save_static_ssid();
        if (err != ESP_OK) s_static_ssid[0] = '\0';
    }
    xSemaphoreGive(s_lock);
    return err;
}
/*
 * Static settings saved on Wi-Fi belong to the network in use. An empty
 * binding applies static settings everywhere, so static settings are never
 * durable without their binding: a static binding is written before the
 * settings, and a DHCP save clears it only after the settings (see
 * jetkvm_network_settings_set). Caller holds s_lock.
 */
static bool same_static_ipv4(const jetkvm_network_settings_t *a,
                             const jetkvm_network_settings_t *b)
{
    return !a->dhcp && !b->dhcp && strcmp(a->address, b->address) == 0 &&
           strcmp(a->netmask, b->netmask) == 0 && strcmp(a->gateway, b->gateway) == 0 &&
           memcmp(a->dns, b->dns, sizeof(a->dns)) == 0;
}
static bool bind_static_network(const jetkvm_network_settings_t *settings)
{
    memcpy(s_previous_static_ssid, s_static_ssid, sizeof(s_previous_static_ssid));
    if (!s_wifi_known || settings->dhcp) return true;
    /* Saving other fields (the hostname) resubmits the static settings; keep
     * them with their network rather than the one in use now. An untrusted
     * binding (an interrupted update) is not kept: bind to this network. */
    if (s_static_ssid[0] && !s_binding_untrusted && same_static_ipv4(settings, &s_settings))
        return true;
    snprintf(s_static_ssid, sizeof(s_static_ssid), "%s", s_wifi_ssid);
    if (save_static_ssid() == ESP_OK) return true;
    memcpy(s_static_ssid, s_previous_static_ssid, sizeof(s_static_ssid));
    return false;
}
bool jetkvm_network_settings_get(jetkvm_network_settings_t *settings, void *ctx)
{
    (void)ctx;
    if (!s_lock || !settings) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *settings = s_settings;
    xSemaphoreGive(s_lock);
    return true;
}
const char *jetkvm_network_settings_set(const jetkvm_network_settings_t *requested, void *ctx)
{
    (void)ctx;
    if (!requested) return "Invalid network settings";
    /* Clearing the hostname field saves the default name. */
    jetkvm_network_settings_t filled = *requested;
    (void)jetkvm_network_settings_fill_hostname(&filled, s_device_id);
    const jetkvm_network_settings_t *settings = &filled;
    const char *error = jetkvm_network_settings_validate(settings);
    if (error) return error;
    if (!s_lock || !active_netif()) return "Network interface unavailable";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_pending) error = "Network settings are still being applied";
    else if (!s_apply_failed && memcmp(&s_settings, settings, sizeof(*settings)) == 0) error = NULL;
    else if (!s_apply_failed && jetkvm_network_settings_connectivity_equal(&s_settings, settings)) {
        /* Upstream saves time sync directly; the interface stays as it is.
         * Configure before persisting so a failed SNTP restart is reported
         * and the saved servers are the ones in effect. */
        if (apply_time_sync(settings) != ESP_OK) {
            (void)apply_time_sync(&s_settings);
            error = "Could not apply time sync servers";
        } else if (save(settings) != ESP_OK) {
            (void)apply_time_sync(&s_settings);
            error = "Could not save network settings";
        } else {
            s_settings = *settings;
        }
    }
    else if (xTaskCreateWithCaps(apply_worker, "jk_network", 4096, NULL, 4, &s_task,
                                 MALLOC_CAP_RTCRAM | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS)
        error = "Could not start network settings worker";
    else if (set_binding_pending(true) != ESP_OK) {
        /* Nothing changed yet; NVS cannot take the update. */
        vTaskDeleteWithCaps(s_task);
        s_task = NULL;
        error = "Could not save network settings";
    } else if (!bind_static_network(settings)) {
        vTaskDeleteWithCaps(s_task);
        s_task = NULL;
        end_binding_update();
        error = "Could not save network settings";
    } else if (save(settings) != ESP_OK) {
        vTaskDeleteWithCaps(s_task);
        s_task = NULL;
        memcpy(s_static_ssid, s_previous_static_ssid, sizeof(s_static_ssid));
        /* If the old binding cannot be written back either, the marker stays
         * and the next boot does not trust the binding. */
        if (save_static_ssid() == ESP_OK) end_binding_update();
        error = "Could not save network settings";
    } else {
        if (s_wifi_known && settings->dhcp && s_static_ssid[0]) {
            /* DHCP is durable now; a stale binding would only matter if
             * static settings came back, and those are rebound on save. */
            s_static_ssid[0] = '\0';
            if (save_static_ssid() != ESP_OK) ESP_LOGW(TAG, "Could not clear the static IPv4 network");
        }
        end_binding_update();
        s_previous = s_settings;
        s_settings = *settings;
        s_pending = true;
        xTaskNotifyGive(s_task);
    }
    xSemaphoreGive(s_lock);
    return error;
}

typedef struct {
    esp_netif_t *netif;
    jetkvm_network_state_t *state;
    bool renew;
    err_t error;
} lease_request_t;
static void timestamp(time_t value, char output[JETKVM_NETWORK_TIMESTAMP_CAPACITY])
{
    struct tm utc;
    if (value > 1700000000 && gmtime_r(&value, &utc)) {
        strftime(output, JETKVM_NETWORK_TIMESTAMP_CAPACITY, "%Y-%m-%dT%H:%M:%SZ", &utc);
    }
}
static void inspect_lease(void *arg)
{
    /* Only TCP/IP context accesses this estimate. lwIP's lease age advances
     * in coarse ticks; anchor the deadline until a new ACK rather than making
     * the displayed expiry move forward on every UI refresh. */
    static struct dhcp *last_lease;
    static uint32_t last_xid, last_age, last_duration;
    static jetkvm_network_lease_clock_t clock;
    static bool renew_pending;
    lease_request_t *request = arg;
    struct netif *netif = esp_netif_get_netif_impl(request->netif);
    if (!netif) return;
    struct dhcp *dhcp = netif_dhcp_data(netif);
    if (!dhcp || !dhcp_supplied_address(netif)) { last_lease = NULL; renew_pending = false; return; }
    if (request->renew) {
        request->error = dhcp_renew(netif);
        if (request->error == ERR_OK) renew_pending = true;
        return;
    }
    jetkvm_network_state_t *state = request->state;
    state->lease_valid = true;
    state->lease_seconds = dhcp->offered_t0_lease;
    ipaddr_ntoa_r(&dhcp->server_ip_addr, state->lease_server, sizeof(state->lease_server));
    int64_t now_us = esp_timer_get_time();
    bool renewed = last_lease != dhcp || dhcp->lease_used < last_age ||
        (dhcp->state == DHCP_STATE_BOUND && (dhcp->xid != last_xid || renew_pending)) ||
        dhcp->offered_t0_lease != last_duration;
    uint32_t used = (uint32_t)dhcp->lease_used * DHCP_COARSE_TIMER_SECS;
    uint32_t remaining = dhcp->offered_t0_lease > used ? dhcp->offered_t0_lease - used : 0;
    last_lease = dhcp;
    last_duration = dhcp->offered_t0_lease;
    last_age = dhcp->lease_used;
    if (dhcp->state == DHCP_STATE_BOUND) { last_xid = dhcp->xid; renew_pending = false; }
    int64_t expiry = jetkvm_network_lease_expiry(&clock, remaining, renewed, DHCP_COARSE_TIMER_SECS, now_us, time(NULL));
    if (dhcp->offered_t0_lease != UINT32_MAX) timestamp(expiry, state->lease_expiry);
}
const char *jetkvm_network_renew_dhcp(void *ctx)
{
    (void)ctx;
    if (!s_lock) return "Network interface unavailable";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_netif_t *netif = active_netif();
    if (s_pending || static_applies(&s_settings) || !netif) {
        xSemaphoreGive(s_lock);
        return "DHCP is not active or settings are still being applied";
    }
    lease_request_t request = {.netif = netif, .renew = true, .error = ERR_ARG};
    err_t err = tcpip_callback_wait(inspect_lease, &request);
    xSemaphoreGive(s_lock);
    return err == ERR_OK && request.error == ERR_OK ? NULL : "No active DHCP lease to renew";
}
void jetkvm_network_snapshot(jetkvm_network_state_t *state)
{
    memset(state, 0, sizeof(*state));
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_netif_t *netif = active_netif();
    strcpy(state->hostname, s_settings.hostname);
    if (!netif) goto done;
    snprintf(state->interface_name, sizeof(state->interface_name), "%s",
             strcmp(esp_netif_get_ifkey(netif), "ETH_DEF") == 0 ? "eth0" : "wlan0");
    const char *hostname = NULL;
    if (esp_netif_get_hostname(netif, &hostname) == ESP_OK && hostname) {
        snprintf(state->hostname, sizeof(state->hostname), "%s", hostname);
    }
    esp_netif_dhcp_status_t status;
    state->dhcp = esp_netif_dhcpc_get_status(netif, &status) == ESP_OK && status == ESP_NETIF_DHCP_STARTED;
    state->up = esp_netif_is_netif_up(netif);
    uint8_t mac[6];
    if (esp_netif_get_mac(netif, mac) == ESP_OK) {
        snprintf(state->mac_address, sizeof(state->mac_address), "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    esp_netif_ip_info_t ip;
    if (state->up && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr) {
        state->ipv4_ready = state->online = true;
        snprintf(state->ipv4_address, sizeof(state->ipv4_address), IPSTR, IP2STR(&ip.ip));
        snprintf(state->netmask, sizeof(state->netmask), IPSTR, IP2STR(&ip.netmask));
        if (ip.gw.addr) snprintf(state->gateway, sizeof(state->gateway), IPSTR, IP2STR(&ip.gw));
    }
    char *dns_values[] = {state->dns_main, state->dns_backup, state->dns_fallback};
    for (unsigned i = 0; i < 3; ++i) {
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(netif, (esp_netif_dns_type_t)i, &dns) == ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr) {
            snprintf(dns_values[i], 16, IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
    }
    if (state->dhcp && state->ipv4_ready) {
        lease_request_t request = {.netif = netif, .state = state};
        tcpip_callback_wait(inspect_lease, &request);
    }
done:
    timestamp(time(NULL), state->last_updated);
    xSemaphoreGive(s_lock);
}

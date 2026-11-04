/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */
#include "jetkvm_time_sync.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"

#define JETKVM_MIN_TRUSTED_UNIX_TIME 1577836800LL /* 2020-01-01 */

static const char *TAG = "jetkvm_time_sync";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_started;
/* start() has been called and stop() has not; configure may (re)start. Mutex-guarded. */
static bool s_wanted;
/* lwIP keeps pointers to the names it is given; these buffers never move. */
static char s_names[CONFIG_LWIP_SNTP_MAX_SERVERS][JETKVM_TIME_SYNC_NAME_CAPACITY] = {
    JETKVM_TIME_SYNC_FALLBACK_SERVER,
};
static size_t s_count = 1;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;

static void lock(void)
{
    portENTER_CRITICAL(&s_lock);
    if (!s_mutex) s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_mutex);
}

static bool started(void)
{
    portENTER_CRITICAL(&s_lock);
    bool value = s_started;
    portEXIT_CRITICAL(&s_lock);
    return value;
}

bool jetkvm_time_sync_ready(void)
{
    time_t now = time(NULL);
    return (int64_t)now >= JETKVM_MIN_TRUSTED_UNIX_TIME;
}

static esp_err_t start_locked(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(0, {NULL});
    config.num_of_servers = s_count;
    for (size_t i = 0; i < s_count; ++i) config.servers[i] = s_names[i];
    esp_err_t err = esp_netif_sntp_init(&config);
    portENTER_CRITICAL(&s_lock);
    s_started = err == ESP_OK;
    portEXIT_CRITICAL(&s_lock);
    return err;
}

esp_err_t jetkvm_time_sync_configure(const jetkvm_time_sync_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    lock();
    /* Stop before the names change: the client may hold the old pointers. */
    if (s_started) esp_sntp_stop();
    s_count = jetkvm_time_sync_server_list(config, s_names, CONFIG_LWIP_SNTP_MAX_SERVERS);
    esp_err_t err = ESP_OK;
    if (s_started) {
        for (size_t i = 0; i < CONFIG_LWIP_SNTP_MAX_SERVERS; ++i) {
            esp_sntp_setservername((u8_t)i, i < s_count ? s_names[i] : NULL);
        }
        /* Upstream re-queries as soon as the servers change. */
        err = esp_netif_sntp_start();
        if (err != ESP_OK) {
            /* The client is stopped: rebuild it with the new names, and if
             * that fails too leave s_started clear so start() can retry. */
            esp_netif_sntp_deinit();
            err = start_locked();
        }
    } else if (s_wanted) {
        /* A previous restart failed: bring the client back with the new names. */
        err = start_locked();
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NTP servers: %s%s", s_count ? s_names[0] : "(none)",
                 s_count > 1 ? " and fallbacks" : "");
    }
    unlock();
    return err;
}

esp_err_t jetkvm_time_sync_start(void)
{
    lock();
    s_wanted = true;
    if (s_started) {
        unlock();
        return ESP_OK;
    }
    esp_err_t err = start_locked();
    unlock();
    return err;
}

esp_err_t jetkvm_time_sync_wait(TickType_t timeout)
{
    if (jetkvm_time_sync_ready()) {
        return ESP_OK;
    }
    if (!started()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Wi-Fi association can complete well after application startup.  The
     * first SNTP request may therefore run without a route and otherwise sit
     * on lwIP's one-hour update interval.  Explicitly restart the already
     * configured client whenever a caller needs trusted time. */
    lock();
    esp_err_t err = esp_netif_sntp_start();
    unlock();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_netif_sntp_sync_wait(timeout);
    if (err != ESP_OK) {
        unsigned int reachability = 0U;
        if (esp_netif_sntp_reachability(0U, &reachability) == ESP_OK) {
            ESP_LOGW(TAG, "SNTP wait failed: %s, reachability=0x%02x",
                     esp_err_to_name(err), reachability);
        }
        return err;
    }
    if (!jetkvm_time_sync_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *server = esp_sntp_getservername(0);
    ESP_LOGI(TAG, "system time synchronized with %s", server ? server : "NTP");
    return ESP_OK;
}

void jetkvm_time_sync_stop(void)
{
    lock();
    s_wanted = false;
    portENTER_CRITICAL(&s_lock);
    bool was_started = s_started;
    s_started = false;
    portEXIT_CRITICAL(&s_lock);
    if (was_started) {
        esp_netif_sntp_deinit();
    }
    unlock();
}

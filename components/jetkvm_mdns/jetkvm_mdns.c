#include "jetkvm_mdns.h"
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mdns.h"

static const char *TAG = "jetkvm_mdns";
static SemaphoreHandle_t s_lock;
static EXT_RAM_BSS_ATTR jetkvm_mdns_device_t s_device;
static EXT_RAM_BSS_ATTR jetkvm_mdns_plan_t s_running;
static bool s_started;
/* The name the responder answers to now. espressif/mdns renames itself when
 * another host on the link has the name. The responder's task writes it while
 * a setter holds s_lock and waits for that task, so it has its own lock. */
static portMUX_TYPE s_hostname_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_hostname[MDNS_NAME_BUF_LEN];

static void set_hostname(const char *hostname)
{
    char name[sizeof(s_hostname)];
    snprintf(name, sizeof(name), "%s", hostname ? hostname : "");
    portENTER_CRITICAL(&s_hostname_lock);
    memcpy(s_hostname, name, sizeof(s_hostname));
    portEXIT_CRITICAL(&s_hostname_lock);
}

static void hostname_changed(const char *hostname, void *arg)
{
    (void)arg;
    set_hostname(hostname);
}

static void stop_responder(void)
{
    mdns_free();
    set_hostname(NULL);
}

/* espressif/mdns puts each item in front of its list and sends the list from
 * the front, so the items go in in reverse to leave in the plan's order. */
static void txt_items(const jetkvm_mdns_plan_t *plan, mdns_txt_item_t items[JETKVM_MDNS_TXT_COUNT])
{
    for (size_t i = 0; i < JETKVM_MDNS_TXT_COUNT; ++i) {
        const jetkvm_mdns_txt_t *txt = &plan->txt[JETKVM_MDNS_TXT_COUNT - 1 - i];
        items[i] = (mdns_txt_item_t){.key = txt->key, .value = txt->value};
    }
}

static esp_err_t start_responder(const jetkvm_mdns_plan_t *plan)
{
    mdns_txt_item_t items[JETKVM_MDNS_TXT_COUNT];
    txt_items(plan, items);
    esp_err_t err = mdns_init();
    /* mdns_free() removes the callback. */
    if (err == ESP_OK) err = mdns_register_hostname_changed_callback(hostname_changed, NULL);
    if (err == ESP_OK) err = mdns_hostname_set(plan->hostname);
    if (err == ESP_OK) err = mdns_service_add(plan->instance, JETKVM_MDNS_SERVICE_TYPE,
                                              JETKVM_MDNS_SERVICE_PROTO, plan->port,
                                              items, JETKVM_MDNS_TXT_COUNT);
    if (err != ESP_OK) stop_responder();
    return err;
}

static esp_err_t update_responder(const jetkvm_mdns_plan_t *plan, unsigned changes)
{
    esp_err_t err = ESP_OK;
    if (changes & JETKVM_MDNS_HOSTNAME) {
        err = mdns_hostname_set(plan->hostname);
        if (err == ESP_OK) err = mdns_service_instance_name_set(JETKVM_MDNS_SERVICE_TYPE,
                                                                JETKVM_MDNS_SERVICE_PROTO, plan->instance);
    }
    if (err == ESP_OK && (changes & JETKVM_MDNS_PORT)) {
        err = mdns_service_port_set(JETKVM_MDNS_SERVICE_TYPE, JETKVM_MDNS_SERVICE_PROTO, plan->port);
    }
    if (err == ESP_OK && (changes & JETKVM_MDNS_TXT)) {
        /* espressif/mdns announces the new TXT record with the cache-flush
         * bit, so browsers see setup=true at once (upstream's UpdateTXT). */
        mdns_txt_item_t items[JETKVM_MDNS_TXT_COUNT];
        txt_items(plan, items);
        err = mdns_service_txt_set(JETKVM_MDNS_SERVICE_TYPE, JETKVM_MDNS_SERVICE_PROTO,
                                   items, JETKVM_MDNS_TXT_COUNT);
    }
    return err;
}

/* Caller holds s_lock. */
static void apply(void)
{
    if (!s_started) return;
    jetkvm_mdns_plan_t wanted;
    jetkvm_mdns_plan(&s_device, &wanted);
    unsigned changes = jetkvm_mdns_changes(&s_running, &wanted);
    if (!changes) return;
    if (changes & JETKVM_MDNS_STOP) {
        stop_responder();
        memset(&s_running, 0, sizeof(s_running));
        ESP_LOGI(TAG, "mDNS responder stopped");
        return;
    }
    esp_err_t err = ESP_OK;
    if (changes & JETKVM_MDNS_START) {
        err = start_responder(&wanted);
    } else if (update_responder(&wanted, changes) != ESP_OK) {
        /* Start over rather than leave a half-updated advertisement. */
        stop_responder();
        err = start_responder(&wanted);
    }
    if (err != ESP_OK) {
        memset(&s_running, 0, sizeof(s_running));
        ESP_LOGW(TAG, "mDNS responder unavailable: %s", esp_err_to_name(err));
        return;
    }
    s_running = wanted;
    ESP_LOGI(TAG, "mDNS: %s.local, %s.%s port %u, setup=%s", wanted.hostname,
             JETKVM_MDNS_SERVICE_TYPE, JETKVM_MDNS_SERVICE_PROTO, wanted.port, wanted.txt[1].value);
}

void jetkvm_mdns_init(const char *device_id, const char *version, uint16_t http_port)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGW(TAG, "mDNS responder unavailable: no memory");
        return;
    }
    snprintf(s_device.device_id, sizeof(s_device.device_id), "%s", device_id ? device_id : "");
    snprintf(s_device.version, sizeof(s_device.version), "%s", version ? version : "");
    s_device.http_port = http_port;
}

size_t jetkvm_mdns_get_hostname(char *hostname, size_t capacity)
{
    if (!hostname || !capacity) return 0;
    char name[sizeof(s_hostname)];
    portENTER_CRITICAL(&s_hostname_lock);
    memcpy(name, s_hostname, sizeof(name));
    portEXIT_CRITICAL(&s_hostname_lock);
    size_t length = (size_t)snprintf(hostname, capacity, "%s", name);
    return length < capacity ? length : capacity - 1;
}

void jetkvm_mdns_set_network(const char *hostname, uint8_t mode)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_device.hostname, sizeof(s_device.hostname), "%s", hostname ? hostname : "");
    s_device.mode = mode;
    apply();
    xSemaphoreGive(s_lock);
}

void jetkvm_mdns_set_setup(bool setup)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_device.setup = setup;
    apply();
    xSemaphoreGive(s_lock);
}

void jetkvm_mdns_set_https_port(uint16_t port)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_device.https_port = port;
    apply();
    xSemaphoreGive(s_lock);
}

void jetkvm_mdns_start(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_started = true;
    apply();
    xSemaphoreGive(s_lock);
}

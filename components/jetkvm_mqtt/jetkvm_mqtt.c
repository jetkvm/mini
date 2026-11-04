/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_mqtt.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jetkvm_cloud_service.h"
#include "jetkvm_device_state.h"
#include "jetkvm_hid_profiles.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_mqtt_internal.h"
#include "jetkvm_network_settings.h"
#include "jetkvm_usb_hid.h"
#include "jetkvm_virtual_media.h"
#include "mqtt_client.h"
#include "mbedtls/ssl.h"
#include "webrtc_transport.h"

/* The manager's task starts and reconnects the client and publishes the
 * states; its deepest path is a round of status publishes over TLS. The
 * stack is in PSRAM; like the client's task, this task must not call the
 * flash driver. */
#define MQTT_MANAGER_TASK_STACK (8U * 1024U)
/* The client's task runs mqtt_event_handler. Its deepest path is a virtual
 * media command that publishes the Home Assistant discovery afterwards, with
 * or without TLS. The stack is in PSRAM
 * (CONFIG_MQTT_TASK_STACK_ON_EXTERNAL_MEMORY), so nothing that this task
 * runs may write or read flash through the flash driver: such a call runs
 * with the cache disabled and asserts on a stack outside internal RAM. */
#define MQTT_CLIENT_TASK_STACK (10U * 1024U)
/* The stack of the task that stores the jiggler's state in NVS for the
 * client's task, in internal RAM while the command runs. */
#define MQTT_JIGGLER_TASK_STACK 4096U
#define MQTT_CLIENT_BUFFER_BYTES 2048
#define MQTT_OUTBOX_LIMIT_BYTES (64U * 1024U)
#define MQTT_STATUS_INTERVAL_MS 15000U
#define MQTT_RECONNECT_INTERVAL_MS 10000U
#define MQTT_TEST_TIMEOUT_MS 5000U
#define MQTT_PUBLISH_DRAIN_TIMEOUT_MS 5000U
#define MQTT_URI_CAPACITY (JETKVM_MQTT_BROKER_CAPACITY + 32U)
#define MQTT_CLIENT_ID_CAPACITY (JETKVM_DEVICE_ID_CAPACITY + 16U)
#define MQTT_TOPIC_CAPACITY (JETKVM_MQTT_EFFECTIVE_TOPIC_CAPACITY + 48U)
#define MQTT_VIDEO_PAYLOAD_CAPACITY 96U
/* A video source without a mode has lost its signal when this time has
 * passed. After a change back from service video the HDMI capture has its
 * mode within 1.7 s. */
#define MQTT_NO_SIGNAL_DELAY_MS 3000U

static const char *TAG = "jetkvm_mqtt";

typedef struct {
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t lock;
    TaskHandle_t manager_task;
    esp_mqtt_client_handle_t client;
    jetkvm_mqtt_config_t config;
    jetkvm_mqtt_status_t status;
    jetkvm_memory_policy_reservation_t tls_reservation;
    char device_id[JETKVM_DEVICE_ID_CAPACITY];
    /* The video/state payload that the connected broker holds. */
    char video_payload[MQTT_VIDEO_PAYLOAD_CAPACITY];
    /* Since when the video source is without a mode, outside a change. */
    TickType_t video_lost_at;
    bool video_lost;
    TickType_t next_reconnect;
    TickType_t last_status_publish;
    uint32_t publishes_in_flight;
    bool reconfiguring;
} mqtt_runtime_t;

static mqtt_runtime_t s_mqtt;

typedef struct {
    StaticSemaphore_t done_storage;
    SemaphoreHandle_t done;
    bool success;
    char error[JETKVM_MQTT_ERROR_CAPACITY];
} mqtt_test_context_t;

typedef struct {
    const char *component;
    const char *object_id;
    const char *name;
    const char *state_suffix;
    const char *value_template;
    const char *device_class;
    const char *unit;
    const char *state_class;
    const char *icon;
    const char *entity_category;
    int enabled_by_default;
} ha_entity_t;

/* A numeric sensor whose template renders None is unknown in Home Assistant:
 * the frame rate without a video mode, the storage without a TF card. */
static const ha_entity_t s_general_discovery[] = {
    {"binary_sensor", "online", "Online", "status",
     "{{ 'ON' if value_json.online else 'OFF' }}", "connectivity", NULL,
     NULL, NULL, NULL, -1},
    {"binary_sensor", "video_signal", "Video Signal", "video/state",
     "{{ 'ON' if value_json.ready else 'OFF' }}", "connectivity", NULL,
     NULL, "mdi:video-input-hdmi", NULL, -1},
    {"sensor", "video_resolution", "Video Resolution", "video/state",
     "{{ (value_json.width ~ 'x' ~ value_json.height) if value_json.ready "
     "else 'No signal' }}", NULL, NULL, NULL,
     "mdi:monitor", "diagnostic", 0},
    {"sensor", "video_fps", "Video FPS", "video/state",
     "{{ (value_json.fps | round(1)) if value_json.ready else None }}", NULL,
     "fps", "measurement", "mdi:speedometer", "diagnostic", 0},
    {"binary_sensor", "cloud_connected", "Cloud Connected", "cloud/state",
     "{{ 'ON' if value_json.connected else 'OFF' }}", "connectivity", NULL,
     NULL, "mdi:cloud", "diagnostic", -1},
    {"sensor", "active_sessions", "Active Sessions", "sessions/state",
     "{{ value_json.active_sessions }}", NULL, NULL, "measurement",
     "mdi:account-multiple", NULL, -1},
    {"binary_sensor", "usb_state", "USB Connected", "usb/state",
     "{{ 'ON' if value_json.state == 'configured' else 'OFF' }}",
     "connectivity", NULL, NULL, "mdi:usb", NULL, -1},
    {"sensor", "ip_address", "IP Address", "network/state",
     "{{ value_json.ip_address }}", NULL, NULL, NULL, "mdi:ip-network",
     "diagnostic", 0},
    {"sensor", "hostname", "Hostname", "network/state",
     "{{ value_json.hostname }}", NULL, NULL, NULL, "mdi:dns",
     "diagnostic", 0},
    {"sensor", "memory_used", "Memory Used", "system/state",
     "{{ (value_json.memory_used / 1048576) | round(1) }}", NULL, "MB",
     "measurement", "mdi:memory", "diagnostic", 0},
    {"sensor", "storage_used", "Storage Used", "system/state",
     "{{ ((value_json.storage_used / 1048576) | round(1)) "
     "if value_json.storage_used is defined else None }}", "data_size",
     "MB", "measurement", NULL, "diagnostic", 0},
    {"sensor", "storage_free", "Storage Free", "system/state",
     "{{ ((value_json.storage_free / 1048576) | round(1)) "
     "if value_json.storage_free is defined else None }}", "data_size",
     "MB", "measurement", NULL, "diagnostic", 0},
};

void jetkvm_mqtt_secure_zero(void *memory, size_t size)
{
    volatile unsigned char *bytes = memory;
    while (bytes != NULL && size-- > 0U) {
        *bytes++ = 0U;
    }
}

static bool runtime_ready(void)
{
    return s_mqtt.lock != NULL && s_mqtt.status.initialized;
}

static void copy_text(char *output, size_t capacity, const char *value)
{
    if (output == NULL || capacity == 0U) {
        return;
    }
    const char *source = value ? value : "";
    size_t length = 0U;
    while (length + 1U < capacity && source[length] != '\0') {
        length++;
    }
    if (length > 0U) {
        memcpy(output, source, length);
    }
    output[length] = '\0';
}

static void set_error_locked(const char *message)
{
    copy_text(s_mqtt.status.error, sizeof(s_mqtt.status.error), message);
}

static void release_tls_locked(void)
{
    if (s_mqtt.tls_reservation.active) {
        jetkvm_memory_policy_release(&s_mqtt.tls_reservation);
    }
}

static void schedule_reconnect_locked(void)
{
    s_mqtt.next_reconnect = xTaskGetTickCount() +
        pdMS_TO_TICKS(MQTT_RECONNECT_INTERVAL_MS);
}

static void record_client_start_failure(uint32_t generation,
                                        const char *message)
{
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    if (generation == s_mqtt.status.generation &&
        s_mqtt.status.service_started && s_mqtt.config.enabled &&
        s_mqtt.client == NULL && !s_mqtt.reconfiguring) {
        s_mqtt.status.connected = false;
        s_mqtt.status.connecting = false;
        set_error_locked(message);
        schedule_reconnect_locked();
    }
    xSemaphoreGive(s_mqtt.lock);
}

static bool config_snapshot(jetkvm_mqtt_config_t *config,
                            char *device_id, size_t device_id_capacity)
{
    if (!runtime_ready() || config == NULL || device_id == NULL ||
        device_id_capacity == 0U) {
        return false;
    }
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    *config = s_mqtt.config;
    copy_text(device_id, device_id_capacity, s_mqtt.device_id);
    xSemaphoreGive(s_mqtt.lock);
    return true;
}

static bool topic_from_snapshot(const jetkvm_mqtt_config_t *config,
                                const char *device_id, const char *suffix,
                                char *output, size_t output_capacity)
{
    char base[JETKVM_MQTT_EFFECTIVE_TOPIC_CAPACITY];
    if (suffix == NULL ||
        !jetkvm_mqtt_build_base_topic(config, device_id, base, sizeof(base))) {
        return false;
    }
    int written = snprintf(output, output_capacity, "%s/%s", base, suffix);
    return written >= 0 && (size_t)written < output_capacity;
}

static bool current_topic(const char *suffix, char *output,
                          size_t output_capacity)
{
    jetkvm_mqtt_config_t config;
    char device_id[JETKVM_DEVICE_ID_CAPACITY];
    bool ok = config_snapshot(&config, device_id, sizeof(device_id)) &&
              topic_from_snapshot(&config, device_id, suffix, output,
                                  output_capacity);
    jetkvm_mqtt_secure_zero(&config, sizeof(config));
    return ok;
}

static bool publish_current_topic(const char *topic, const char *payload,
                                  size_t payload_length, bool retained)
{
    if (!runtime_ready() || topic == NULL || payload == NULL ||
        payload_length > INT32_MAX) {
        return false;
    }
    /* esp-mqtt runs mqtt_event_handler, which takes s_mqtt.lock, while it
     * holds its own client lock, and publishing takes that client lock. So
     * publish without s_mqtt.lock; jetkvm_mqtt_set_config waits for
     * publishes_in_flight to reach zero before it destroys the client. */
    esp_mqtt_client_handle_t client = NULL;
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    if (s_mqtt.client != NULL && s_mqtt.status.connected &&
        !s_mqtt.reconfiguring) {
        client = s_mqtt.client;
        s_mqtt.publishes_in_flight++;
    }
    xSemaphoreGive(s_mqtt.lock);
    if (client == NULL) {
        return false;
    }
    int message_id = esp_mqtt_client_publish(
        client, topic, payload, (int)payload_length, 1, retained ? 1 : 0);
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    s_mqtt.publishes_in_flight--;
    if (message_id < 0) {
        s_mqtt.status.publish_failures++;
    }
    xSemaphoreGive(s_mqtt.lock);
    return message_id >= 0;
}

/* Returns once no publish uses the client. The caller sets reconfiguring
 * first, so no new publish starts. */
static void wait_for_publishes(void)
{
    for (;;) {
        xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
        bool idle = s_mqtt.publishes_in_flight == 0U;
        xSemaphoreGive(s_mqtt.lock);
        if (idle) {
            return;
        }
        vTaskDelay(1);
    }
}

static bool publish_suffix(const char *suffix, const char *payload,
                           size_t payload_length)
{
    char topic[MQTT_TOPIC_CAPACITY];
    return current_topic(suffix, topic, sizeof(topic)) &&
           publish_current_topic(topic, payload, payload_length, true);
}

static bool publish_json_suffix(const char *suffix, cJSON *root)
{
    if (root == NULL) {
        return false;
    }
    char *rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (rendered == NULL) {
        return false;
    }
    bool published = publish_suffix(suffix, rendered, strlen(rendered));
    cJSON_free(rendered);
    return published;
}

static const char *usb_state_name(void)
{
    static char state_name[16];
    jetkvm_usb_hid_state_t state = {0};
    const char *name = "not attached";
    if (jetkvm_usb_hid_get_state(&state) == ESP_OK && state.initialized &&
        state.connected) {
        if (state.suspended) {
            name = "suspended";
        } else if (state.mounted) {
            name = "configured";
        } else {
            name = "attached";
        }
    }
    copy_text(state_name, sizeof(state_name), name);
    return state_name;
}

/* Writes the video/state payload and returns its length. Returns 0 when
 * there is nothing to publish: without a mode while the video source
 * changes, the signal is not lost and the broker keeps the last payload. */
static size_t video_state_payload(const webrtc_transport_stats_t *stats,
                                  char *payload, size_t capacity)
{
    const bool ready = stats != NULL && stats->mode_valid;
    if (!ready && stats != NULL && stats->video_switching) {
        return 0U;
    }
    int length = ready ?
        snprintf(payload, capacity,
                 "{\"ready\":true,\"width\":%u,\"height\":%u,\"fps\":%.1f}",
                 (unsigned)stats->width, (unsigned)stats->height,
                 (double)stats->fps) :
        snprintf(payload, capacity,
                 "{\"ready\":false,\"error\":\"no_signal\",\"width\":0,"
                 "\"height\":0,\"fps\":0}");
    return length > 0 && (size_t)length < capacity ? (size_t)length : 0U;
}

/* With only_changes, publishes only a payload that the broker does not
 * hold. A lost signal is published when it has lasted
 * MQTT_NO_SIGNAL_DELAY_MS. */
static void publish_video_state(bool only_changes)
{
    webrtc_transport_stats_t stats = {0};
    bool available = webrtc_transport_get_stats(&stats) == ESP_OK;
    bool ready = available && stats.mode_valid;
    char payload[MQTT_VIDEO_PAYLOAD_CAPACITY];
    size_t length = video_state_payload(available ? &stats : NULL, payload,
                                        sizeof(payload));
    TickType_t now = xTaskGetTickCount();
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    if (ready || length == 0U) {
        s_mqtt.video_lost = false;
    } else if (!s_mqtt.video_lost) {
        s_mqtt.video_lost = true;
        s_mqtt.video_lost_at = now;
    }
    bool settled = !s_mqtt.video_lost ||
                   (now - s_mqtt.video_lost_at) >=
                       pdMS_TO_TICKS(MQTT_NO_SIGNAL_DELAY_MS);
    bool changed = length > 0U &&
                   strcmp(payload, s_mqtt.video_payload) != 0;
    xSemaphoreGive(s_mqtt.lock);
    if (length == 0U || !settled || (only_changes && !changed)) {
        return;
    }
    if (publish_suffix("video/state", payload, length)) {
        xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
        copy_text(s_mqtt.video_payload, sizeof(s_mqtt.video_payload),
                  payload);
        xSemaphoreGive(s_mqtt.lock);
    }
}

static void publish_cloud_state(void)
{
    jetkvm_cloud_service_stats_t stats = {0};
    bool connected = jetkvm_cloud_service_get_stats(&stats) == ESP_OK &&
                     stats.client.connected;
    const char *payload = connected ? "{\"connected\":true}" :
                                      "{\"connected\":false}";
    (void)publish_suffix("cloud/state", payload, strlen(payload));
}

static void publish_sessions_state(void)
{
    webrtc_transport_stats_t stats = {0};
    unsigned active = webrtc_transport_get_stats(&stats) == ESP_OK &&
                      stats.browser_session_active && stats.peer_connected ?
                          1U : 0U;
    char payload[48];
    int length = snprintf(payload, sizeof(payload),
                          "{\"active_sessions\":%u}", active);
    if (length > 0 && (size_t)length < sizeof(payload)) {
        (void)publish_suffix("sessions/state", payload, (size_t)length);
    }
}

static void publish_usb_state(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root != NULL &&
        cJSON_AddStringToObject(root, "state", usb_state_name())) {
        (void)publish_json_suffix("usb/state", root);
    } else {
        cJSON_Delete(root);
    }
}

static void publish_jiggler_state(void)
{
    bool enabled = false;
    (void)jetkvm_hid_profiles_get_jiggler_state(&enabled);
    const char *payload = enabled ? "{\"enabled\":true}" :
                                    "{\"enabled\":false}";
    (void)publish_suffix("jiggler/state", payload, strlen(payload));
}

static void network_snapshot(char ip_address[16],
                             char hostname[64])
{
    /* The active Ethernet or Wi-Fi interface, with the effective hostname. */
    jetkvm_network_state_t state;
    jetkvm_network_snapshot(&state);
    copy_text(ip_address, 16U, state.ipv4_address);
    copy_text(hostname, 64U, state.hostname);
}

static void publish_network_state(void)
{
    char ip_address[16];
    char hostname[64];
    network_snapshot(ip_address, hostname);
    cJSON *root = cJSON_CreateObject();
    if (root != NULL &&
        cJSON_AddStringToObject(root, "ip_address", ip_address) &&
        cJSON_AddStringToObject(root, "hostname", hostname)) {
        (void)publish_json_suffix("network/state", root);
    } else {
        cJSON_Delete(root);
    }
}

/* Writes the system/state payload and returns its length, or 0 when it does
 * not fit. The storage fields are present only with a mounted TF card
 * filesystem (space != NULL). */
static size_t system_state_payload(uint64_t memory_used,
                                   uint64_t memory_total,
                                   const jetkvm_virtual_media_space_t *space,
                                   char *payload, size_t capacity)
{
    int length = snprintf(payload, capacity,
                          "{\"memory_used\":%" PRIu64
                          ",\"memory_total\":%" PRIu64,
                          memory_used, memory_total);
    if (length <= 0 || (size_t)length >= capacity) {
        return 0U;
    }
    size_t used = (size_t)length;
    length = space != NULL ?
        snprintf(payload + used, capacity - used,
                 ",\"storage_used\":%" PRIu64 ",\"storage_free\":%" PRIu64
                 "}",
                 (uint64_t)space->bytes_used, (uint64_t)space->bytes_free) :
        snprintf(payload + used, capacity - used, "}");
    return length > 0 && (size_t)length < capacity - used ?
               used + (size_t)length : 0U;
}

static void publish_system_state(void)
{
    uint64_t memory_total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    uint64_t memory_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint64_t memory_used = memory_total >= memory_free ?
                               memory_total - memory_free : 0U;
    jetkvm_virtual_media_space_t space = {0};
    bool have_space = jetkvm_virtual_media_get_space(&space) == ESP_OK;
    char payload[256];
    size_t length = system_state_payload(memory_used, memory_total,
                                         have_space ? &space : NULL, payload,
                                         sizeof(payload));
    if (length > 0U) {
        (void)publish_suffix("system/state", payload, length);
    }
}

static void url_basename(const char *url, char *output, size_t capacity)
{
    copy_text(output, capacity, url);
    if (url == NULL || url[0] == '\0') {
        return;
    }
    const char *path = strrchr(url, '/');
    if (path == NULL || path[1] == '\0') {
        return;
    }
    path++;
    const char *end = strpbrk(path, "?#");
    size_t length = end != NULL ? (size_t)(end - path) : strlen(path);
    if (length == 0U) {
        return;
    }
    if (length >= capacity) {
        length = capacity - 1U;
    }
    memcpy(output, path, length);
    output[length] = '\0';
}

static void virtual_media_label(const jetkvm_virtual_media_state_t *state,
                                char *label, size_t label_capacity,
                                const char **source)
{
    copy_text(label, label_capacity, "-- no media --");
    *source = "none";
    if (state == NULL || !state->image_mounted) {
        return;
    }
    if (state->source == JETKVM_VIRTUAL_MEDIA_SOURCE_HTTP) {
        *source = "url";
        url_basename(state->url, label, label_capacity);
    } else if (state->source == JETKVM_VIRTUAL_MEDIA_SOURCE_BUILTIN) {
        *source = "builtin";
        copy_text(label, label_capacity, state->filename);
    } else {
        *source = "storage";
        copy_text(label, label_capacity, state->filename);
    }
}

static void publish_virtual_media_state(void)
{
    jetkvm_virtual_media_state_t state = {0};
    bool have_state = jetkvm_virtual_media_get_state(&state) == ESP_OK;
    char label[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
    const char *source = "none";
    virtual_media_label(have_state ? &state : NULL, label, sizeof(label),
                        &source);
    cJSON *root = cJSON_CreateObject();
    if (root != NULL &&
        cJSON_AddStringToObject(root, "mounted_image", label) &&
        cJSON_AddStringToObject(root, "source", source)) {
        (void)publish_json_suffix("virtual_media/state", root);
    } else {
        cJSON_Delete(root);
    }
}

static cJSON *ha_device_info(const char *device_id)
{
    cJSON *device = cJSON_CreateObject();
    cJSON *identifiers = cJSON_CreateArray();
    char name[JETKVM_DEVICE_ID_CAPACITY + 16U];
    char version[96];
    char ip_address[16];
    char hostname[64];
    network_snapshot(ip_address, hostname);
    (void)snprintf(name, sizeof(name), "JetKVM %s", device_id);
    const esp_app_desc_t *description = esp_app_get_description();
    (void)snprintf(version, sizeof(version), "App %s",
                   description != NULL ? description->version : "unknown");
    if (device == NULL || identifiers == NULL ||
        !cJSON_AddItemToArray(identifiers, cJSON_CreateString(device_id)) ||
        !cJSON_AddItemToObject(device, "identifiers", identifiers) ||
        !cJSON_AddStringToObject(device, "name", name) ||
        !cJSON_AddStringToObject(device, "manufacturer", "JetKVM") ||
        !cJSON_AddStringToObject(device, "model", "JetKVM ESP32-P4") ||
        !cJSON_AddStringToObject(device, "sw_version", version) ||
        !cJSON_AddStringToObject(device, "serial_number", device_id)) {
        if (device == NULL ||
            cJSON_GetObjectItemCaseSensitive(device, "identifiers") == NULL) {
            cJSON_Delete(identifiers);
        }
        cJSON_Delete(device);
        return NULL;
    }
    if (ip_address[0] != '\0') {
        char url[32];
        int length = snprintf(url, sizeof(url), "http://%s", ip_address);
        if (length <= 0 || (size_t)length >= sizeof(url) ||
            !cJSON_AddStringToObject(device, "configuration_url", url)) {
            cJSON_Delete(device);
            return NULL;
        }
    }
    return device;
}

static bool add_optional_string(cJSON *object, const char *name,
                                const char *value)
{
    return value == NULL || value[0] == '\0' ||
           cJSON_AddStringToObject(object, name, value) != NULL;
}

static cJSON *new_discovery_payload(
    const char *component, const char *object_id, const char *name,
    const char *state_suffix, const char *command_suffix,
    const char *value_template, const char *device_id,
    const jetkvm_mqtt_config_t *config)
{
    char unique_id[JETKVM_DEVICE_ID_CAPACITY + 64U];
    char entity_id[JETKVM_DEVICE_ID_CAPACITY + 80U];
    char availability[MQTT_TOPIC_CAPACITY];
    char state_topic[MQTT_TOPIC_CAPACITY];
    char command_topic[MQTT_TOPIC_CAPACITY];
    if (!topic_from_snapshot(config, device_id, "status", availability,
                             sizeof(availability)) ||
        (state_suffix != NULL &&
         !topic_from_snapshot(config, device_id, state_suffix, state_topic,
                              sizeof(state_topic))) ||
        (command_suffix != NULL &&
         !topic_from_snapshot(config, device_id, command_suffix,
                              command_topic, sizeof(command_topic)))) {
        return NULL;
    }
    (void)snprintf(unique_id, sizeof(unique_id), "jetkvm_%s_%s", device_id,
                   object_id);
    (void)snprintf(entity_id, sizeof(entity_id), "%s.jetkvm_%s_%s",
                   component, device_id, object_id);
    cJSON *root = cJSON_CreateObject();
    cJSON *device = ha_device_info(device_id);
    if (root == NULL || device == NULL ||
        !cJSON_AddStringToObject(root, "name", name) ||
        !cJSON_AddStringToObject(root, "unique_id", unique_id) ||
        (state_suffix != NULL &&
         !cJSON_AddStringToObject(root, "state_topic", state_topic)) ||
        (command_suffix != NULL &&
         !cJSON_AddStringToObject(root, "command_topic", command_topic)) ||
        !add_optional_string(root, "value_template", value_template) ||
        !cJSON_AddStringToObject(root, "availability_topic", availability) ||
        !cJSON_AddStringToObject(
            root, "availability_template",
            "{{ 'online' if value_json.online else 'offline' }}") ||
        !cJSON_AddItemToObject(root, "device", device) ||
        !cJSON_AddStringToObject(root, "default_entity_id", entity_id)) {
        if (root == NULL ||
            cJSON_GetObjectItemCaseSensitive(root, "device") == NULL) {
            cJSON_Delete(device);
        }
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool publish_discovery_root(const char *component,
                                   const char *object_id, cJSON *root,
                                   const char *device_id)
{
    if (root == NULL) {
        return false;
    }
    char topic[MQTT_TOPIC_CAPACITY];
    int length = snprintf(topic, sizeof(topic),
                          "homeassistant/%s/jetkvm_%s/%s/config",
                          component, device_id, object_id);
    char *rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (length <= 0 || (size_t)length >= sizeof(topic) || rendered == NULL) {
        cJSON_free(rendered);
        return false;
    }
    bool published = publish_current_topic(topic, rendered, strlen(rendered),
                                           true);
    cJSON_free(rendered);
    return published;
}

static void remove_discovery(const char *component, const char *object_id,
                             const char *device_id)
{
    char topic[MQTT_TOPIC_CAPACITY];
    int length = snprintf(topic, sizeof(topic),
                          "homeassistant/%s/jetkvm_%s/%s/config",
                          component, device_id, object_id);
    if (length > 0 && (size_t)length < sizeof(topic)) {
        (void)publish_current_topic(topic, "", 0U, true);
    }
}

static void publish_general_discovery(const jetkvm_mqtt_config_t *config,
                                      const char *device_id)
{
    for (size_t i = 0U;
         i < sizeof(s_general_discovery) / sizeof(s_general_discovery[0]);
         ++i) {
        const ha_entity_t *entity = &s_general_discovery[i];
        cJSON *root = new_discovery_payload(
            entity->component, entity->object_id, entity->name,
            entity->state_suffix, NULL, entity->value_template, device_id,
            config);
        if (root == NULL ||
            !add_optional_string(root, "device_class", entity->device_class) ||
            !add_optional_string(root, "unit_of_measurement", entity->unit) ||
            !add_optional_string(root, "state_class", entity->state_class) ||
            !add_optional_string(root, "icon", entity->icon) ||
            !add_optional_string(root, "entity_category",
                                 entity->entity_category) ||
            (entity->enabled_by_default >= 0 &&
             !cJSON_AddBoolToObject(root, "enabled_by_default",
                                    entity->enabled_by_default != 0))) {
            cJSON_Delete(root);
            continue;
        }
        (void)publish_discovery_root(entity->component, entity->object_id,
                                     root, device_id);
    }
}

/* The option of a mounted image that is not a file on the card, in `label`,
 * or false when there is none. */
static bool mounted_image_option(char *label, size_t capacity)
{
    jetkvm_virtual_media_state_t state = {0};
    if (jetkvm_virtual_media_get_state(&state) != ESP_OK ||
        !state.image_mounted ||
        state.source == JETKVM_VIRTUAL_MEDIA_SOURCE_STORAGE) {
        return false;
    }
    const char *source = NULL;
    virtual_media_label(&state, label, capacity, &source);
    return true;
}

static bool file_list_has(const jetkvm_virtual_media_file_list_t *files,
                          const char *filename)
{
    for (size_t i = 0U; i < files->count; ++i) {
        if (strcmp(files->files[i].filename, filename) == 0) {
            return true;
        }
    }
    return false;
}

static void add_virtual_media_options(cJSON *root)
{
    cJSON *options = cJSON_CreateArray();
    if (options == NULL ||
        !cJSON_AddItemToArray(options, cJSON_CreateString("-- no media --"))) {
        cJSON_Delete(options);
        return;
    }
    jetkvm_virtual_media_file_list_t *files =
        jetkvm_memory_policy_control_alloc(sizeof(*files));
    bool listed = false;
    if (files != NULL) {
        memset(files, 0, sizeof(*files));
        listed = jetkvm_virtual_media_list_files(files) == ESP_OK;
        for (size_t i = 0U; listed && i < files->count; ++i) {
            cJSON *name = cJSON_CreateString(files->files[i].filename);
            if (name == NULL || !cJSON_AddItemToArray(options, name)) {
                cJSON_Delete(name);
                break;
            }
        }
    }
    /* A mounted image that is not a file on the card is an option too, so
     * that the select can show it, unless a card file has its name: that
     * option selects the card file. */
    char label[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
    if (mounted_image_option(label, sizeof(label)) &&
        !(listed && file_list_has(files, label))) {
        cJSON *name = cJSON_CreateString(label);
        if (name != NULL && !cJSON_AddItemToArray(options, name)) {
            cJSON_Delete(name);
        }
    }
    jetkvm_memory_policy_control_free(files);
    if (!cJSON_AddItemToObject(root, "options", options)) {
        cJSON_Delete(options);
    }
}

/* The Mini has no updater and no ATX/DC extension hardware, and it does not
 * measure its CPU load or temperature. */
static const struct {
    const char *component;
    const char *object_id;
} s_unsupported_discovery[] = {
    {"update", "firmware"},
    {"binary_sensor", "power_led"}, {"binary_sensor", "hdd_led"},
    {"button", "atx_power_short"}, {"button", "atx_power_long"},
    {"button", "atx_reset"}, {"sensor", "voltage"},
    {"sensor", "current"}, {"sensor", "power"},
    {"switch", "dc_power"}, {"binary_sensor", "dc_power"},
    {"select", "dc_restore"}, {"sensor", "dc_restore"},
    {"sensor", "cpu_load"}, {"sensor", "temperature"},
};

/* Clears the retained entities that another firmware left under the same
 * identity. */
static void remove_unsupported_discovery(const char *device_id)
{
    for (size_t i = 0U; i < sizeof(s_unsupported_discovery) /
                                sizeof(s_unsupported_discovery[0]);
         ++i) {
        remove_discovery(s_unsupported_discovery[i].component,
                         s_unsupported_discovery[i].object_id, device_id);
    }
}

static void publish_action_discovery(const jetkvm_mqtt_config_t *config,
                                     const char *device_id)
{
    const bool actions = config->enable_actions;
    const char *vm_component = actions ? "select" : "sensor";
    remove_discovery(actions ? "sensor" : "select", "virtual_media",
                     device_id);
    cJSON *vm = new_discovery_payload(
        vm_component, "virtual_media", "Virtual Media",
        "virtual_media/state", actions ? "virtual_media/set" : NULL,
        "{{ value_json.mounted_image }}", device_id, config);
    char vm_state_topic[MQTT_TOPIC_CAPACITY];
    if (vm != NULL && topic_from_snapshot(
            config, device_id, "virtual_media/state", vm_state_topic,
            sizeof(vm_state_topic)) &&
        cJSON_AddStringToObject(vm, "icon", "mdi:disc") &&
        cJSON_AddStringToObject(vm, "json_attributes_topic",
                               vm_state_topic) &&
        cJSON_AddStringToObject(
            vm, "json_attributes_template",
            "{{ {'source': value_json.source} | tojson }}")) {
        if (actions) {
            add_virtual_media_options(vm);
        }
        (void)publish_discovery_root(vm_component, "virtual_media", vm,
                                     device_id);
    } else {
        cJSON_Delete(vm);
    }

    const char *jiggler_component = actions ? "switch" : "binary_sensor";
    remove_discovery(actions ? "binary_sensor" : "switch", "jiggler",
                     device_id);
    cJSON *jiggler = new_discovery_payload(
        jiggler_component, "jiggler", "Mouse Jiggler", "jiggler/state",
        actions ? "jiggler/set" : NULL,
        "{{ 'ON' if value_json.enabled else 'OFF' }}", device_id, config);
    if (jiggler != NULL &&
        cJSON_AddStringToObject(jiggler, "icon", "mdi:mouse") &&
        (!actions ||
         (cJSON_AddStringToObject(jiggler, "payload_on", "ON") &&
          cJSON_AddStringToObject(jiggler, "payload_off", "OFF")))) {
        (void)publish_discovery_root(jiggler_component, "jiggler", jiggler,
                                     device_id);
    } else {
        cJSON_Delete(jiggler);
    }

    if (actions) {
        cJSON *reboot = new_discovery_payload(
            "button", "reboot", "Reboot", NULL, "reboot/set", NULL,
            device_id, config);
        if (reboot != NULL &&
            cJSON_AddStringToObject(reboot, "payload_press", "PRESS") &&
            cJSON_AddStringToObject(reboot, "device_class", "restart") &&
            cJSON_AddStringToObject(reboot, "icon", "mdi:restart") &&
            cJSON_AddStringToObject(reboot, "entity_category", "config")) {
            (void)publish_discovery_root("button", "reboot", reboot,
                                         device_id);
        } else {
            cJSON_Delete(reboot);
        }
    } else {
        remove_discovery("button", "reboot", device_id);
    }
    remove_unsupported_discovery(device_id);
}

static void publish_ha_discovery(void)
{
    jetkvm_mqtt_config_t config;
    char device_id[JETKVM_DEVICE_ID_CAPACITY];
    if (!config_snapshot(&config, device_id, sizeof(device_id)) ||
        !config.enable_ha_discovery) {
        jetkvm_mqtt_secure_zero(&config, sizeof(config));
        return;
    }
    publish_general_discovery(&config, device_id);
    publish_action_discovery(&config, device_id);
    jetkvm_mqtt_secure_zero(&config, sizeof(config));
}

static void publish_all_states(void)
{
    static const char online[] = "{\"online\":true}";
    (void)publish_suffix("status", online, sizeof(online) - 1U);
    publish_video_state(false);
    publish_cloud_state();
    publish_sessions_state();
    publish_usb_state();
    publish_jiggler_state();
    publish_network_state();
    publish_system_state();
    publish_virtual_media_state();
}

static void format_mqtt_error(const esp_mqtt_event_t *event,
                              char *output, size_t capacity)
{
    if (output == NULL || capacity == 0U) {
        return;
    }
    output[0] = '\0';
    if (event == NULL || event->error_handle == NULL) {
        copy_text(output, capacity, "MQTT connection failed");
        return;
    }
    const esp_mqtt_error_codes_t *error = event->error_handle;
    if (error->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        switch (error->connect_return_code) {
        case MQTT_CONNECTION_REFUSE_BAD_USERNAME:
            copy_text(output, capacity, "bad user name or password");
            return;
        case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:
            copy_text(output, capacity, "not authorized");
            return;
        case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE:
            copy_text(output, capacity, "connection refused: server unavailable");
            return;
        case MQTT_CONNECTION_REFUSE_ID_REJECTED:
            copy_text(output, capacity, "connection refused: client ID rejected");
            return;
        default:
            copy_text(output, capacity, "connection refused");
            return;
        }
    }
    if (error->esp_transport_sock_errno != 0) {
        (void)snprintf(output, capacity, "%s",
                       strerror(error->esp_transport_sock_errno));
        return;
    }
    if (error->esp_tls_last_esp_err != ESP_OK ||
        error->esp_tls_stack_err != 0) {
        (void)snprintf(output, capacity, "TLS connection failed (0x%x/0x%x)",
                       (unsigned)error->esp_tls_last_esp_err,
                       (unsigned)error->esp_tls_stack_err);
        return;
    }
    copy_text(output, capacity, "MQTT connection failed");
}

static bool topic_equals(const esp_mqtt_event_t *event, const char *suffix)
{
    char expected[MQTT_TOPIC_CAPACITY];
    return event != NULL && event->topic != NULL && event->topic_len >= 0 &&
           current_topic(suffix, expected, sizeof(expected)) &&
           strlen(expected) == (size_t)event->topic_len &&
           memcmp(event->topic, expected, (size_t)event->topic_len) == 0;
}

static bool actions_allowed(void)
{
    if (!runtime_ready()) {
        return false;
    }
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    bool allowed = s_mqtt.config.enable_actions;
    xSemaphoreGive(s_mqtt.lock);
    return allowed;
}

static bool copy_trimmed_payload(const esp_mqtt_event_t *event,
                                 char *output, size_t capacity)
{
    if (event == NULL || output == NULL || capacity == 0U ||
        event->data == NULL || event->data_len < 0 ||
        event->total_data_len != event->data_len ||
        event->current_data_offset != 0 ||
        (size_t)event->data_len >= capacity) {
        return false;
    }
    size_t begin = 0U;
    size_t end = (size_t)event->data_len;
    while (begin < end &&
           (event->data[begin] == ' ' || event->data[begin] == '\t' ||
            event->data[begin] == '\r' || event->data[begin] == '\n')) {
        begin++;
    }
    while (end > begin &&
           (event->data[end - 1U] == ' ' ||
            event->data[end - 1U] == '\t' ||
            event->data[end - 1U] == '\r' ||
            event->data[end - 1U] == '\n')) {
        end--;
    }
    size_t length = end - begin;
    memcpy(output, event->data + begin, length);
    output[length] = '\0';
    return true;
}

typedef struct {
    bool enabled;
    esp_err_t error;
    StaticSemaphore_t done_storage;
    SemaphoreHandle_t done;
} mqtt_jiggler_job_t;

static void mqtt_jiggler_task(void *argument)
{
    mqtt_jiggler_job_t *job = argument;
    job->error = jetkvm_hid_profiles_set_jiggler_state(job->enabled);
    (void)xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

/* The jiggler's state is stored in NVS. A flash write runs with the cache
 * disabled and needs a stack in internal RAM, and the client task's stack is
 * in PSRAM, so the write runs on a task of its own, which lives for the
 * write. The client task waits for it: a command is applied before the next
 * one is read, as when the client task stored the state itself. */
static esp_err_t set_jiggler_state(bool enabled)
{
    mqtt_jiggler_job_t job = {.enabled = enabled, .error = ESP_FAIL};
    job.done = xSemaphoreCreateBinaryStatic(&job.done_storage);
    if (xTaskCreate(mqtt_jiggler_task, "mqtt_jiggler",
                    MQTT_JIGGLER_TASK_STACK, &job, uxTaskPriorityGet(NULL),
                    NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    (void)xSemaphoreTake(job.done, portMAX_DELAY);
    return job.error;
}

static void handle_jiggler_command(const esp_mqtt_event_t *event)
{
    char payload[16];
    bool accepted = actions_allowed() &&
                    copy_trimmed_payload(event, payload, sizeof(payload));
    esp_err_t error = ESP_ERR_INVALID_ARG;
    if (accepted && strcasecmp(payload, "ON") == 0) {
        error = set_jiggler_state(true);
    } else if (accepted && strcasecmp(payload, "OFF") == 0) {
        error = set_jiggler_state(false);
    } else {
        accepted = false;
    }
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    if (!accepted || error != ESP_OK) {
        s_mqtt.status.commands_rejected++;
    }
    xSemaphoreGive(s_mqtt.lock);
    if (accepted && error == ESP_OK) {
        publish_jiggler_state();
    }
}

static bool valid_virtual_media_filename(const char *filename)
{
    return filename != NULL && filename[0] != '\0' &&
           strstr(filename, "..") == NULL && strchr(filename, '/') == NULL &&
           strchr(filename, '\\') == NULL;
}

/* Whether `option` is the option add_virtual_media_options() adds for a
 * mounted image that is not a file on the card. A card file of that name
 * takes the option. */
static bool option_is_mounted_image(const char *option)
{
    char label[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
    if (!mounted_image_option(label, sizeof(label)) ||
        strcmp(label, option) != 0) {
        return false;
    }
    jetkvm_virtual_media_file_list_t *files =
        jetkvm_memory_policy_control_alloc(sizeof(*files));
    if (files == NULL) {
        return true;
    }
    memset(files, 0, sizeof(*files));
    const bool on_card = jetkvm_virtual_media_list_files(files) == ESP_OK &&
                         file_list_has(files, option);
    jetkvm_memory_policy_control_free(files);
    return !on_card;
}

static void handle_virtual_media_command(const esp_mqtt_event_t *event)
{
    char payload[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
    bool accepted = actions_allowed() &&
                    copy_trimmed_payload(event, payload, sizeof(payload));
    esp_err_t error = ESP_ERR_INVALID_ARG;
    if (accepted && strcmp(payload, "-- no media --") == 0) {
        error = jetkvm_virtual_media_unmount();
    } else if (accepted && option_is_mounted_image(payload)) {
        /* No file on the card has that image, so it stays mounted. */
        error = ESP_OK;
    } else if (accepted && valid_virtual_media_filename(payload)) {
        /* The entity replaces a mounted image, so that image is taken out
         * first; without one the unmount changes nothing. The mount is the
         * one of the RPC mountWithStorage. */
        error = jetkvm_virtual_media_unmount();
        if (error == ESP_OK &&
            !webrtc_transport_mount_storage_image(payload, false)) {
            error = ESP_FAIL;
        }
    } else {
        accepted = false;
    }
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    if (!accepted || error != ESP_OK) {
        s_mqtt.status.commands_rejected++;
    }
    xSemaphoreGive(s_mqtt.lock);
    if (accepted && error == ESP_OK) {
        publish_virtual_media_state();
        publish_ha_discovery();
    }
    jetkvm_mqtt_secure_zero(payload, sizeof(payload));
}

static void mqtt_reboot_task(void *argument)
{
    (void)argument;
    static const char offline[] = "{\"online\":false}";
    (void)publish_suffix("status", offline, sizeof(offline) - 1U);
    vTaskDelay(pdMS_TO_TICKS(300U));
    esp_restart();
}

static void handle_reboot_command(void)
{
    if (!actions_allowed()) {
        xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
        s_mqtt.status.commands_rejected++;
        xSemaphoreGive(s_mqtt.lock);
        return;
    }
    BaseType_t created = xTaskCreate(
        mqtt_reboot_task, "mqtt_reboot", 2048U, NULL,
        tskIDLE_PRIORITY + 1U, NULL);
    if (created != pdPASS) {
        xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
        s_mqtt.status.commands_rejected++;
        xSemaphoreGive(s_mqtt.lock);
    }
}

static void subscribe_commands(esp_mqtt_client_handle_t client)
{
    char jiggler[MQTT_TOPIC_CAPACITY];
    char reboot[MQTT_TOPIC_CAPACITY];
    char virtual_media[MQTT_TOPIC_CAPACITY];
    if (!current_topic("jiggler/set", jiggler, sizeof(jiggler)) ||
        !current_topic("reboot/set", reboot, sizeof(reboot)) ||
        !current_topic("virtual_media/set", virtual_media,
                       sizeof(virtual_media))) {
        return;
    }
    const esp_mqtt_topic_t topics[] = {
        {.filter = jiggler, .qos = 1},
        {.filter = reboot, .qos = 1},
        {.filter = virtual_media, .qos = 1},
    };
    (void)esp_mqtt_client_subscribe_multiple(
        client, topics, (int)(sizeof(topics) / sizeof(topics[0])));
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_t *event = event_data;
    if (event == NULL || !runtime_ready()) {
        return;
    }
    bool current = false;
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    current = event->client == s_mqtt.client;
    xSemaphoreGive(s_mqtt.lock);
    if (!current) {
        return;
    }

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
        s_mqtt.status.connected = true;
        s_mqtt.status.connecting = false;
        s_mqtt.status.connections++;
        s_mqtt.status.error[0] = '\0';
        release_tls_locked();
        s_mqtt.video_payload[0] = '\0';
        s_mqtt.last_status_publish = xTaskGetTickCount();
        xSemaphoreGive(s_mqtt.lock);
        ESP_LOGI(TAG, "connected to MQTT broker");
        subscribe_commands(event->client);
        publish_all_states();
        publish_ha_discovery();
        break;
    case MQTT_EVENT_DISCONNECTED:
        xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
        if (s_mqtt.status.connected) {
            s_mqtt.status.disconnections++;
        }
        s_mqtt.status.connected = false;
        s_mqtt.status.connecting = false;
        if (s_mqtt.status.error[0] == '\0') {
            set_error_locked("MQTT connection lost");
        }
        release_tls_locked();
        schedule_reconnect_locked();
        xSemaphoreGive(s_mqtt.lock);
        break;
    case MQTT_EVENT_ERROR: {
        char error[JETKVM_MQTT_ERROR_CAPACITY];
        format_mqtt_error(event, error, sizeof(error));
        xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
        s_mqtt.status.connected = false;
        s_mqtt.status.connecting = false;
        set_error_locked(error);
        release_tls_locked();
        schedule_reconnect_locked();
        xSemaphoreGive(s_mqtt.lock);
        ESP_LOGW(TAG, "MQTT connection error: %s", error);
        break;
    }
    case MQTT_EVENT_DATA:
        xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
        s_mqtt.status.commands_received++;
        xSemaphoreGive(s_mqtt.lock);
        if (topic_equals(event, "jiggler/set")) {
            handle_jiggler_command(event);
        } else if (topic_equals(event, "virtual_media/set")) {
            handle_virtual_media_command(event);
        } else if (topic_equals(event, "reboot/set")) {
            handle_reboot_command();
        }
        break;
    default:
        break;
    }
}

static void test_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)base;
    mqtt_test_context_t *test = handler_args;
    esp_mqtt_event_t *event = event_data;
    if (test == NULL || test->done == NULL) {
        return;
    }
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        test->success = true;
        test->error[0] = '\0';
        (void)xSemaphoreGive(test->done);
        break;
    case MQTT_EVENT_ERROR:
        format_mqtt_error(event, test->error, sizeof(test->error));
        (void)xSemaphoreGive(test->done);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (!test->success && test->error[0] == '\0') {
            copy_text(test->error, sizeof(test->error),
                      "MQTT connection failed");
        }
        (void)xSemaphoreGive(test->done);
        break;
    default:
        break;
    }
}

static esp_err_t mqtt_tls_attach_insecure(void *conf)
{
    if (conf == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* ESP-TLS requires an explicit verification policy. Its bundle hook runs
     * after setting VERIFY_REQUIRED, so override only this MQTT connection's
     * policy when the user explicitly requests tls_insecure. Do not enable
     * CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY for unrelated TLS clients. */
    mbedtls_ssl_conf_authmode((mbedtls_ssl_config *)conf, MBEDTLS_SSL_VERIFY_NONE);
    return ESP_OK;
}

static bool build_client_config(
    const jetkvm_mqtt_config_t *config, const char *device_id, bool test,
    char *uri, size_t uri_capacity,
    char *client_id, size_t client_id_capacity,
    char *will_topic, size_t will_topic_capacity,
    esp_mqtt_client_config_t *mqtt_config)
{
    if (config == NULL || device_id == NULL || uri == NULL ||
        client_id == NULL || will_topic == NULL || mqtt_config == NULL) {
        return false;
    }
    int32_t port = config->port == 0 ? JETKVM_MQTT_DEFAULT_PORT :
                                      config->port;
    int uri_length = snprintf(uri, uri_capacity, "%s://%s:%" PRId32,
                              config->use_tls ? "mqtts" : "mqtt",
                              config->broker, port);
    int id_length = snprintf(client_id, client_id_capacity,
                             test ? "jetkvm-%s-test" : "jetkvm-%s",
                             device_id);
    if (uri_length <= 0 || (size_t)uri_length >= uri_capacity ||
        id_length <= 0 || (size_t)id_length >= client_id_capacity ||
        (!test && !topic_from_snapshot(config, device_id, "status",
                                       will_topic, will_topic_capacity))) {
        return false;
    }
    *mqtt_config = (esp_mqtt_client_config_t) {
        .broker = {
            .address = {.uri = uri},
            .verification = {
                .crt_bundle_attach = !config->use_tls ? NULL :
                    (config->tls_insecure ? mqtt_tls_attach_insecure :
                                           esp_crt_bundle_attach),
                .skip_cert_common_name_check =
                    config->use_tls && config->tls_insecure,
            },
        },
        .credentials = {
            .username = config->username,
            .client_id = client_id,
            .authentication = {.password = config->password},
        },
        .session = {
            .last_will = {
                .topic = test ? NULL : will_topic,
                .msg = test ? NULL : "{\"online\":false}",
                .qos = 1,
                .retain = 1,
            },
            .disable_clean_session = !test,
            .keepalive = 30,
            .protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        },
        .network = {
            .reconnect_timeout_ms = MQTT_RECONNECT_INTERVAL_MS,
            .timeout_ms = test ? MQTT_TEST_TIMEOUT_MS : 10000,
            .disable_auto_reconnect = true,
        },
        .task = {
            .priority = 4,
            .stack_size = MQTT_CLIENT_TASK_STACK,
        },
        .buffer = {
            .size = MQTT_CLIENT_BUFFER_BYTES,
            .out_size = MQTT_CLIENT_BUFFER_BYTES,
        },
        .outbox = {.limit = MQTT_OUTBOX_LIMIT_BYTES},
    };
    return true;
}

static bool wait_for_publish_acks(esp_mqtt_client_handle_t client)
{
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(MQTT_PUBLISH_DRAIN_TIMEOUT_MS);
    while (esp_mqtt_client_get_outbox_size(client) > 0) {
        if (xTaskGetTickCount() - started >= timeout) {
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

static void destroy_client(esp_mqtt_client_handle_t client,
                           bool graceful_disconnect)
{
    if (client == NULL) {
        return;
    }
    if (graceful_disconnect) {
        /* A successful publish queues QoS 1 data; only PUBACK proves that
         * the broker applied retained-state tombstones. Closing immediately
         * can discard the last packets on the C5 Wi-Fi transport. Bound the
         * drain so an unreachable broker cannot block reconfiguration. */
        if (!wait_for_publish_acks(client)) {
            ESP_LOGW(TAG, "MQTT publish acknowledgements timed out before disconnect");
        }
        (void)esp_mqtt_client_disconnect(client);
    }
    (void)esp_mqtt_client_stop(client);
    (void)esp_mqtt_client_destroy(client);
}

static esp_err_t start_current_client(void)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    jetkvm_mqtt_config_t config;
    char device_id[JETKVM_DEVICE_ID_CAPACITY];
    uint32_t generation = 0U;
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    bool should_start = s_mqtt.status.service_started &&
                        s_mqtt.config.enabled && s_mqtt.client == NULL &&
                        !s_mqtt.reconfiguring;
    config = s_mqtt.config;
    copy_text(device_id, sizeof(device_id), s_mqtt.device_id);
    generation = s_mqtt.status.generation;
    xSemaphoreGive(s_mqtt.lock);
    if (!should_start) {
        jetkvm_mqtt_secure_zero(&config, sizeof(config));
        return ESP_OK;
    }

    jetkvm_memory_policy_reservation_t tls = {0};
    if (config.use_tls &&
        !jetkvm_memory_policy_reserve_tls("mqtt-connect", &tls)) {
        xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
        set_error_locked("insufficient memory for MQTT TLS connection");
        schedule_reconnect_locked();
        xSemaphoreGive(s_mqtt.lock);
        jetkvm_mqtt_secure_zero(&config, sizeof(config));
        return ESP_ERR_NO_MEM;
    }
    char uri[MQTT_URI_CAPACITY];
    char client_id[MQTT_CLIENT_ID_CAPACITY];
    char will_topic[MQTT_TOPIC_CAPACITY];
    esp_mqtt_client_config_t mqtt_config;
    if (!build_client_config(&config, device_id, false, uri, sizeof(uri),
                             client_id, sizeof(client_id), will_topic,
                             sizeof(will_topic), &mqtt_config)) {
        jetkvm_memory_policy_release(&tls);
        record_client_start_failure(
            generation, "invalid MQTT connection settings");
        jetkvm_mqtt_secure_zero(&config, sizeof(config));
        return ESP_ERR_INVALID_ARG;
    }
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_config);
    if (client == NULL) {
        jetkvm_memory_policy_release(&tls);
        record_client_start_failure(
            generation, "failed to initialize MQTT client");
        jetkvm_mqtt_secure_zero(&config, sizeof(config));
        return ESP_ERR_NO_MEM;
    }
    esp_err_t register_error = esp_mqtt_client_register_event(
        client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    if (register_error != ESP_OK) {
        (void)esp_mqtt_client_destroy(client);
        jetkvm_memory_policy_release(&tls);
        record_client_start_failure(
            generation, "failed to initialize MQTT client");
        jetkvm_mqtt_secure_zero(&config, sizeof(config));
        return register_error;
    }

    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    bool current = generation == s_mqtt.status.generation &&
                   s_mqtt.status.service_started && s_mqtt.config.enabled &&
                   s_mqtt.client == NULL && !s_mqtt.reconfiguring;
    if (current) {
        s_mqtt.client = client;
        s_mqtt.status.connecting = true;
        s_mqtt.status.connected = false;
        s_mqtt.tls_reservation = tls;
        memset(&tls, 0, sizeof(tls));
    }
    xSemaphoreGive(s_mqtt.lock);
    if (!current) {
        (void)esp_mqtt_client_destroy(client);
        jetkvm_memory_policy_release(&tls);
        jetkvm_mqtt_secure_zero(&config, sizeof(config));
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = esp_mqtt_client_start(client);
    if (error != ESP_OK) {
        xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
        if (s_mqtt.client == client) {
            s_mqtt.client = NULL;
            s_mqtt.status.connecting = false;
            release_tls_locked();
            set_error_locked("failed to start MQTT client");
            schedule_reconnect_locked();
        }
        xSemaphoreGive(s_mqtt.lock);
        (void)esp_mqtt_client_destroy(client);
    }
    jetkvm_mqtt_secure_zero(&config, sizeof(config));
    return error;
}

static void reconnect_current_client(void)
{
    jetkvm_memory_policy_reservation_t tls = {0};
    bool use_tls = false;
    esp_mqtt_client_handle_t client = NULL;
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    client = s_mqtt.client;
    use_tls = s_mqtt.config.use_tls;
    bool eligible = client != NULL && s_mqtt.status.service_started &&
                    s_mqtt.config.enabled && !s_mqtt.status.connected &&
                    !s_mqtt.status.connecting && !s_mqtt.reconfiguring;
    xSemaphoreGive(s_mqtt.lock);
    if (!eligible) {
        return;
    }
    if (use_tls &&
        !jetkvm_memory_policy_reserve_tls("mqtt-reconnect", &tls)) {
        xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
        set_error_locked("insufficient memory for MQTT TLS connection");
        schedule_reconnect_locked();
        xSemaphoreGive(s_mqtt.lock);
        return;
    }
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    eligible = client == s_mqtt.client && s_mqtt.status.service_started &&
               s_mqtt.config.enabled && !s_mqtt.status.connected &&
               !s_mqtt.status.connecting && !s_mqtt.reconfiguring;
    if (eligible) {
        s_mqtt.status.connecting = true;
        s_mqtt.status.reconnect_attempts++;
        s_mqtt.tls_reservation = tls;
        memset(&tls, 0, sizeof(tls));
        esp_err_t error = esp_mqtt_client_reconnect(client);
        if (error != ESP_OK) {
            s_mqtt.status.connecting = false;
            release_tls_locked();
            set_error_locked("failed to reconnect MQTT client");
            schedule_reconnect_locked();
        }
    }
    xSemaphoreGive(s_mqtt.lock);
    jetkvm_memory_policy_release(&tls);
}

static void mqtt_manager_task(void *argument)
{
    (void)argument;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000U));
        TickType_t now = xTaskGetTickCount();
        bool need_create = false;
        bool need_reconnect = false;
        bool need_publish = false;
        bool connected = false;
        xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
        bool enabled = s_mqtt.status.service_started &&
                       s_mqtt.config.enabled && !s_mqtt.reconfiguring;
        if (enabled && s_mqtt.client == NULL &&
            (int32_t)(now - s_mqtt.next_reconnect) >= 0) {
            need_create = true;
        } else if (enabled && s_mqtt.client != NULL &&
                   !s_mqtt.status.connected && !s_mqtt.status.connecting &&
                   (int32_t)(now - s_mqtt.next_reconnect) >= 0) {
            need_reconnect = true;
        }
        if (enabled && s_mqtt.status.connected &&
            (now - s_mqtt.last_status_publish) >=
                pdMS_TO_TICKS(MQTT_STATUS_INTERVAL_MS)) {
            s_mqtt.last_status_publish = now;
            need_publish = true;
        }
        connected = enabled && s_mqtt.status.connected;
        xSemaphoreGive(s_mqtt.lock);
        if (need_create) {
            (void)start_current_client();
        } else if (need_reconnect) {
            reconnect_current_client();
        }
        if (need_publish) {
            publish_all_states();
        } else if (connected) {
            /* A new video mode reaches the broker with this task's next
             * pass, within one second, and a lost signal with the first
             * pass after MQTT_NO_SIGNAL_DELAY_MS. */
            publish_video_state(true);
        }
    }
}

static void direct_publish(esp_mqtt_client_handle_t client,
                           const char *topic, const char *payload)
{
    if (client != NULL && topic != NULL && payload != NULL) {
        (void)esp_mqtt_client_publish(client, topic, payload,
                                      (int)strlen(payload), 1, 1);
    }
}

static void direct_clear_discovery(esp_mqtt_client_handle_t client,
                                   const char *device_id,
                                   const char *component,
                                   const char *object_id)
{
    char topic[MQTT_TOPIC_CAPACITY];
    int length = snprintf(topic, sizeof(topic),
                          "homeassistant/%s/jetkvm_%s/%s/config",
                          component, device_id, object_id);
    if (length > 0 && (size_t)length < sizeof(topic)) {
        direct_publish(client, topic, "");
    }
}

static void direct_remove_all_discovery(esp_mqtt_client_handle_t client,
                                        const char *device_id)
{
    for (size_t i = 0U;
         i < sizeof(s_general_discovery) / sizeof(s_general_discovery[0]);
         ++i) {
        direct_clear_discovery(client, device_id,
                               s_general_discovery[i].component,
                               s_general_discovery[i].object_id);
    }
    static const struct {
        const char *component;
        const char *object_id;
    } extra[] = {
        {"select", "virtual_media"}, {"sensor", "virtual_media"},
        {"switch", "jiggler"}, {"binary_sensor", "jiggler"},
        {"button", "reboot"},
    };
    for (size_t i = 0U; i < sizeof(extra) / sizeof(extra[0]); ++i) {
        direct_clear_discovery(client, device_id, extra[i].component,
                               extra[i].object_id);
    }
    for (size_t i = 0U; i < sizeof(s_unsupported_discovery) /
                                sizeof(s_unsupported_discovery[0]);
         ++i) {
        direct_clear_discovery(client, device_id,
                               s_unsupported_discovery[i].component,
                               s_unsupported_discovery[i].object_id);
    }
}

static void direct_cleanup_states(esp_mqtt_client_handle_t client,
                                  const jetkvm_mqtt_config_t *config,
                                  const char *device_id)
{
    static const char *suffixes[] = {
        "status", "video/state", "cloud/state", "sessions/state",
        "usb/state", "jiggler/state", "network/state", "system/state",
        "virtual_media/state", "update/state", "atx/state", "dc/state",
    };
    char topic[MQTT_TOPIC_CAPACITY];
    for (size_t i = 0U; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        if (topic_from_snapshot(config, device_id, suffixes[i], topic,
                                sizeof(topic))) {
            direct_publish(client, topic, "");
        }
    }
}

esp_err_t jetkvm_mqtt_init(void)
{
    if (runtime_ready()) {
        return s_mqtt.status.load_error;
    }
    memset(&s_mqtt, 0, sizeof(s_mqtt));
    s_mqtt.lock = xSemaphoreCreateMutexStatic(&s_mqtt.lock_storage);
    if (s_mqtt.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    jetkvm_mqtt_config_t config;
    esp_err_t load_error = jetkvm_mqtt_store_load(&config);
    if (load_error != ESP_OK) {
        jetkvm_mqtt_config_defaults(&config);
    }
    char device_id[JETKVM_DEVICE_ID_CAPACITY] = {0};
    esp_err_t identity_error =
        jetkvm_device_state_get_id(device_id, sizeof(device_id));
    if (identity_error != ESP_OK) {
        jetkvm_mqtt_secure_zero(&config, sizeof(config));
        return identity_error;
    }
    s_mqtt.config = config;
    copy_text(s_mqtt.device_id, sizeof(s_mqtt.device_id), device_id);
    s_mqtt.status = (jetkvm_mqtt_status_t) {
        .initialized = true,
        .recovered_defaults = load_error != ESP_OK,
        .load_error = load_error,
        .persistence_error = ESP_OK,
        .generation = 1U,
    };
    s_mqtt.next_reconnect = xTaskGetTickCount();
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        mqtt_manager_task, "mqtt_manager", MQTT_MANAGER_TASK_STACK, NULL,
        tskIDLE_PRIORITY + 1U, &s_mqtt.manager_task, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    jetkvm_mqtt_secure_zero(&config, sizeof(config));
    jetkvm_mqtt_secure_zero(device_id, sizeof(device_id));
    if (created != pdPASS) {
        s_mqtt.status.initialized = false;
        return ESP_ERR_NO_MEM;
    }
    return load_error;
}

bool jetkvm_mqtt_is_initialized(void)
{
    return runtime_ready();
}

esp_err_t jetkvm_mqtt_start(void)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    s_mqtt.status.service_started = true;
    s_mqtt.next_reconnect = xTaskGetTickCount();
    xSemaphoreGive(s_mqtt.lock);
    if (s_mqtt.manager_task != NULL) {
        xTaskNotifyGive(s_mqtt.manager_task);
    }
    return start_current_client();
}

esp_err_t jetkvm_mqtt_get_config(jetkvm_mqtt_config_t *config,
                                 bool mask_password)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    *config = s_mqtt.config;
    xSemaphoreGive(s_mqtt.lock);
    if (mask_password && config->password[0] != '\0') {
        memset(config->password, 0, sizeof(config->password));
        copy_text(config->password, sizeof(config->password),
                  JETKVM_MQTT_PASSWORD_MASK);
    }
    return ESP_OK;
}

esp_err_t jetkvm_mqtt_set_config(
    const jetkvm_mqtt_config_t *config,
    char *error, size_t error_capacity)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    jetkvm_mqtt_config_t candidate = *config;
    jetkvm_mqtt_config_t existing;
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    existing = s_mqtt.config;
    xSemaphoreGive(s_mqtt.lock);
    if (!jetkvm_mqtt_config_normalize(&candidate, &existing, error,
                                      error_capacity)) {
        jetkvm_mqtt_secure_zero(&candidate, sizeof(candidate));
        jetkvm_mqtt_secure_zero(&existing, sizeof(existing));
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t save_error = jetkvm_mqtt_store_save(&candidate);
    if (save_error != ESP_OK) {
        xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
        s_mqtt.status.persistence_error = save_error;
        xSemaphoreGive(s_mqtt.lock);
        copy_text(error, error_capacity, "failed to save MQTT settings");
        jetkvm_mqtt_secure_zero(&candidate, sizeof(candidate));
        jetkvm_mqtt_secure_zero(&existing, sizeof(existing));
        return save_error;
    }

    esp_mqtt_client_handle_t old_client = NULL;
    bool old_connected = false;
    bool service_started = false;
    jetkvm_memory_policy_reservation_t old_tls_reservation = {0};
    char device_id[JETKVM_DEVICE_ID_CAPACITY];
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    s_mqtt.reconfiguring = true;
    old_client = s_mqtt.client;
    old_connected = s_mqtt.status.connected;
    service_started = s_mqtt.status.service_started;
    copy_text(device_id, sizeof(device_id), s_mqtt.device_id);
    xSemaphoreGive(s_mqtt.lock);
    wait_for_publishes();

    if (old_client != NULL && old_connected) {
        if (existing.enabled && !candidate.enabled) {
            direct_remove_all_discovery(old_client, device_id);
            direct_cleanup_states(old_client, &existing, device_id);
        } else if (existing.enable_ha_discovery &&
                   !candidate.enable_ha_discovery) {
            direct_remove_all_discovery(old_client, device_id);
        }
        char old_status[MQTT_TOPIC_CAPACITY];
        if (topic_from_snapshot(&existing, device_id, "status", old_status,
                                sizeof(old_status))) {
            direct_publish(old_client, old_status, "{\"online\":false}");
        }
    }

    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    if (s_mqtt.client == old_client) {
        s_mqtt.client = NULL;
    }
    old_tls_reservation = s_mqtt.tls_reservation;
    memset(&s_mqtt.tls_reservation, 0, sizeof(s_mqtt.tls_reservation));
    s_mqtt.status.connected = false;
    s_mqtt.status.connecting = false;
    s_mqtt.config = candidate;
    s_mqtt.status.persistence_error = ESP_OK;
    s_mqtt.status.error[0] = '\0';
    s_mqtt.status.generation++;
    if (s_mqtt.status.generation == 0U) {
        s_mqtt.status.generation = 1U;
    }
    s_mqtt.next_reconnect = xTaskGetTickCount();
    xSemaphoreGive(s_mqtt.lock);

    destroy_client(old_client, old_connected);
    jetkvm_memory_policy_release(&old_tls_reservation);
    /* Keep the manager from starting another client with the same ID while
     * the old client's retained cleanup is still awaiting acknowledgements. */
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    s_mqtt.reconfiguring = false;
    xSemaphoreGive(s_mqtt.lock);
    if (s_mqtt.manager_task != NULL) {
        xTaskNotifyGive(s_mqtt.manager_task);
    }
    esp_err_t start_error = service_started && candidate.enabled ?
                                start_current_client() : ESP_OK;
    if (start_error != ESP_OK) {
        ESP_LOGW(TAG,
                 "saved MQTT settings; client start deferred after error: %s",
                 esp_err_to_name(start_error));
    }
    jetkvm_mqtt_secure_zero(&candidate, sizeof(candidate));
    jetkvm_mqtt_secure_zero(&existing, sizeof(existing));
    jetkvm_mqtt_secure_zero(device_id, sizeof(device_id));
    return ESP_OK;
}

esp_err_t jetkvm_mqtt_get_status(jetkvm_mqtt_status_t *status)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    *status = s_mqtt.status;
    xSemaphoreGive(s_mqtt.lock);
    return ESP_OK;
}

esp_err_t jetkvm_mqtt_test_connection(
    const jetkvm_mqtt_config_t *config,
    jetkvm_mqtt_test_result_t *result)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config == NULL || result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(result, 0, sizeof(*result));
    jetkvm_mqtt_config_t candidate = *config;
    jetkvm_mqtt_config_t existing;
    char device_id[JETKVM_DEVICE_ID_CAPACITY];
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    existing = s_mqtt.config;
    copy_text(device_id, sizeof(device_id), s_mqtt.device_id);
    xSemaphoreGive(s_mqtt.lock);
    if (memcmp(candidate.password, JETKVM_MQTT_PASSWORD_MASK,
               sizeof(JETKVM_MQTT_PASSWORD_MASK)) == 0) {
        memcpy(candidate.password, existing.password,
               sizeof(candidate.password));
        candidate.password[sizeof(candidate.password) - 1U] = '\0';
    }
    if (candidate.broker[0] == '\0') {
        copy_text(result->error, sizeof(result->error),
                  "broker address is required");
        goto done;
    }
    if (candidate.port < 0 || candidate.port > 65535) {
        copy_text(result->error, sizeof(result->error),
                  "port must be between 1 and 65535");
        goto done;
    }

    jetkvm_memory_policy_reservation_t tls = {0};
    if (candidate.use_tls &&
        !jetkvm_memory_policy_reserve_tls("mqtt-test", &tls)) {
        copy_text(result->error, sizeof(result->error),
                  "insufficient memory for MQTT TLS connection");
        goto done;
    }
    char uri[MQTT_URI_CAPACITY];
    char client_id[MQTT_CLIENT_ID_CAPACITY];
    char unused_will[MQTT_TOPIC_CAPACITY];
    esp_mqtt_client_config_t mqtt_config;
    if (!build_client_config(&candidate, device_id, true, uri, sizeof(uri),
                             client_id, sizeof(client_id), unused_will,
                             sizeof(unused_will), &mqtt_config)) {
        jetkvm_memory_policy_release(&tls);
        copy_text(result->error, sizeof(result->error),
                  "invalid MQTT connection settings");
        goto done;
    }
    mqtt_test_context_t test = {0};
    test.done = xSemaphoreCreateBinaryStatic(&test.done_storage);
    esp_mqtt_client_handle_t client = test.done != NULL ?
        esp_mqtt_client_init(&mqtt_config) : NULL;
    if (client == NULL ||
        esp_mqtt_client_register_event(client, MQTT_EVENT_ANY,
                                       test_event_handler, &test) != ESP_OK ||
        esp_mqtt_client_start(client) != ESP_OK) {
        copy_text(result->error, sizeof(result->error),
                  "failed to start MQTT connection test");
        if (client != NULL) {
            destroy_client(client, false);
        }
        jetkvm_memory_policy_release(&tls);
        goto done;
    }
    if (xSemaphoreTake(test.done, pdMS_TO_TICKS(MQTT_TEST_TIMEOUT_MS)) !=
        pdTRUE) {
        copy_text(test.error, sizeof(test.error), "connection timed out");
    }
    result->success = test.success;
    copy_text(result->error, sizeof(result->error), test.error);
    destroy_client(client, test.success);
    jetkvm_memory_policy_release(&tls);
    jetkvm_mqtt_secure_zero(&test, sizeof(test));

done:
    jetkvm_mqtt_secure_zero(&candidate, sizeof(candidate));
    jetkvm_mqtt_secure_zero(&existing, sizeof(existing));
    jetkvm_mqtt_secure_zero(device_id, sizeof(device_id));
    return ESP_OK;
}

esp_err_t jetkvm_mqtt_publish_now(void)
{
    if (!runtime_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
    bool connected = s_mqtt.status.connected;
    xSemaphoreGive(s_mqtt.lock);
    if (!connected) {
        return ESP_ERR_INVALID_STATE;
    }
    publish_all_states();
    return ESP_OK;
}

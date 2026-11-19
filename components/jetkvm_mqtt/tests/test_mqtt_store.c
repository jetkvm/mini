/* Loads MQTT records through jetkvm_mqtt_store.c from a one-blob NVS fake.
 * A record that firmware before enable_keyboard_macros stored (version 1)
 * must keep its settings and load with keyboard macros over MQTT off, as Go
 * loads a config file saved before the field existed. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "jetkvm_mqtt_internal.h"
#include "nvs.h"

static uint8_t s_blob[2048];
static size_t s_blob_size;

void jetkvm_mqtt_secure_zero(void *memory, size_t size)
{
    memset(memory, 0, size);
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    (void)open_mode;
    assert(strcmp(namespace_name, "jkvm_mqtt") == 0);
    *out_handle = 1U;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value,
                       size_t *length)
{
    (void)handle;
    assert(strcmp(key, "config") == 0);
    if (s_blob_size == 0U) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (*length < s_blob_size) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, s_blob, s_blob_size);
    *length = s_blob_size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length)
{
    (void)handle;
    assert(strcmp(key, "config") == 0 && length <= sizeof(s_blob));
    memcpy(s_blob, value, length);
    s_blob_size = length;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

/* The record layout of the firmware before enable_keyboard_macros. */
typedef struct {
    bool enabled;
    char broker[256];
    int32_t port;
    char username[128];
    char password[128];
    char base_topic[128];
    bool use_tls;
    bool tls_insecure;
    bool enable_ha_discovery;
    bool enable_actions;
    int32_t debounce_ms;
} old_config_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    old_config_t config;
    uint32_t crc32;
} old_record_t;

static uint32_t crc32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0U; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}

static void store_old_record(void)
{
    old_record_t record;
    memset(&record, 0, sizeof(record));
    record.magic = 0x54514d4aU;
    record.version = 1U;
    record.config.enabled = true;
    strcpy(record.config.broker, "192.168.50.1");
    record.config.port = 1884;
    strcpy(record.config.username, "kvm");
    strcpy(record.config.password, "secret");
    strcpy(record.config.base_topic, "lab");
    record.config.enable_ha_discovery = true;
    record.config.enable_actions = true;
    record.config.debounce_ms = 250;
    record.crc32 = crc32(&record, offsetof(old_record_t, crc32));
    memcpy(s_blob, &record, sizeof(record));
    s_blob_size = sizeof(record);
}

int main(void)
{
    jetkvm_mqtt_config_t config;
    jetkvm_mqtt_config_defaults(&config);
    assert(!config.enable_keyboard_macros);

    store_old_record();
    assert(jetkvm_mqtt_store_load(&config) == ESP_OK);
    assert(config.enabled);
    assert(strcmp(config.broker, "192.168.50.1") == 0);
    assert(config.port == 1884);
    assert(strcmp(config.username, "kvm") == 0);
    assert(strcmp(config.password, "secret") == 0);
    assert(strcmp(config.base_topic, "lab") == 0);
    assert(config.enable_ha_discovery);
    assert(config.enable_actions);
    assert(!config.enable_keyboard_macros);
    assert(config.debounce_ms == 250);

    /* A damaged old record is not taken for settings. */
    s_blob[8] ^= 0x01U;
    assert(jetkvm_mqtt_store_load(&config) == ESP_ERR_INVALID_CRC);
    assert(!config.enabled && config.enable_actions);

    store_old_record();
    assert(jetkvm_mqtt_store_load(&config) == ESP_OK);
    config.enable_keyboard_macros = true;
    assert(jetkvm_mqtt_store_save(&config) == ESP_OK);
    jetkvm_mqtt_config_t reloaded;
    assert(jetkvm_mqtt_store_load(&reloaded) == ESP_OK);
    assert(reloaded.enable_keyboard_macros);
    assert(reloaded.port == 1884 && reloaded.debounce_ms == 250);
    assert(strcmp(reloaded.password, "secret") == 0);

    puts("MQTT settings record migration: OK");
    return 0;
}

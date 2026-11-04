#include "jetkvm_wol.h"

#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define WOL_NAMESPACE "jkvm_wol"
#define WOL_KEY "devices"
#define WOL_MAGIC 0x4c4f574aU
#define WOL_VERSION 1U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    jetkvm_wol_device_t devices[JETKVM_WOL_MAX_DEVICES];
    uint32_t crc32;
} wol_record_t;

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static jetkvm_wol_device_list_t s_devices;
static bool s_initialized;

static uint32_t crc32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0U; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

static bool terminated(const char *value, size_t capacity)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

static bool valid_mac(const char *text)
{
    if (!terminated(text, JETKVM_WOL_MAC_CAPACITY) || strlen(text) != 17U) {
        return false;
    }
    for (size_t i = 0U; i < 17U; ++i) {
        if (i % 3U == 2U) {
            if (text[i] != ':') return false;
        } else if (!((text[i] >= '0' && text[i] <= '9') ||
                     (text[i] >= 'a' && text[i] <= 'f') ||
                     (text[i] >= 'A' && text[i] <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool valid_ipv4(const char *text)
{
    if (!terminated(text, JETKVM_WOL_BROADCAST_CAPACITY)) return false;
    if (text[0] == '\0') return true;
    unsigned parts = 0U;
    const char *cursor = text;
    while (*cursor != '\0' && parts < 4U) {
        unsigned value = 0U;
        unsigned digits = 0U;
        while (*cursor >= '0' && *cursor <= '9') {
            value = value * 10U + (unsigned)(*cursor - '0');
            cursor++;
            digits++;
            if (digits > 3U || value > 255U) return false;
        }
        if (digits == 0U) return false;
        parts++;
        if (parts < 4U) {
            if (*cursor != '.') return false;
            cursor++;
        }
    }
    return parts == 4U && *cursor == '\0';
}

static bool valid_list(const jetkvm_wol_device_list_t *list)
{
    if (list == NULL || list->count > JETKVM_WOL_MAX_DEVICES) return false;
    for (size_t i = 0U; i < list->count; ++i) {
        const jetkvm_wol_device_t *device = &list->devices[i];
        if (!terminated(device->name, sizeof(device->name)) ||
            device->name[0] == '\0' || !valid_mac(device->mac_address) ||
            !valid_ipv4(device->broadcast_ip)) return false;
    }
    return true;
}

static esp_err_t save(const jetkvm_wol_device_list_t *list)
{
    wol_record_t record = {.magic = WOL_MAGIC, .version = WOL_VERSION,
                           .count = (uint32_t)list->count};
    memcpy(record.devices, list->devices,
           list->count * sizeof(list->devices[0]));
    record.crc32 = crc32(&record, offsetof(wol_record_t, crc32));
    nvs_handle_t handle;
    esp_err_t error = nvs_open(WOL_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_blob(handle, WOL_KEY, &record, sizeof(record));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

esp_err_t jetkvm_wol_init(void)
{
    if (s_initialized) return ESP_OK;
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    memset(&s_devices, 0, sizeof(s_devices));
    wol_record_t record = {0};
    size_t size = sizeof(record);
    nvs_handle_t handle;
    esp_err_t error = nvs_open(WOL_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_OK) {
        error = nvs_get_blob(handle, WOL_KEY, &record, &size);
        nvs_close(handle);
    }
    if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    if (error == ESP_OK && size == sizeof(record) &&
        record.magic == WOL_MAGIC && record.version == WOL_VERSION &&
        record.count <= JETKVM_WOL_MAX_DEVICES &&
        record.crc32 == crc32(&record, offsetof(wol_record_t, crc32))) {
        s_devices.count = record.count;
        memcpy(s_devices.devices, record.devices,
               s_devices.count * sizeof(s_devices.devices[0]));
        if (!valid_list(&s_devices)) {
            memset(&s_devices, 0, sizeof(s_devices));
            error = ESP_ERR_INVALID_STATE;
        }
    } else if (error == ESP_OK && size != sizeof(record)) {
        error = ESP_ERR_INVALID_SIZE;
    } else if (error == ESP_OK && record.magic != 0U) {
        error = ESP_ERR_INVALID_CRC;
    }
    s_initialized = true;
    return error;
}

bool jetkvm_wol_is_initialized(void) { return s_initialized; }

esp_err_t jetkvm_wol_get_devices(jetkvm_wol_device_list_t *list)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (list == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *list = s_devices;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t jetkvm_wol_set_devices(const jetkvm_wol_device_list_t *list)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!valid_list(list)) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t error = save(list);
    if (error == ESP_OK) s_devices = *list;
    xSemaphoreGive(s_lock);
    return error;
}

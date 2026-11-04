#include "nvs.h"

#include <stdbool.h>
#include <string.h>

#include "mock_nvs.h"

#define MOCK_NVS_MAX_BLOB (20U * 1024U)

static uint8_t s_committed[MOCK_NVS_MAX_BLOB];
static size_t s_committed_size;
static bool s_namespace_exists;
static uint8_t s_staged[MOCK_NVS_MAX_BLOB];
static size_t s_staged_size;
static bool s_staged_valid;
static bool s_fail_next_commit;

void mock_nvs_reset(void)
{
    memset(s_committed, 0, sizeof(s_committed));
    memset(s_staged, 0, sizeof(s_staged));
    s_committed_size = 0U;
    s_staged_size = 0U;
    s_namespace_exists = false;
    s_staged_valid = false;
    s_fail_next_commit = false;
}

void mock_nvs_fail_next_commit(void) { s_fail_next_commit = true; }

void mock_nvs_corrupt_byte(size_t offset, uint8_t xor_mask)
{
    if (offset < s_committed_size) {
        s_committed[offset] ^= xor_mask;
    }
}

size_t mock_nvs_committed_size(void) { return s_committed_size; }

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    if (namespace_name == NULL || out_handle == NULL ||
        strcmp(namespace_name, "jkvm_hidprof") != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (open_mode == NVS_READONLY && !s_namespace_exists) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (open_mode == NVS_READWRITE) {
        s_namespace_exists = true;
    }
    *out_handle = 1U;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value,
                       size_t *length)
{
    (void)handle;
    if (key == NULL || strcmp(key, "profiles") != 0 || length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_committed_size == 0U) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (out_value == NULL) {
        *length = s_committed_size;
        return ESP_OK;
    }
    if (*length < s_committed_size) {
        *length = s_committed_size;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, s_committed, s_committed_size);
    *length = s_committed_size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length)
{
    (void)handle;
    if (key == NULL || strcmp(key, "profiles") != 0 || value == NULL ||
        length > sizeof(s_staged)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_staged, value, length);
    s_staged_size = length;
    s_staged_valid = true;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    if (key == NULL || strcmp(key, "profiles") != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_committed_size == 0U) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    s_staged_size = 0U;
    s_staged_valid = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    if (s_fail_next_commit) {
        s_fail_next_commit = false;
        s_staged_valid = false;
        return ESP_FAIL;
    }
    if (s_staged_valid) {
        if (s_staged_size > 0U) {
            memcpy(s_committed, s_staged, s_staged_size);
        }
        s_committed_size = s_staged_size;
        s_staged_valid = false;
    }
    return ESP_OK;
}

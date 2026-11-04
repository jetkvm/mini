#include "nvs.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "mock_nvs.h"

#define MOCK_NVS_MAX_BLOB 4096U

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

void mock_nvs_fail_next_commit(void)
{
    s_fail_next_commit = true;
}

void mock_nvs_corrupt_byte(size_t offset, uint8_t xor_mask)
{
    assert(offset < s_committed_size);
    s_committed[offset] ^= xor_mask;
}

size_t mock_nvs_committed_size(void)
{
    return s_committed_size;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    if (!namespace_name || !out_handle) {
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

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
    s_staged_valid = false;
    s_staged_size = 0U;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                       void *out_value, size_t *length)
{
    if (handle != 1U || !key || !length) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_committed_size) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (!out_value) {
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
    if (handle != 1U || !key || !value || length > sizeof(s_staged)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_staged, value, length);
    s_staged_size = length;
    s_staged_valid = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    if (handle != 1U || !s_staged_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_fail_next_commit) {
        s_fail_next_commit = false;
        return ESP_FAIL;
    }
    memcpy(s_committed, s_staged, s_staged_size);
    s_committed_size = s_staged_size;
    s_namespace_exists = true;
    return ESP_OK;
}

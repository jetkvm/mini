#include "mock_nvs.h"

#include <assert.h>
#include <string.h>

#include "nvs.h"

#define MOCK_HANDLE 1U
#define MOCK_CAPACITY 256U

typedef struct {
    bool present;
    size_t size;
    uint8_t data[MOCK_CAPACITY];
} mock_blob_t;

static mock_blob_t s_committed[2];
static mock_blob_t s_staged[2];
static bool s_namespace_exists;
static bool s_handle_open;
static nvs_open_mode_t s_open_mode;
static bool s_fail_open;
static bool s_fail_set;
static bool s_fail_commit;
static bool s_fail_readback;
static bool s_fail_get_now;
static bool s_corrupt_after_commit;
static size_t s_corrupt_offset;
static uint8_t s_corrupt_mask;
static int s_last_staged_slot;
static unsigned s_set_count;
static unsigned s_commit_count;
static char s_last_namespace[32];
static char s_last_set_key[16];

static int key_to_slot(const char *key)
{
    if (key && strcmp(key, "cfg_a") == 0) {
        return MOCK_USB_SLOT_A;
    }
    if (key && strcmp(key, "cfg_b") == 0) {
        return MOCK_USB_SLOT_B;
    }
    return -1;
}

void mock_nvs_reset(void)
{
    memset(s_committed, 0, sizeof(s_committed));
    memset(s_staged, 0, sizeof(s_staged));
    s_namespace_exists = false;
    s_handle_open = false;
    s_open_mode = NVS_READONLY;
    s_fail_open = false;
    s_fail_set = false;
    s_fail_commit = false;
    s_fail_readback = false;
    s_fail_get_now = false;
    s_corrupt_after_commit = false;
    s_corrupt_offset = 0U;
    s_corrupt_mask = 0U;
    s_last_staged_slot = -1;
    s_set_count = 0U;
    s_commit_count = 0U;
    memset(s_last_namespace, 0, sizeof(s_last_namespace));
    memset(s_last_set_key, 0, sizeof(s_last_set_key));
}

void mock_nvs_fail_next_open(void)
{
    s_fail_open = true;
}

void mock_nvs_fail_next_set(void)
{
    s_fail_set = true;
}

void mock_nvs_fail_next_commit(void)
{
    s_fail_commit = true;
}

void mock_nvs_fail_readback_after_commit(void)
{
    s_fail_readback = true;
}

void mock_nvs_corrupt_after_next_commit(size_t offset, uint8_t xor_mask)
{
    s_corrupt_after_commit = true;
    s_corrupt_offset = offset;
    s_corrupt_mask = xor_mask;
}

void mock_nvs_corrupt_committed(mock_usb_slot_t slot, size_t offset,
                                uint8_t xor_mask)
{
    assert(slot == MOCK_USB_SLOT_A || slot == MOCK_USB_SLOT_B);
    assert(s_committed[slot].present);
    assert(offset < s_committed[slot].size);
    s_committed[slot].data[offset] ^= xor_mask;
}

void mock_nvs_replace_committed(mock_usb_slot_t slot, const void *data,
                                size_t size)
{
    assert(slot == MOCK_USB_SLOT_A || slot == MOCK_USB_SLOT_B);
    assert(data || size == 0U);
    assert(size <= MOCK_CAPACITY);
    s_namespace_exists = true;
    s_committed[slot].present = true;
    s_committed[slot].size = size;
    if (size > 0U) {
        memcpy(s_committed[slot].data, data, size);
    }
}

void mock_nvs_remove_committed(mock_usb_slot_t slot)
{
    assert(slot == MOCK_USB_SLOT_A || slot == MOCK_USB_SLOT_B);
    memset(&s_committed[slot], 0, sizeof(s_committed[slot]));
}

const uint8_t *mock_nvs_committed_data(mock_usb_slot_t slot)
{
    assert(slot == MOCK_USB_SLOT_A || slot == MOCK_USB_SLOT_B);
    return s_committed[slot].data;
}

size_t mock_nvs_committed_size(mock_usb_slot_t slot)
{
    assert(slot == MOCK_USB_SLOT_A || slot == MOCK_USB_SLOT_B);
    return s_committed[slot].size;
}

bool mock_nvs_committed_present(mock_usb_slot_t slot)
{
    assert(slot == MOCK_USB_SLOT_A || slot == MOCK_USB_SLOT_B);
    return s_committed[slot].present;
}

unsigned mock_nvs_set_count(void)
{
    return s_set_count;
}

unsigned mock_nvs_commit_count(void)
{
    return s_commit_count;
}

const char *mock_nvs_last_namespace(void)
{
    return s_last_namespace;
}

const char *mock_nvs_last_set_key(void)
{
    return s_last_set_key;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    if (s_fail_open) {
        s_fail_open = false;
        return ESP_FAIL;
    }
    if (!namespace_name || strcmp(namespace_name, "jkvm_usb") != 0 ||
        !out_handle || s_handle_open) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(s_last_namespace, 0, sizeof(s_last_namespace));
    memcpy(s_last_namespace, namespace_name, strlen(namespace_name) + 1U);
    if (open_mode == NVS_READONLY && !s_namespace_exists) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (open_mode != NVS_READONLY && open_mode != NVS_READWRITE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (open_mode == NVS_READWRITE) {
        s_namespace_exists = true;
    }
    s_handle_open = true;
    s_open_mode = open_mode;
    *out_handle = MOCK_HANDLE;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == MOCK_HANDLE);
    assert(s_handle_open);
    memset(s_staged, 0, sizeof(s_staged));
    s_last_staged_slot = -1;
    s_handle_open = false;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                       void *out_value, size_t *length)
{
    if (handle != MOCK_HANDLE || !s_handle_open || !length) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_fail_get_now) {
        s_fail_get_now = false;
        return ESP_FAIL;
    }
    int slot = key_to_slot(key);
    if (slot < 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    const mock_blob_t *blob = &s_committed[slot];
    if (!blob->present) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (!out_value) {
        *length = blob->size;
        return ESP_OK;
    }
    if (*length < blob->size) {
        *length = blob->size;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, blob->data, blob->size);
    *length = blob->size;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length)
{
    if (handle != MOCK_HANDLE || !s_handle_open ||
        s_open_mode != NVS_READWRITE || !value || length > MOCK_CAPACITY) {
        return ESP_ERR_INVALID_ARG;
    }
    ++s_set_count;
    memset(s_last_set_key, 0, sizeof(s_last_set_key));
    if (key) {
        size_t key_length = strlen(key);
        assert(key_length < sizeof(s_last_set_key));
        memcpy(s_last_set_key, key, key_length + 1U);
    }
    if (s_fail_set) {
        s_fail_set = false;
        return ESP_FAIL;
    }
    int slot = key_to_slot(key);
    if (slot < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    s_staged[slot].present = true;
    s_staged[slot].size = length;
    memcpy(s_staged[slot].data, value, length);
    s_last_staged_slot = slot;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    if (handle != MOCK_HANDLE || !s_handle_open ||
        s_open_mode != NVS_READWRITE) {
        return ESP_ERR_INVALID_ARG;
    }
    ++s_commit_count;
    if (s_fail_commit) {
        s_fail_commit = false;
        return ESP_FAIL;
    }
    for (size_t slot = 0U; slot < 2U; ++slot) {
        if (s_staged[slot].present) {
            s_committed[slot] = s_staged[slot];
        }
    }
    if (s_corrupt_after_commit && s_last_staged_slot >= 0) {
        mock_blob_t *blob = &s_committed[s_last_staged_slot];
        assert(s_corrupt_offset < blob->size);
        blob->data[s_corrupt_offset] ^= s_corrupt_mask;
        s_corrupt_after_commit = false;
    }
    if (s_fail_readback) {
        s_fail_get_now = true;
        s_fail_readback = false;
    }
    return ESP_OK;
}

/* The USB debugging setting in NVS (jetkvm_usb_debugging_setting.c).
 * run_host_test.sh builds this file twice: as a release build, and with
 * CONFIG_JETKVM_USB_DEBUGGING_DEFAULT_ON=1 as a bench build. */

#include "jetkvm_usb_debugging.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"

#ifndef BENCH_BUILD
#error "run_host_test.sh sets BENCH_BUILD to 0 or 1"
#endif

/* NVS with one namespace, the device settings', and one u8 entry. */
static bool s_namespace;
static bool s_stored;
static uint8_t s_value;
static bool s_staged;
static uint8_t s_staged_value;
static esp_err_t s_open_error;
static esp_err_t s_commit_error;
static unsigned s_open_handles;

static void nvs_reset(void)
{
    s_namespace = s_stored = s_staged = false;
    s_value = s_staged_value = 0U;
    s_open_error = s_commit_error = ESP_OK;
    s_open_handles = 0U;
}

static void nvs_store(uint8_t value)
{
    s_namespace = true;
    s_stored = true;
    s_value = value;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode,
                   nvs_handle_t *out_handle)
{
    assert(strcmp(namespace_name, "jkvm_settings") == 0);
    if (s_open_error != ESP_OK) {
        return s_open_error;
    }
    if (open_mode == NVS_READONLY && !s_namespace) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (open_mode == NVS_READWRITE) {
        s_namespace = true;
    }
    s_open_handles++;
    *out_handle = 1U;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1U && s_open_handles > 0U);
    s_open_handles--;
    s_staged = false;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value)
{
    assert(handle == 1U && strcmp(key, "usb_debug") == 0);
    if (!s_stored) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *out_value = s_value;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(handle == 1U && strcmp(key, "usb_debug") == 0);
    s_staged = true;
    s_staged_value = value;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1U && s_staged);
    if (s_commit_error != ESP_OK) {
        return s_commit_error;
    }
    s_stored = true;
    s_value = s_staged_value;
    return ESP_OK;
}

static bool load(esp_err_t expected_result)
{
    bool enabled = !BENCH_BUILD;
    assert(jetkvm_usb_debugging_load(&enabled) == expected_result);
    assert(s_open_handles == 0U);
    return enabled;
}

/* A new device, or one after a factory reset (which erases the namespace):
 * a release keeps the port off, a bench build keeps it on. */
static void test_without_a_stored_setting(void)
{
    nvs_reset();
    assert(jetkvm_usb_debugging_build_default() == BENCH_BUILD);
    assert(load(ESP_OK) == BENCH_BUILD);
    /* The namespace exists for the other settings, without this entry. */
    s_namespace = true;
    assert(load(ESP_OK) == BENCH_BUILD);
}

/* A stored choice wins over the build default in both directions, so a
 * bench build keeps a user's off and a release keeps a user's on. */
static void test_round_trip(void)
{
    nvs_reset();
    assert(jetkvm_usb_debugging_save(!BENCH_BUILD) == ESP_OK);
    assert(s_value == (BENCH_BUILD ? 0U : 1U));
    assert(load(ESP_OK) == !BENCH_BUILD);
    assert(jetkvm_usb_debugging_save(BENCH_BUILD) == ESP_OK);
    assert(load(ESP_OK) == BENCH_BUILD);
    assert(s_open_handles == 0U);
}

/* A setting that cannot be read is the build default, with the error. */
static void test_unreadable_setting(void)
{
    nvs_reset();
    nvs_store(2U);
    assert(load(ESP_ERR_INVALID_STATE) == BENCH_BUILD);
    nvs_store(1U);
    s_open_error = ESP_FAIL;
    assert(load(ESP_FAIL) == BENCH_BUILD);
    assert(jetkvm_usb_debugging_load(NULL) == ESP_ERR_INVALID_ARG);
}

/* A failed commit reports the error and leaves the stored value. */
static void test_failed_save(void)
{
    nvs_reset();
    nvs_store(BENCH_BUILD ? 1U : 0U);
    s_commit_error = ESP_FAIL;
    assert(jetkvm_usb_debugging_save(!BENCH_BUILD) == ESP_FAIL);
    assert(s_open_handles == 0U);
    s_commit_error = ESP_OK;
    assert(load(ESP_OK) == BENCH_BUILD);
    s_open_error = ESP_FAIL;
    assert(jetkvm_usb_debugging_save(true) == ESP_FAIL);
}

int main(void)
{
    test_without_a_stored_setting();
    test_round_trip();
    test_unreadable_setting();
    test_failed_save();
    printf("jetkvm_usb_debugging setting (%s build): PASS\n",
           BENCH_BUILD ? "bench" : "release");
    return 0;
}

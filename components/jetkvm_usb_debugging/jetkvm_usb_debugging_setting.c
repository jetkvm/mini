/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_usb_debugging.h"

#include <stdint.h>

#include "nvs.h"
#include "sdkconfig.h"

#define USB_DEBUGGING_NAMESPACE "jkvm_settings"
#define USB_DEBUGGING_KEY "usb_debug"

bool jetkvm_usb_debugging_build_default(void)
{
#if defined(CONFIG_JETKVM_USB_DEBUGGING_DEFAULT_ON) && CONFIG_JETKVM_USB_DEBUGGING_DEFAULT_ON
    return true;
#else
    return false;
#endif
}

esp_err_t jetkvm_usb_debugging_load(bool *enabled)
{
    if (!enabled) {
        return ESP_ERR_INVALID_ARG;
    }
    *enabled = jetkvm_usb_debugging_build_default();
    nvs_handle_t handle;
    esp_err_t error = nvs_open(USB_DEBUGGING_NAMESPACE, NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    uint8_t value = 0;
    error = nvs_get_u8(handle, USB_DEBUGGING_KEY, &value);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    if (value > 1U) {
        return ESP_ERR_INVALID_STATE;
    }
    *enabled = value == 1U;
    return ESP_OK;
}

esp_err_t jetkvm_usb_debugging_save(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(USB_DEBUGGING_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = nvs_set_u8(handle, USB_DEBUGGING_KEY, enabled ? 1U : 0U);
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

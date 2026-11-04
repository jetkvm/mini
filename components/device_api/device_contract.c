/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "device_contract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define P4_CAPABILITIES_JSON_FORMAT \
    "{" \
      "\"schemaVersion\":1," \
      "\"platform\":\"esp32-p4\"," \
      "\"signaling\":\"p4-http-offer-v1\"," \
      "\"rpc\":true," \
      "\"video\":{" \
        "\"h264\":true," \
        "\"h265\":false," \
        "\"audio\":false," \
        "\"modes\":[" \
          "{\"width\":1920,\"height\":1080,\"fps\":30}," \
          "{\"width\":1280,\"height\":720,\"fps\":30}," \
          "{\"width\":1280,\"height\":720,\"fps\":60}," \
          "{\"width\":1024,\"height\":768,\"fps\":30}," \
          "{\"width\":1280,\"height\":800,\"fps\":30}," \
          "{\"width\":1280,\"height\":1024,\"fps\":30}" \
        "]," \
        "\"qualityControl\":true," \
        "\"edidControl\":%s," \
        "\"hostDisplayIdleControl\":%s," \
        "\"sleepControl\":%s," \
        "\"customEdid\":false" \
      "}," \
      "\"hid\":%s," \
      "\"keyboardLayout\":%s," \
      "\"hidMacros\":%s," \
      "\"paste\":%s," \
      "\"mouseJiggler\":%s," \
      "\"usbWake\":%s," \
      "\"hidUnreliable\":false," \
      "\"virtualMedia\":true," \
      "\"virtualMediaHttp\":%s," \
      "\"terminal\":false," \
      "\"serial\":false," \
      "\"extensions\":false," \
      "\"usbConfiguration\":%s," \
      "\"reboot\":%s," \
      "\"ota\":%s," \
      "\"networkSettings\":false," \
      "\"wakeOnLan\":%s," \
      "\"access\":{" \
        "\"tls\":false," \
        "\"localAuth\":false," \
        "\"cloudDeregister\":false" \
      "}," \
      "\"cloud\":true," \
      "\"mqtt\":%s" \
    "}"
static bool s_hid_available;
static bool s_keyboard_layout_available;
static bool s_hid_macros_available;
static bool s_mouse_jiggler_available;
static bool s_virtual_media_http_available;
static bool s_usb_wake_available;
static bool s_usb_configuration_available;
static bool s_reboot_available;
static bool s_wake_on_lan_available;
static bool s_edid_control_available;
static bool s_host_display_idle_control_available;
static bool s_video_sleep_control_available;
static bool s_mqtt_available;
static bool s_ota_available;

void p4_device_set_hid_available(bool available)
{
    __atomic_store_n(&s_hid_available, available, __ATOMIC_RELEASE);
}

bool p4_device_hid_available(void)
{
    return __atomic_load_n(&s_hid_available, __ATOMIC_ACQUIRE);
}

void p4_device_set_usb_wake_available(bool available)
{
    __atomic_store_n(&s_usb_wake_available, available, __ATOMIC_RELEASE);
}

bool p4_device_usb_wake_available(void)
{
    return __atomic_load_n(&s_usb_wake_available, __ATOMIC_ACQUIRE);
}

void p4_device_set_usb_configuration_available(bool available)
{
    __atomic_store_n(&s_usb_configuration_available, available,
                     __ATOMIC_RELEASE);
}

bool p4_device_usb_configuration_available(void)
{
    return __atomic_load_n(&s_usb_configuration_available, __ATOMIC_ACQUIRE);
}

void p4_device_set_keyboard_layout_available(bool available)
{
    __atomic_store_n(&s_keyboard_layout_available, available,
                     __ATOMIC_RELEASE);
}

bool p4_device_keyboard_layout_available(void)
{
    return __atomic_load_n(&s_keyboard_layout_available, __ATOMIC_ACQUIRE);
}

void p4_device_set_hid_macros_available(bool available)
{
    __atomic_store_n(&s_hid_macros_available, available, __ATOMIC_RELEASE);
}

bool p4_device_hid_macros_available(void)
{
    return __atomic_load_n(&s_hid_macros_available, __ATOMIC_ACQUIRE);
}

void p4_device_set_mouse_jiggler_available(bool available)
{
    __atomic_store_n(&s_mouse_jiggler_available, available,
                     __ATOMIC_RELEASE);
}

bool p4_device_mouse_jiggler_available(void)
{
    return __atomic_load_n(&s_mouse_jiggler_available, __ATOMIC_ACQUIRE);
}

void p4_device_set_virtual_media_http_available(bool available)
{
    __atomic_store_n(&s_virtual_media_http_available, available,
                     __ATOMIC_RELEASE);
}

bool p4_device_virtual_media_http_available(void)
{
    return __atomic_load_n(&s_virtual_media_http_available, __ATOMIC_ACQUIRE);
}

void p4_device_set_reboot_available(bool available)
{
    __atomic_store_n(&s_reboot_available, available, __ATOMIC_RELEASE);
}

bool p4_device_reboot_available(void)
{
    return __atomic_load_n(&s_reboot_available, __ATOMIC_ACQUIRE);
}

void p4_device_set_wake_on_lan_available(bool available)
{
    __atomic_store_n(&s_wake_on_lan_available, available, __ATOMIC_RELEASE);
}

bool p4_device_wake_on_lan_available(void)
{
    return __atomic_load_n(&s_wake_on_lan_available, __ATOMIC_ACQUIRE);
}

void p4_device_set_edid_control_available(bool available)
{
    __atomic_store_n(&s_edid_control_available, available, __ATOMIC_RELEASE);
}

bool p4_device_edid_control_available(void)
{
    return __atomic_load_n(&s_edid_control_available, __ATOMIC_ACQUIRE);
}

void p4_device_set_host_display_idle_control_available(bool available)
{
    __atomic_store_n(&s_host_display_idle_control_available, available,
                     __ATOMIC_RELEASE);
}

bool p4_device_host_display_idle_control_available(void)
{
    return __atomic_load_n(&s_host_display_idle_control_available,
                           __ATOMIC_ACQUIRE);
}

void p4_device_set_video_sleep_control_available(bool available)
{
    __atomic_store_n(&s_video_sleep_control_available, available,
                     __ATOMIC_RELEASE);
}

bool p4_device_video_sleep_control_available(void)
{
    return __atomic_load_n(&s_video_sleep_control_available,
                           __ATOMIC_ACQUIRE);
}

void p4_device_set_mqtt_available(bool available)
{
    __atomic_store_n(&s_mqtt_available, available, __ATOMIC_RELEASE);
}

bool p4_device_mqtt_available(void)
{
    return __atomic_load_n(&s_mqtt_available, __ATOMIC_ACQUIRE);
}

void p4_device_set_ota_available(bool available)
{
    __atomic_store_n(&s_ota_available, available, __ATOMIC_RELEASE);
}

bool p4_device_ota_available(void)
{
    return __atomic_load_n(&s_ota_available, __ATOMIC_ACQUIRE);
}

bool p4_device_status_json(bool is_setup, bool reset_pending, const char *reset_error,
                           const char *sku, char *output, size_t output_capacity)
{
    if (!sku || !output || !output_capacity) return false;
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddBoolToObject(root, "isSetup", is_setup) ||
        !cJSON_AddBoolToObject(root, "factoryResetPending", reset_pending) ||
        !cJSON_AddStringToObject(root, "sku", sku) ||
        (reset_error && !cJSON_AddStringToObject(root, "factoryResetError", reset_error))) {
        cJSON_Delete(root);
        return false;
    }
    char *rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) return false;
    size_t length = strlen(rendered);
    bool fits = length < output_capacity;
    if (fits) memcpy(output, rendered, length + 1);
    cJSON_free(rendered);
    return fits;
}

bool p4_device_identity_json(const char *device_id, bool password_enabled,
                             char *output, size_t output_capacity)
{
    if (!device_id || !output || output_capacity == 0U || !device_id[0]) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)device_id;
         *cursor; ++cursor) {
        bool safe = (*cursor >= '0' && *cursor <= '9') ||
                    (*cursor >= 'a' && *cursor <= 'z') ||
                    (*cursor >= 'A' && *cursor <= 'Z') ||
                    *cursor == '.' || *cursor == '_' || *cursor == ':' ||
                    *cursor == '-';
        if (!safe) {
            return false;
        }
    }
    int written = snprintf(output, output_capacity,
                           "{\"authMode\":\"%s\","
                           "\"deviceId\":\"%s\"}",
                           password_enabled ? "password" : "noPassword", device_id);
    return written >= 0 && (size_t)written < output_capacity;
}

bool p4_cloud_state_json(bool connected, const char *cloud_url,
                         const char *cloud_app_url,
                         char *output, size_t output_capacity)
{
    if (!cloud_url || !cloud_app_url || !output || output_capacity == 0U) {
        return false;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddBoolToObject(root, "connected", connected) ||
        !cJSON_AddStringToObject(root, "url", cloud_url) ||
        !cJSON_AddStringToObject(root, "appUrl", cloud_app_url)) {
        cJSON_Delete(root);
        return false;
    }
    char *rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return false;
    }
    size_t length = strlen(rendered);
    bool fits = length < output_capacity;
    if (fits) {
        memcpy(output, rendered, length + 1U);
    }
    cJSON_free(rendered);
    return fits;
}

const char *p4_device_capabilities_json(void)
{
    /* Compatibility helper for sequential callers and host tests.  The HTTP
     * endpoint uses the caller-owned renderer below, so concurrent requests
     * never share this storage. */
    static char json[P4_DEVICE_CAPABILITIES_JSON_CAPACITY];
    if (!p4_device_capabilities_json_copy(json, sizeof(json))) {
        return "{}";
    }
    return json;
}

bool p4_device_capabilities_json_copy(char *output, size_t output_capacity)
{
    if (!output || output_capacity == 0U) {
        return false;
    }
    /* Paste is the existing frontend-generated HIDRPC keyboard macro path;
     * it has no device-side storage or additional RPC.  Advertise it once the
     * generation-gated bridge exists; its runtime input gate remains closed
     * whenever target USB emulation is disabled. */
    const bool hid_available = p4_device_hid_available();
    int written = snprintf(
        output, output_capacity, P4_CAPABILITIES_JSON_FORMAT,
        p4_device_edid_control_available() ? "true" : "false",
        p4_device_host_display_idle_control_available() ? "true" : "false",
        p4_device_video_sleep_control_available() ? "true" : "false",
        hid_available ? "true" : "false",
        p4_device_keyboard_layout_available() ? "true" : "false",
        p4_device_hid_macros_available() ? "true" : "false",
        hid_available ? "true" : "false",
        p4_device_mouse_jiggler_available() ? "true" : "false",
        p4_device_usb_wake_available() ? "true" : "false",
        p4_device_virtual_media_http_available() ? "true" : "false",
        p4_device_usb_configuration_available() ? "true" : "false",
        p4_device_reboot_available() ? "true" : "false",
        p4_device_ota_available() ? "true" : "false",
        p4_device_wake_on_lan_available() ? "true" : "false",
        p4_device_mqtt_available() ? "true" : "false");
    return written >= 0 && (size_t)written < output_capacity;
}

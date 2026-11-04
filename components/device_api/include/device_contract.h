/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define P4_DEVICE_CAPABILITIES_JSON_CAPACITY 1024U

/** `sku` is the hardware SKU (JETKVM_DEVICE_SKU); the welcome page picks its
 *  device photo from it before setup. */
bool p4_device_status_json(bool is_setup, bool reset_pending, const char *reset_error,
                           const char *sku, char *output, size_t output_capacity);
/** Advertise HID after the generation-gated input bridge first starts. */
void p4_device_set_hid_available(bool available);
bool p4_device_hid_available(void);
/** Advertise wakeHost after the persistent P4 HID bridge first initializes. */
void p4_device_set_usb_wake_available(bool available);
bool p4_device_usb_wake_available(void);
/** Advertise persistent USB identity/class configuration after its owner is ready. */
void p4_device_set_usb_configuration_available(bool available);
bool p4_device_usb_configuration_available(void);
/** Advertise layout RPCs only after persistent settings are initialized. */
void p4_device_set_keyboard_layout_available(bool available);
bool p4_device_keyboard_layout_available(void);
/** Advertise saved macro RPCs only after their transactional store is ready. */
void p4_device_set_hid_macros_available(bool available);
bool p4_device_hid_macros_available(void);
/** Advertise the persisted device-local mouse jiggler and scheduler. */
void p4_device_set_mouse_jiggler_available(bool available);
bool p4_device_mouse_jiggler_available(void);
/** Advertise HTTP(S) byte-range media only after its backend initializes. */
void p4_device_set_virtual_media_http_available(bool available);
bool p4_device_virtual_media_http_available(void);
/** Advertise reboot only while its transport worker is fully usable. */
void p4_device_set_reboot_available(bool available);
bool p4_device_reboot_available(void);
/** Advertise saved-target WOL RPCs only after their NVS store initializes. */
void p4_device_set_wake_on_lan_available(bool available);
bool p4_device_wake_on_lan_available(void);
/** Advertise safe EDID presets after their command boundary is initialized. */
void p4_device_set_edid_control_available(bool available);
bool p4_device_edid_control_available(void);
/** Advertise session-aware HDMI HPD control after settings are initialized. */
void p4_device_set_host_display_idle_control_available(bool available);
bool p4_device_host_display_idle_control_available(void);
/** Advertise session-aware HDMI capture sleep after persistent wiring. */
void p4_device_set_video_sleep_control_available(bool available);
bool p4_device_video_sleep_control_available(void);
/** Advertise MQTT only after its CRC-protected configuration store is ready. */
void p4_device_set_mqtt_available(bool available);
bool p4_device_mqtt_available(void);
void p4_device_set_ota_available(bool available);
bool p4_device_ota_available(void);
bool p4_device_identity_json(const char *device_id, bool password_enabled,
                             char *output, size_t output_capacity);
bool p4_cloud_state_json(bool connected, const char *cloud_url,
                         const char *cloud_app_url,
                         char *output, size_t output_capacity);
const char *p4_device_capabilities_json(void);
/** Reentrant renderer used by the HTTP endpoint. */
bool p4_device_capabilities_json_copy(char *output, size_t output_capacity);

#ifdef __cplusplus
}
#endif

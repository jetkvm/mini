/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "jetkvm_control.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

typedef struct {
    cJSON *result;
    int error_code;
    const char *error_message;
    const char *error_data;
    /* Formatted error data. Sized for the longest message that embeds a
     * storage filename, "file does not exist: <filename>". */
    char error_storage[24U + JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
} rpc_dispatch_result_t;

typedef bool (*rpc_method_fn)(const jetkvm_control_context_t *context,
                              const cJSON *params,
                              rpc_dispatch_result_t *dispatch);

typedef struct {
    const char *name;
    rpc_method_fn handler;
} rpc_method_t;

/* Process-wide default contexts installed by the public setters. */
extern jetkvm_local_display_context_t s_default_local_display;
bool rpc_get_backlight_settings(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_set_backlight_settings(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_get_display_rotation(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_set_display_rotation(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
extern jetkvm_keyboard_layout_context_t s_default_keyboard_layout;
extern jetkvm_edid_context_t s_default_edid;
extern jetkvm_host_display_idle_context_t s_default_host_display_idle;
extern jetkvm_video_sleep_context_t s_default_video_sleep;
extern jetkvm_mqtt_context_t s_default_mqtt;
extern jetkvm_ota_context_t s_default_ota;

/* Per-request context takes precedence over the process-wide default. */
const jetkvm_keyboard_layout_context_t *keyboard_layout_context(
    const jetkvm_control_context_t *context);
const jetkvm_edid_context_t *edid_context(
    const jetkvm_control_context_t *context);
const jetkvm_host_display_idle_context_t *host_display_idle_context(
    const jetkvm_control_context_t *context);
const jetkvm_video_sleep_context_t *video_sleep_context(
    const jetkvm_control_context_t *context);
const jetkvm_mqtt_context_t *mqtt_context(
    const jetkvm_control_context_t *context);
const jetkvm_ota_context_t *ota_context(
    const jetkvm_control_context_t *context);

/* Validation/dispatch helpers shared by two or more rpc_*.c domains. */
bool set_invalid_hid_params(rpc_dispatch_result_t *dispatch,
                            const char *data);
bool set_hid_unavailable(rpc_dispatch_result_t *dispatch,
                         const char *data);
bool require_no_rpc_params(const cJSON *params,
                           rpc_dispatch_result_t *dispatch);
const cJSON *rpc_param(const cJSON *params, const char *name);
bool json_integer_in_range(const cJSON *item, int64_t minimum,
                           int64_t maximum, int64_t *value);
int hex_nibble(char value);
bool copy_json_string(const cJSON *item, char *output,
                      size_t output_capacity, bool allow_empty);

bool rpc_factory_reset(const jetkvm_control_context_t *context,
    const cJSON *params, rpc_dispatch_result_t *dispatch);

bool rpc_deregister_device(const jetkvm_control_context_t *context,
                           const cJSON *params, rpc_dispatch_result_t *dispatch);

/* rpc_video.c */
bool rpc_ping(const jetkvm_control_context_t *context,
              const cJSON *params, rpc_dispatch_result_t *dispatch);
bool rpc_get_device_capabilities(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch);
bool rpc_get_video_state(const jetkvm_control_context_t *context,
                         const cJSON *params,
                         rpc_dispatch_result_t *dispatch);
bool rpc_get_video_log_status(const jetkvm_control_context_t *context,
                              const cJSON *params,
                              rpc_dispatch_result_t *dispatch);
bool rpc_get_local_version(const jetkvm_control_context_t *context,
                           const cJSON *params,
                           rpc_dispatch_result_t *dispatch);
bool rpc_get_device_id(const jetkvm_control_context_t *context,
                       const cJSON *params, rpc_dispatch_result_t *dispatch);
bool rpc_get_network_settings(const jetkvm_control_context_t *context,
                              const cJSON *params,
                              rpc_dispatch_result_t *dispatch);
bool rpc_get_network_state(const jetkvm_control_context_t *context,
                           const cJSON *params,
                           rpc_dispatch_result_t *dispatch);
bool rpc_get_cloud_state(const jetkvm_control_context_t *context,
                         const cJSON *params,
                         rpc_dispatch_result_t *dispatch);
bool rpc_set_audio_config(const jetkvm_control_context_t *context, const cJSON *params, rpc_dispatch_result_t *dispatch);

bool rpc_get_audio_config(const jetkvm_control_context_t *context,
                          const cJSON *params,
                          rpc_dispatch_result_t *dispatch);
bool rpc_get_stream_quality(const jetkvm_control_context_t *context,
                            const cJSON *params,
                            rpc_dispatch_result_t *dispatch);
bool rpc_get_supported_video_codecs(const jetkvm_control_context_t *context,
                                    const cJSON *params,
                                    rpc_dispatch_result_t *dispatch);
bool rpc_get_video_codec(const jetkvm_control_context_t *context,
                         const cJSON *params,
                         rpc_dispatch_result_t *dispatch);
bool rpc_set_video_codec(const jetkvm_control_context_t *context,
                         const cJSON *params,
                         rpc_dispatch_result_t *dispatch);
bool rpc_set_video_stream_paused(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch);
bool rpc_set_stream_quality(const jetkvm_control_context_t *context,
                            const cJSON *params,
                            rpc_dispatch_result_t *dispatch);
bool rpc_set_cloud_url(const jetkvm_control_context_t *context,
                       const cJSON *params, rpc_dispatch_result_t *dispatch);

/* rpc_display_settings.c */
bool rpc_get_edid_presets(const jetkvm_control_context_t *context,
                           const cJSON *params,
                           rpc_dispatch_result_t *dispatch);
bool rpc_get_edid(const jetkvm_control_context_t *context,
                  const cJSON *params, rpc_dispatch_result_t *dispatch);
bool rpc_set_edid(const jetkvm_control_context_t *context,
                  const cJSON *params, rpc_dispatch_result_t *dispatch);
bool rpc_get_host_display_idle(const jetkvm_control_context_t *context,
                               const cJSON *params,
                               rpc_dispatch_result_t *dispatch);
bool rpc_set_host_display_idle(const jetkvm_control_context_t *context,
                               const cJSON *params,
                               rpc_dispatch_result_t *dispatch);
bool rpc_get_video_sleep_mode(const jetkvm_control_context_t *context,
                              const cJSON *params,
                              rpc_dispatch_result_t *dispatch);
bool rpc_set_video_sleep_mode(const jetkvm_control_context_t *context,
                              const cJSON *params,
                              rpc_dispatch_result_t *dispatch);
bool rpc_get_keyboard_layout(const jetkvm_control_context_t *context,
                             const cJSON *params,
                             rpc_dispatch_result_t *dispatch);
bool rpc_set_keyboard_layout(const jetkvm_control_context_t *context,
                             const cJSON *params,
                             rpc_dispatch_result_t *dispatch);

/* rpc_usb.c */
bool rpc_get_keyboard_led_state(const jetkvm_control_context_t *context,
                                const cJSON *params,
                                rpc_dispatch_result_t *dispatch);
bool rpc_get_key_down_state(const jetkvm_control_context_t *context,
                            const cJSON *params,
                            rpc_dispatch_result_t *dispatch);
bool rpc_get_usb_state(const jetkvm_control_context_t *context,
                       const cJSON *params, rpc_dispatch_result_t *dispatch);
bool rpc_set_usb_config(const jetkvm_control_context_t *context,
                        const cJSON *params, rpc_dispatch_result_t *dispatch);
bool rpc_set_usb_devices(const jetkvm_control_context_t *context,
                         const cJSON *params,
                         rpc_dispatch_result_t *dispatch);
bool rpc_set_usb_device_state(const jetkvm_control_context_t *context,
                              const cJSON *params,
                              rpc_dispatch_result_t *dispatch);
bool rpc_get_usb_devices(const jetkvm_control_context_t *context,
                         const cJSON *params,
                         rpc_dispatch_result_t *dispatch);
bool rpc_get_usb_config(const jetkvm_control_context_t *context,
                        const cJSON *params, rpc_dispatch_result_t *dispatch);
bool rpc_get_usb_emulation_state(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch);
bool rpc_set_usb_emulation_state(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch);
extern jetkvm_usb_debugging_context_t s_default_usb_debugging;
bool rpc_get_usb_debugging_state(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch);
bool rpc_set_usb_debugging_state(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch);
bool rpc_wake_host(const jetkvm_control_context_t *context,
                   const cJSON *params, rpc_dispatch_result_t *dispatch);
bool rpc_keyboard_report(const jetkvm_control_context_t *context,
                         const cJSON *params,
                         rpc_dispatch_result_t *dispatch);
bool rpc_keypress_report(const jetkvm_control_context_t *context,
                         const cJSON *params,
                         rpc_dispatch_result_t *dispatch);
bool rpc_abs_mouse_report(const jetkvm_control_context_t *context,
                          const cJSON *params,
                          rpc_dispatch_result_t *dispatch);
bool rpc_rel_mouse_report(const jetkvm_control_context_t *context,
                          const cJSON *params,
                          rpc_dispatch_result_t *dispatch);
bool rpc_wheel_report(const jetkvm_control_context_t *context,
                      const cJSON *params, rpc_dispatch_result_t *dispatch);

/* rpc_wol.c */
bool rpc_get_timezones(const jetkvm_control_context_t *context,
                       const cJSON *params, rpc_dispatch_result_t *dispatch);
bool rpc_get_wake_on_lan_devices(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch);
bool rpc_set_wake_on_lan_devices(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch);
bool rpc_send_wol_magic_packet(const jetkvm_control_context_t *context,
                               const cJSON *params,
                               rpc_dispatch_result_t *dispatch);

/* rpc_system.c */
bool rpc_reboot(const jetkvm_control_context_t *context,
                const cJSON *params, rpc_dispatch_result_t *dispatch);

/* rpc_ota.c */
bool rpc_get_update_status(const jetkvm_control_context_t *context,
                           const cJSON *params,
                           rpc_dispatch_result_t *dispatch);
bool rpc_check_update_components(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch);
bool rpc_try_update(const jetkvm_control_context_t *context,
                    const cJSON *params, rpc_dispatch_result_t *dispatch);
bool rpc_try_update_components(const jetkvm_control_context_t *context,
                               const cJSON *params,
                               rpc_dispatch_result_t *dispatch);
bool rpc_get_auto_update_state(const jetkvm_control_context_t *context,
                               const cJSON *params,
                               rpc_dispatch_result_t *dispatch);
bool rpc_set_auto_update_state(const jetkvm_control_context_t *context,
                               const cJSON *params,
                               rpc_dispatch_result_t *dispatch);
bool rpc_get_dev_channel_state(const jetkvm_control_context_t *context,
                               const cJSON *params,
                               rpc_dispatch_result_t *dispatch);
bool rpc_set_dev_channel_state(const jetkvm_control_context_t *context,
                               const cJSON *params,
                               rpc_dispatch_result_t *dispatch);
bool rpc_is_update_pending(const jetkvm_control_context_t *context,
                           const cJSON *params,
                           rpc_dispatch_result_t *dispatch);
bool rpc_get_update_api_url(const jetkvm_control_context_t *context,
                            const cJSON *params,
                            rpc_dispatch_result_t *dispatch);
bool rpc_set_update_api_url(const jetkvm_control_context_t *context,
                            const cJSON *params,
                            rpc_dispatch_result_t *dispatch);

/* rpc_mqtt.c */
bool rpc_get_mqtt_settings(const jetkvm_control_context_t *context,
                           const cJSON *params,
                           rpc_dispatch_result_t *dispatch);
bool rpc_set_mqtt_settings(const jetkvm_control_context_t *context,
                           const cJSON *params,
                           rpc_dispatch_result_t *dispatch);
bool rpc_get_mqtt_status(const jetkvm_control_context_t *context,
                         const cJSON *params,
                         rpc_dispatch_result_t *dispatch);
bool rpc_test_mqtt_connection(const jetkvm_control_context_t *context,
                              const cJSON *params,
                              rpc_dispatch_result_t *dispatch);

/* rpc_macros.c */
bool rpc_get_keyboard_macros(const jetkvm_control_context_t *context,
                             const cJSON *params,
                             rpc_dispatch_result_t *dispatch);
bool rpc_set_keyboard_macros(const jetkvm_control_context_t *context,
                             const cJSON *params,
                             rpc_dispatch_result_t *dispatch);
bool rpc_get_jiggler_state(const jetkvm_control_context_t *context,
                           const cJSON *params,
                           rpc_dispatch_result_t *dispatch);
bool rpc_set_jiggler_state(const jetkvm_control_context_t *context,
                           const cJSON *params,
                           rpc_dispatch_result_t *dispatch);
bool rpc_get_jiggler_config(const jetkvm_control_context_t *context,
                            const cJSON *params,
                            rpc_dispatch_result_t *dispatch);
bool rpc_set_jiggler_config(const jetkvm_control_context_t *context,
                            const cJSON *params,
                            rpc_dispatch_result_t *dispatch);

/* rpc_virtual_media.c */
bool rpc_get_virtual_media_state(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch);
bool rpc_get_storage_state(const jetkvm_control_context_t *context,
                           const cJSON *params,
                           rpc_dispatch_result_t *dispatch);
#if JETKVM_TF_CARD_TEST
bool rpc_test_tf_card_out(const jetkvm_control_context_t *context,
                          const cJSON *params,
                          rpc_dispatch_result_t *dispatch);
#endif
bool rpc_format_storage(const jetkvm_control_context_t *context,
                        const cJSON *params,
                        rpc_dispatch_result_t *dispatch);
bool rpc_get_storage_space(const jetkvm_control_context_t *context,
                           const cJSON *params,
                           rpc_dispatch_result_t *dispatch);
bool rpc_list_storage_files(const jetkvm_control_context_t *context,
                            const cJSON *params,
                            rpc_dispatch_result_t *dispatch);
bool rpc_mount_with_storage(const jetkvm_control_context_t *context,
                            const cJSON *params,
                            rpc_dispatch_result_t *dispatch);
bool rpc_check_mount_url(const jetkvm_control_context_t *context,
                         const cJSON *params,
                         rpc_dispatch_result_t *dispatch);
bool rpc_mount_with_http(const jetkvm_control_context_t *context,
                         const cJSON *params,
                         rpc_dispatch_result_t *dispatch);
bool rpc_mount_builtin_installer(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch);
bool rpc_unmount_image(const jetkvm_control_context_t *context,
                       const cJSON *params, rpc_dispatch_result_t *dispatch);
bool rpc_delete_storage_file(const jetkvm_control_context_t *context,
                             const cJSON *params,
                             rpc_dispatch_result_t *dispatch);
bool rpc_start_storage_file_upload(const jetkvm_control_context_t *context,
                                   const cJSON *params,
                                   rpc_dispatch_result_t *dispatch);
extern jetkvm_storage_download_context_t s_default_storage_download;
bool rpc_download_to_storage(const jetkvm_control_context_t *context,
                             const cJSON *params,
                             rpc_dispatch_result_t *dispatch);
bool rpc_get_storage_download_state(const jetkvm_control_context_t *context,
                                    const cJSON *params,
                                    rpc_dispatch_result_t *dispatch);

/* rpc_os_services.c */
extern jetkvm_os_services_context_t s_default_os_services;
bool rpc_get_os_services_settings(const jetkvm_control_context_t *context,
                                   const cJSON *params,
                                   rpc_dispatch_result_t *dispatch);
bool rpc_set_os_services_settings(const jetkvm_control_context_t *context,
                                   const cJSON *params,
                                   rpc_dispatch_result_t *dispatch);
bool rpc_guest_clipboard_get(const jetkvm_control_context_t *context,
                             const cJSON *params,
                             rpc_dispatch_result_t *dispatch);
bool rpc_guest_clipboard_set(const jetkvm_control_context_t *context,
                             const cJSON *params,
                             rpc_dispatch_result_t *dispatch);
bool rpc_guest_file_drop(const jetkvm_control_context_t *context,
                         const cJSON *params, rpc_dispatch_result_t *dispatch);
bool rpc_guest_update_start(const jetkvm_control_context_t *context,
                            const cJSON *params,
                            rpc_dispatch_result_t *dispatch);

extern jetkvm_tls_context_t s_default_tls;
extern jetkvm_extra_hostnames_context_t s_default_extra_hostnames;
extern jetkvm_tailscale_context_t s_default_tailscale;
const jetkvm_tailscale_context_t *tailscale_context(const jetkvm_control_context_t *);
bool rpc_get_tailscale_status(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_get_tailscale_config(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_set_tailscale_config(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_logout_tailscale(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_get_tailscale_control_url(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_set_tailscale_control_url(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_get_tls_state(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_set_tls_state(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_get_extra_hostnames(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_set_extra_hostnames(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_set_network_settings(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_renew_dhcp_lease(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_get_mac_address_settings(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_set_mac_address_settings(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_get_public_ip_addresses(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);

bool rpc_get_wifi_settings(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_set_wifi_band(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_scan_wifi_networks(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_save_wifi_network(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_remove_wifi_network(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);
bool rpc_connect_wifi_network(const jetkvm_control_context_t *, const cJSON *, rpc_dispatch_result_t *);

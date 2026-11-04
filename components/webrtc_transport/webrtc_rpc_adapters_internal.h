/* Component-private RPC adapter declarations. */
#pragma once

#include "jetkvm_control.h"

/* The RPC adapters in webrtc_transport_rpc_adapters.c; only the peer's
 * control_context in webrtc_data_channels.c and the RPC worker call these. */
bool rpc_send_wol(const uint8_t mac[6], const char *broadcast_ip, void *ctx);
bool rpc_get_wol_devices_adapter(
    jetkvm_control_wol_device_list_t *output, void *ctx);
bool rpc_set_wol_devices_adapter(
    const jetkvm_control_wol_device_list_t *input, void *ctx);
bool rpc_get_keyboard_macros_adapter(
    jetkvm_control_keyboard_macro_list_t *output, void *ctx);
bool rpc_set_keyboard_macros_adapter(
    const jetkvm_control_keyboard_macro_list_t *input, void *ctx);
bool rpc_get_jiggler_state_adapter(bool *enabled, void *ctx);
bool rpc_set_jiggler_state_adapter(bool enabled, void *ctx);
bool rpc_get_jiggler_config_adapter(
    jetkvm_control_jiggler_config_t *config, void *ctx);
jetkvm_jiggler_config_change_result_t rpc_set_jiggler_config_adapter(
    const jetkvm_control_jiggler_config_t *config, void *ctx);
jetkvm_cloud_config_change_result_t rpc_set_cloud_urls(
    const char *cloud_url, const char *cloud_app_url, void *ctx);
bool rpc_get_usb_configuration(
    jetkvm_usb_configuration_snapshot_t *snapshot, void *ctx);
bool rpc_get_virtual_media_state_adapter(
    jetkvm_control_virtual_media_state_t *output, void *ctx);
bool rpc_get_virtual_media_space_adapter(
    jetkvm_control_storage_space_t *output, void *ctx);
bool rpc_list_virtual_media_files_adapter(
    jetkvm_visit_storage_file_fn visit, void *visit_ctx, void *ctx);
jetkvm_control_storage_format_result_t rpc_format_virtual_media_storage_adapter(
    jetkvm_control_storage_space_t *space, void *ctx);
jetkvm_virtual_media_change_result_t rpc_mount_virtual_media_http_adapter(
    const char *url, jetkvm_control_virtual_media_mode_t mode, void *ctx);
jetkvm_virtual_media_change_result_t rpc_mount_virtual_media_storage_adapter(
    const char *filename, jetkvm_control_virtual_media_mode_t mode, void *ctx);
jetkvm_virtual_media_change_result_t
rpc_mount_virtual_media_builtin_installer_adapter(void *ctx);
jetkvm_virtual_media_change_result_t rpc_unmount_virtual_media_adapter(
    void *ctx);
bool rpc_check_virtual_media_url_adapter(
    const char *url, jetkvm_control_virtual_media_url_info_t *output,
    void *ctx);
jetkvm_virtual_media_change_result_t rpc_delete_virtual_media_file_adapter(
    const char *filename, void *ctx);
jetkvm_virtual_media_change_result_t rpc_start_virtual_media_upload_adapter(
    const char *filename, uint64_t size, const char *sha256,
    jetkvm_control_storage_upload_t *output, void *ctx);
jetkvm_usb_configuration_change_result_t rpc_set_usb_descriptor_config(
    const jetkvm_usb_descriptor_config_t *descriptor, void *ctx);
jetkvm_usb_configuration_change_result_t rpc_set_usb_devices_adapter(
    const jetkvm_usb_devices_t *devices, void *ctx);
jetkvm_usb_configuration_change_result_t rpc_set_usb_device_state_adapter(
    jetkvm_usb_device_t device, bool enabled, void *ctx);
jetkvm_quality_change_result_t rpc_queue_video_quality_change(
    double factor, void *ctx);
bool rpc_get_video_log_status_adapter(char *output, size_t output_capacity,
                                      void *arg);
jetkvm_video_codec_change_result_t rpc_set_video_codec_preference(
    const char *codec, void *ctx);
bool rpc_set_video_stream_paused_adapter(bool paused, void *ctx);

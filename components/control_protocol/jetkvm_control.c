/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "control_protocol_internal.h"

#include <limits.h>
#include <math.h>
#include <string.h>

static jetkvm_control_result_t render_response(
    cJSON *response, char *output, size_t output_capacity, size_t *output_size)
{
    /* Print straight into the caller's buffer. A reply near the 128 KiB RPC
     * buffers otherwise also needs cJSON's print buffer, which doubles as it
     * grows, and a final copy of it. cJSON asks for one byte more than the
     * reply uses here, so a reply that only just fits takes the copy below. */
    if (output_capacity <= (size_t)INT_MAX &&
        cJSON_PrintPreallocated(response, output, (int)output_capacity, false)) {
        *output_size = strlen(output);
        return JETKVM_CONTROL_RESPONSE_READY;
    }
    char *rendered = cJSON_PrintUnformatted(response);
    if (!rendered) {
        return JETKVM_CONTROL_OUTPUT_TOO_SMALL;
    }
    size_t size = strlen(rendered);
    if (size + 1U > output_capacity) {
        cJSON_free(rendered);
        return JETKVM_CONTROL_OUTPUT_TOO_SMALL;
    }
    memcpy(output, rendered, size + 1U);
    cJSON_free(rendered);
    *output_size = size;
    return JETKVM_CONTROL_RESPONSE_READY;
}

static cJSON *new_rpc_response(const cJSON *id)
{
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        return NULL;
    }
    if (!cJSON_AddStringToObject(response, "jsonrpc", "2.0")) {
        cJSON_Delete(response);
        return NULL;
    }
    cJSON *response_id = id ? cJSON_Duplicate(id, true) : cJSON_CreateNull();
    if (!response_id || !cJSON_AddItemToObject(response, "id", response_id)) {
        cJSON_Delete(response_id);
        cJSON_Delete(response);
        return NULL;
    }
    return response;
}

static bool add_rpc_error(cJSON *response, int code, const char *message,
                          const char *data)
{
    cJSON *error = cJSON_AddObjectToObject(response, "error");
    if (!error || !cJSON_AddNumberToObject(error, "code", code) ||
        !cJSON_AddStringToObject(error, "message", message)) {
        return false;
    }
    return !data || cJSON_AddStringToObject(error, "data", data);
}

jetkvm_local_display_context_t s_default_local_display;
void jetkvm_control_set_default_local_display_context(const jetkvm_local_display_context_t *context)
{
    s_default_local_display = context ? *context : (jetkvm_local_display_context_t){0};
}

jetkvm_keyboard_layout_context_t s_default_keyboard_layout;
jetkvm_edid_context_t s_default_edid;
jetkvm_host_display_idle_context_t s_default_host_display_idle;
jetkvm_video_sleep_context_t s_default_video_sleep;
jetkvm_mqtt_context_t s_default_mqtt;
jetkvm_ota_context_t s_default_ota;
jetkvm_tls_context_t s_default_tls;
jetkvm_extra_hostnames_context_t s_default_extra_hostnames;
jetkvm_tailscale_context_t s_default_tailscale;
void jetkvm_control_set_default_tailscale_context(const jetkvm_tailscale_context_t *context)
{
    s_default_tailscale = context ? *context : (jetkvm_tailscale_context_t){0};
}
void jetkvm_control_set_default_extra_hostnames_context(const jetkvm_extra_hostnames_context_t *context)
{
    s_default_extra_hostnames = context ? *context : (jetkvm_extra_hostnames_context_t){0};
}
void jetkvm_control_set_default_tls_context(const jetkvm_tls_context_t *context)
{
    s_default_tls = context ? *context : (jetkvm_tls_context_t){0};
}

void jetkvm_control_set_default_keyboard_layout_context(
    const jetkvm_keyboard_layout_context_t *context)
{
    s_default_keyboard_layout = context ? *context :
                                          (jetkvm_keyboard_layout_context_t){ 0 };
}

void jetkvm_control_set_default_edid_context(
    const jetkvm_edid_context_t *context)
{
    s_default_edid = context ? *context : (jetkvm_edid_context_t){ 0 };
}

void jetkvm_control_set_default_host_display_idle_context(
    const jetkvm_host_display_idle_context_t *context)
{
    s_default_host_display_idle = context ? *context :
        (jetkvm_host_display_idle_context_t){ 0 };
}

void jetkvm_control_set_default_video_sleep_context(
    const jetkvm_video_sleep_context_t *context)
{
    s_default_video_sleep = context ? *context :
        (jetkvm_video_sleep_context_t){ 0 };
}

void jetkvm_control_set_default_mqtt_context(
    const jetkvm_mqtt_context_t *context)
{
    s_default_mqtt = context ? *context : (jetkvm_mqtt_context_t){ 0 };
}

jetkvm_mqtt_context_t jetkvm_control_default_mqtt_context(void)
{
    return s_default_mqtt;
}

void jetkvm_control_set_default_ota_context(
    const jetkvm_ota_context_t *context)
{
    s_default_ota = context ? *context : (jetkvm_ota_context_t){ 0 };
}

jetkvm_ota_context_t jetkvm_control_default_ota_context(void)
{
    return s_default_ota;
}

jetkvm_storage_download_context_t s_default_storage_download;
void jetkvm_control_set_default_storage_download_context(
    const jetkvm_storage_download_context_t *context)
{
    s_default_storage_download = context ? *context :
        (jetkvm_storage_download_context_t){ 0 };
}

bool set_invalid_hid_params(rpc_dispatch_result_t *dispatch,
                                   const char *data)
{
    dispatch->error_code = -32602;
    dispatch->error_message = "Invalid params";
    dispatch->error_data = data;
    return true;
}

bool set_hid_unavailable(rpc_dispatch_result_t *dispatch,
                                const char *data)
{
    dispatch->error_code = -32603;
    dispatch->error_message = "Internal error";
    dispatch->error_data = data;
    return true;
}

bool require_no_rpc_params(const cJSON *params,
                                  rpc_dispatch_result_t *dispatch)
{
    if (params == NULL || cJSON_IsNull(params) ||
        (cJSON_IsObject(params) && params->child == NULL)) {
        return true;
    }
    dispatch->error_code = -32602;
    dispatch->error_message = "Invalid params";
    dispatch->error_data = "method does not accept parameters";
    return false;
}

const cJSON *rpc_param(const cJSON *params, const char *name)
{
    return cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, name) : NULL;
}

bool json_integer_in_range(const cJSON *item, int64_t minimum,
                                  int64_t maximum, int64_t *value)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        floor(item->valuedouble) != item->valuedouble ||
        item->valuedouble < (double)minimum ||
        item->valuedouble > (double)maximum) {
        return false;
    }
    *value = (int64_t)item->valuedouble;
    return true;
}

int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

bool copy_json_string(const cJSON *item, char *output,
                             size_t output_capacity, bool allow_empty)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        output == NULL || output_capacity == 0U) {
        return false;
    }
    size_t length = strlen(item->valuestring);
    if (length >= output_capacity || (!allow_empty && length == 0U)) {
        return false;
    }
    memcpy(output, item->valuestring, length + 1U);
    return true;
}

static const rpc_method_t s_rpc_methods[] = {
    { "ping", rpc_ping },
    { "reboot", rpc_reboot },
    { "getDeviceID", rpc_get_device_id },
    { "getDeviceCapabilities", rpc_get_device_capabilities },
    { "getVideoState", rpc_get_video_state },
    { "getVideoLogStatus", rpc_get_video_log_status },
    { "getLocalVersion", rpc_get_local_version },
    { "getNetworkState", rpc_get_network_state },
    { "getWifiSettings", rpc_get_wifi_settings },
    { "setWifiBand", rpc_set_wifi_band },
    { "scanWifiNetworks", rpc_scan_wifi_networks },
    { "saveWifiNetwork", rpc_save_wifi_network },
    { "removeWifiNetwork", rpc_remove_wifi_network },
    { "connectWifiNetwork", rpc_connect_wifi_network },
    { "getNetworkSettings", rpc_get_network_settings },
    { "getTailscaleStatus", rpc_get_tailscale_status },
    { "getTailscaleControlURL", rpc_get_tailscale_control_url },
    { "setTailscaleControlURL", rpc_set_tailscale_control_url },
    { "getTailscaleConfig", rpc_get_tailscale_config },
    { "setTailscaleConfig", rpc_set_tailscale_config },
    { "logoutTailscale", rpc_logout_tailscale },
    { "setNetworkSettings", rpc_set_network_settings },
    { "renewDHCPLease", rpc_renew_dhcp_lease },
    { "getMacAddressSettings", rpc_get_mac_address_settings },
    { "setMacAddressSettings", rpc_set_mac_address_settings },
    { "getPublicIPAddresses", rpc_get_public_ip_addresses },
    { "getCloudState", rpc_get_cloud_state },
    { "deregisterDevice", rpc_deregister_device },
    { "factoryReset", rpc_factory_reset },
    { "getAudioConfig", rpc_get_audio_config },
    { "setAudioConfig", rpc_set_audio_config },
    { "getTLSState", rpc_get_tls_state },
    { "setTLSState", rpc_set_tls_state },
    { "getExtraHostnames", rpc_get_extra_hostnames },
    { "setExtraHostnames", rpc_set_extra_hostnames },
    { "getBacklightSettings", rpc_get_backlight_settings },
    { "setBacklightSettings", rpc_set_backlight_settings },
    { "getDisplayRotation", rpc_get_display_rotation },
    { "setDisplayRotation", rpc_set_display_rotation },
    { "getKeyboardLayout", rpc_get_keyboard_layout },
    { "setKeyboardLayout", rpc_set_keyboard_layout },
    { "getKeyboardLedState", rpc_get_keyboard_led_state },
    { "getKeyDownState", rpc_get_key_down_state },
    { "getUSBState", rpc_get_usb_state },
    { "getUsbDevices", rpc_get_usb_devices },
    { "getUsbConfig", rpc_get_usb_config },
    { "setUsbConfig", rpc_set_usb_config },
    { "setUsbDevices", rpc_set_usb_devices },
    { "setUsbDeviceState", rpc_set_usb_device_state },
    { "getUsbEmulationState", rpc_get_usb_emulation_state },
    { "setUsbEmulationState", rpc_set_usb_emulation_state },
    { "getUsbDebuggingState", rpc_get_usb_debugging_state },
    { "setUsbDebuggingState", rpc_set_usb_debugging_state },
    { "wakeHost", rpc_wake_host },
    { "sendWOLMagicPacket", rpc_send_wol_magic_packet },
    { "getWakeOnLanDevices", rpc_get_wake_on_lan_devices },
    { "setWakeOnLanDevices", rpc_set_wake_on_lan_devices },
    { "getKeyboardMacros", rpc_get_keyboard_macros },
    { "setKeyboardMacros", rpc_set_keyboard_macros },
    { "getJigglerState", rpc_get_jiggler_state },
    { "setJigglerState", rpc_set_jiggler_state },
    { "getJigglerConfig", rpc_get_jiggler_config },
    { "setJigglerConfig", rpc_set_jiggler_config },
    { "getTimezones", rpc_get_timezones },
    { "getVirtualMediaState", rpc_get_virtual_media_state },
    { "getStorageState", rpc_get_storage_state },
    { "formatStorage", rpc_format_storage },
#if JETKVM_TF_CARD_TEST
    { "testTfCardOut", rpc_test_tf_card_out },
#endif
    { "getStorageSpace", rpc_get_storage_space },
    { "listStorageFiles", rpc_list_storage_files },
    { "checkMountUrl", rpc_check_mount_url },
    { "mountWithHTTP", rpc_mount_with_http },
    { "mountWithStorage", rpc_mount_with_storage },
    { "mountBuiltinInstaller", rpc_mount_builtin_installer },
    { "unmountImage", rpc_unmount_image },
    { "deleteStorageFile", rpc_delete_storage_file },
    { "startStorageFileUpload", rpc_start_storage_file_upload },
    { "downloadToStorage", rpc_download_to_storage },
    { "getStorageDownloadState", rpc_get_storage_download_state },
    { "keyboardReport", rpc_keyboard_report },
    { "keypressReport", rpc_keypress_report },
    { "absMouseReport", rpc_abs_mouse_report },
    { "relMouseReport", rpc_rel_mouse_report },
    { "wheelReport", rpc_wheel_report },
    { "getStreamQualityFactor", rpc_get_stream_quality },
    { "setStreamQualityFactor", rpc_set_stream_quality },
    { "setCloudUrl", rpc_set_cloud_url },
    { "getVideoCodecPreference", rpc_get_video_codec },
    { "getSupportedVideoCodecs", rpc_get_supported_video_codecs },
    { "setVideoCodecPreference", rpc_set_video_codec },
    { "setVideoStreamPaused", rpc_set_video_stream_paused },
    { "getEDID", rpc_get_edid },
    { "getEDIDPresets", rpc_get_edid_presets },
    { "setEDID", rpc_set_edid },
    { "getHostDisplayIdleMode", rpc_get_host_display_idle },
    { "setHostDisplayIdleMode", rpc_set_host_display_idle },
    { "getVideoSleepMode", rpc_get_video_sleep_mode },
    { "setVideoSleepMode", rpc_set_video_sleep_mode },
    { "getMqttSettings", rpc_get_mqtt_settings },
    { "setMqttSettings", rpc_set_mqtt_settings },
    { "getMqttStatus", rpc_get_mqtt_status },
    { "testMqttConnection", rpc_test_mqtt_connection },
    { "getUpdateStatus", rpc_get_update_status },
    { "checkUpdateComponents", rpc_check_update_components },
    { "tryUpdate", rpc_try_update },
    { "tryUpdateComponents", rpc_try_update_components },
    { "getAutoUpdateState", rpc_get_auto_update_state },
    { "setAutoUpdateState", rpc_set_auto_update_state },
    { "getDevChannelState", rpc_get_dev_channel_state },
    { "setDevChannelState", rpc_set_dev_channel_state },
    { "isUpdatePending", rpc_is_update_pending },
    { "getUpdateApiUrl", rpc_get_update_api_url },
    { "setUpdateApiUrl", rpc_set_update_api_url },
    { "getOsServicesSettings", rpc_get_os_services_settings },
    { "setOsServicesSettings", rpc_set_os_services_settings },
    { "guestClipboardGet", rpc_guest_clipboard_get },
    { "guestClipboardSet", rpc_guest_clipboard_set },
    { "guestFileDrop", rpc_guest_file_drop },
    { "guestUpdateStart", rpc_guest_update_start },
};

static const rpc_method_t *find_method(const char *name)
{
    for (size_t i = 0; i < sizeof(s_rpc_methods) / sizeof(s_rpc_methods[0]); ++i) {
        if (strcmp(name, s_rpc_methods[i].name) == 0) {
            return &s_rpc_methods[i];
        }
    }
    return NULL;
}

jetkvm_control_result_t jetkvm_control_handle_rpc_with_context(
    const jetkvm_control_context_t *context,
    const uint8_t *input, size_t input_size,
    char *output, size_t output_capacity, size_t *output_size)
{
    if (!context || !input || !output || !output_size || output_capacity == 0U) {
        return JETKVM_CONTROL_INVALID_ARGUMENT;
    }
    *output_size = 0;
    cJSON *request = cJSON_ParseWithLength((const char *)input, input_size);
    if (!request) {
        cJSON *response = new_rpc_response(NULL);
        if (!response || !add_rpc_error(response, -32700, "Parse error", NULL)) {
            cJSON_Delete(response);
            return JETKVM_CONTROL_OUTPUT_TOO_SMALL;
        }
        jetkvm_control_result_t result = render_response(
            response, output, output_capacity, output_size);
        cJSON_Delete(response);
        return result;
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "id");
    bool notification = id == NULL;
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(request, "jsonrpc");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(request, "method");
    bool valid = cJSON_IsObject(request) && cJSON_IsString(version) &&
                 strcmp(version->valuestring, "2.0") == 0 &&
                 cJSON_IsString(method) && method->valuestring[0] != '\0';
    cJSON *response = NULL;
    bool added = true;
    if (!valid) {
        response = new_rpc_response(id);
        added = response && add_rpc_error(
            response, -32600, "Invalid Request", NULL);
    } else {
        rpc_dispatch_result_t dispatch = { 0 };
        const rpc_method_t *rpc_method = find_method(method->valuestring);
        if (!rpc_method) {
            dispatch.error_code = -32601;
            dispatch.error_message = "Method not found";
        } else {
            const cJSON *params = cJSON_GetObjectItemCaseSensitive(request, "params");
            added = rpc_method->handler(context, params, &dispatch);
        }

        if (notification) {
            cJSON_Delete(dispatch.result);
            cJSON_Delete(request);
            return added ? JETKVM_CONTROL_NO_RESPONSE :
                           JETKVM_CONTROL_OUTPUT_TOO_SMALL;
        }

        response = new_rpc_response(id);
        if (!response) {
            added = false;
        } else if (dispatch.error_code) {
            added = add_rpc_error(response, dispatch.error_code,
                                  dispatch.error_message,
                                  dispatch.error_data);
        } else if (!dispatch.result ||
                   !cJSON_AddItemToObject(response, "result", dispatch.result)) {
            cJSON_Delete(dispatch.result);
            added = false;
        }
    }
    jetkvm_control_result_t result = JETKVM_CONTROL_OUTPUT_TOO_SMALL;
    if (added) {
        result = render_response(response, output, output_capacity, output_size);
    }
    cJSON_Delete(response);
    cJSON_Delete(request);
    return result;
}

jetkvm_control_result_t jetkvm_control_handle_rpc(
    const uint8_t *input, size_t input_size,
    char *output, size_t output_capacity, size_t *output_size)
{
    const jetkvm_control_context_t defaults = {
        .video = {
            .ready = false,
            .streaming = JETKVM_VIDEO_STREAM_INACTIVE,
            .error = "no_signal",
        },
        .app_version = "0.0.0",
        .system_version = "0.0.0",
        .device_id = "unknown_device_id",
        .cloud_url = "https://api.jetkvm.com",
        .cloud_app_url = "https://app.jetkvm.com",
        .stream_quality_factor = 1.0,
    };
    return jetkvm_control_handle_rpc_with_context(
        &defaults, input, input_size, output, output_capacity, output_size);
}

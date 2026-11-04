/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "control_protocol_internal.h"

#include <math.h>
#include <string.h>

static const char *safe_string(const char *value, const char *fallback)
{
    return value && value[0] ? value : fallback;
}

bool rpc_ping(const jetkvm_control_context_t *context,
                     const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    (void)context;
    (void)params;
    dispatch->result = cJSON_CreateString("pong");
    return dispatch->result != NULL;
}

static bool add_capability(cJSON *list, bool present, const char *name)
{
    if (!present) return true;
    cJSON *item = cJSON_CreateString(name);
    if (item && cJSON_AddItemToArray(list, item)) return true;
    cJSON_Delete(item);
    return false;
}

/* Optional features the UI may gate on.  This firmware has no device shell,
 * extension port, USB serial function, custom EDID, upload data channel,
 * loopback-only mode or log-level control. It reports "tls" only
 * when the build registered an HTTPS provider that can read and change the
 * TLS state: the UI shows an editable HTTPS setting for it.  getTLSState and
 * setTLSState each take the context's callback, else the default's.
 * "removable_storage": images live on a TF card whose state getStorageState
 * reports.  "wifi": the board has Wi-Fi (the Ethernet board's provider always
 * reports it unavailable).  "os_services": the build registered the OS
 * Services component, so the UI shows the OS Services settings.
 * "display_buttons": side buttons drive the local display and wake it, so
 * the UI says that a button press wakes the display.
 * "display_quarter_turns": the build registered a local display that takes
 * every setDisplayRotation value (0, 90, 180 and 270), so the UI offers four
 * orientations instead of upstream's Normal (270) and Inverted (90).
 * "usb_debugging": the build registered the USB debugging setting, so the UI
 * shows its switch (getUsbDebuggingState, setUsbDebuggingState).
 * "extra_hostnames": the web server takes other names for the device
 * (getExtraHostnames, setExtraHostnames), so the UI shows that setting. */
static cJSON *device_capabilities_json(const jetkvm_control_context_t *context)
{
    cJSON *result = cJSON_CreateArray();
    if (!result) return NULL;
    const bool tls_get = (context && context->tls.get) || s_default_tls.get;
    const bool tls_set = (context && context->tls.set) || s_default_tls.set;
    jetkvm_wifi_settings_t wifi = {0};
    const bool has_wifi = context && context->get_wifi_settings &&
        context->get_wifi_settings(&wifi, context->network_ctx) && wifi.available;
    const jetkvm_tailscale_context_t *ts = tailscale_context(context);
    if (!add_capability(result, tls_get && tls_set, "tls") ||
        !add_capability(result, context && context->virtual_media.get_state, "removable_storage") ||
        !add_capability(result, has_wifi, "wifi") ||
        !add_capability(result, s_default_os_services.get_settings != NULL,
                        "os_services") ||
        !add_capability(result, s_default_local_display.buttons, "display_buttons") ||
        !add_capability(result, s_default_local_display.set_rotation != NULL,
                        "display_quarter_turns") ||
        !add_capability(result, ts->get_status && ts->get_config && ts->set_config, "tailscale") ||
        !add_capability(result, s_default_usb_debugging.get && s_default_usb_debugging.set,
                        "usb_debugging") ||
        !add_capability(result, s_default_extra_hostnames.get && s_default_extra_hostnames.set,
                        "extra_hostnames")) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

bool rpc_get_device_capabilities(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch)
{
    (void)params;
    dispatch->result = device_capabilities_json(context);
    return dispatch->result != NULL;
}

/* appVersion is left out when empty: the firmware is one image with no
 * separate app, and the UI hides the App line without one. */
static cJSON *local_version_json(const char *app_version,
                                 const char *system_version, const char *sku)
{
    cJSON *result = cJSON_CreateObject();
    if (!result ||
        (app_version && app_version[0] &&
         !cJSON_AddStringToObject(result, "appVersion", app_version)) ||
        (sku && sku[0] && !cJSON_AddStringToObject(result, "sku", sku)) ||
        !cJSON_AddStringToObject(result, "systemVersion",
                                 safe_string(system_version, "0.0.0"))) {
        cJSON_Delete(result);
        return NULL;
    }
    return result;
}

static bool render_notification(const char *method, cJSON *params,
                                char *output, size_t capacity, size_t *length)
{
    cJSON *notification = cJSON_CreateObject();
    if (!notification || !params ||
        !cJSON_AddStringToObject(notification, "jsonrpc", "2.0") ||
        !cJSON_AddStringToObject(notification, "method", method) ||
        !cJSON_AddItemToObject(notification, "params", params)) {
        cJSON_Delete(params);
        cJSON_Delete(notification);
        return false;
    }
    bool rendered = cJSON_PrintPreallocated(notification, output, (int)capacity, false);
    cJSON_Delete(notification);
    if (rendered) *length = strlen(output);
    return rendered;
}

bool jetkvm_control_render_local_version_notification(
    const char *app_version, const char *system_version, const char *sku,
    char *output, size_t capacity, size_t *length)
{
    return render_notification("localVersion",
                               local_version_json(app_version, system_version, sku),
                               output, capacity, length);
}

bool jetkvm_control_render_capabilities_notification(
    const jetkvm_control_context_t *context,
    char *output, size_t capacity, size_t *length)
{
    return render_notification("deviceCapabilities",
                               device_capabilities_json(context),
                               output, capacity, length);
}

bool rpc_get_video_state(const jetkvm_control_context_t *context,
                                const cJSON *params,
                                rpc_dispatch_result_t *dispatch)
{
    (void)params;
    cJSON *result = cJSON_CreateObject();
    const jetkvm_video_state_t *video = &context->video;
    if (!result || !cJSON_AddBoolToObject(result, "ready", video->ready) ||
        !cJSON_AddNumberToObject(result, "streaming",
                                (double)video->streaming) ||
        ((!video->ready && safe_string(video->error, "")[0] != '\0') &&
         !cJSON_AddStringToObject(result, "error", video->error)) ||
        (video->upload_pauses_video &&
         !cJSON_AddTrueToObject(result, "uploadPausesVideo")) ||
        !cJSON_AddNumberToObject(result, "width", video->width) ||
        !cJSON_AddNumberToObject(result, "height", video->height) ||
        !cJSON_AddNumberToObject(result, "fps", video->fps)) {
        cJSON_Delete(result);
        return false;
    }
    /* The OS Services fields follow, as in the videoInputState event. */
    const jetkvm_os_services_context_t *os = &s_default_os_services;
    if (os->add_video_state && !os->add_video_state(result, os->ctx)) {
        cJSON_Delete(result);
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "OS Services unavailable";
        return true;
    }
    dispatch->result = result;
    return true;
}

bool rpc_get_video_log_status(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    (void)params;
    if (!context->get_video_log_status) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Video debugging information unavailable";
        return true;
    }
    char status[JETKVM_CONTROL_VIDEO_LOG_STATUS_CAPACITY] = { 0 };
    if (!context->get_video_log_status(
            status, sizeof(status), context->video_log_status_ctx) ||
        memchr(status, '\0', sizeof(status)) == NULL) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Video debugging information unavailable";
        return true;
    }
    dispatch->result = cJSON_CreateString(status);
    return dispatch->result != NULL;
}

bool rpc_get_local_version(const jetkvm_control_context_t *context,
                                  const cJSON *params,
                                  rpc_dispatch_result_t *dispatch)
{
    (void)params;
    dispatch->result = local_version_json(context->app_version,
                                          context->system_version, context->sku);
    return dispatch->result != NULL;
}

bool rpc_get_device_id(const jetkvm_control_context_t *context,
                              const cJSON *params,
                              rpc_dispatch_result_t *dispatch)
{
    (void)params;
    dispatch->result = cJSON_CreateString(
        safe_string(context->device_id, "unknown_device_id"));
    return dispatch->result != NULL;
}

bool rpc_get_cloud_state(const jetkvm_control_context_t *context,
                                const cJSON *params,
                                rpc_dispatch_result_t *dispatch)
{
    (void)params;
    cJSON *result = cJSON_CreateObject();
    if (!result ||
        !cJSON_AddBoolToObject(result, "connected",
                              context->cloud_registered) ||
        !cJSON_AddStringToObject(result, "url",
                                safe_string(context->cloud_url,
                                            "https://api.jetkvm.com")) ||
        !cJSON_AddStringToObject(result, "appUrl",
                                safe_string(context->cloud_app_url,
                                            "https://app.jetkvm.com"))) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

static jetkvm_audio_context_t s_audio;
void jetkvm_control_set_default_audio_context(const jetkvm_audio_context_t *context)
{ s_audio = context ? *context : (jetkvm_audio_context_t){0}; }

bool rpc_get_audio_config(const jetkvm_control_context_t *context,
                         const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    (void)context; (void)params;
    bool enabled = false;
    if (s_audio.get_enabled && !s_audio.get_enabled(&enabled, s_audio.ctx)) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Audio unavailable";
        return true;
    }
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddBoolToObject(result, "enabled", enabled)) {
        cJSON_Delete(result); return false;
    }
    dispatch->result = result; return true;
}

bool rpc_set_audio_config(const jetkvm_control_context_t *context,
                         const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    (void)context;
    const cJSON *config = cJSON_GetObjectItemCaseSensitive(params, "params");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(config, "enabled");
    if (!cJSON_IsObject(params) || cJSON_GetArraySize(params) != 1 ||
        !cJSON_IsObject(config) || cJSON_GetArraySize(config) != 1 || !cJSON_IsBool(enabled)) {
        dispatch->error_code = -32602; dispatch->error_message = "Invalid params"; return true;
    }
    if (!s_audio.set_enabled) {
        dispatch->error_code = -32603; dispatch->error_message = "Audio unavailable"; return true;
    }
    cJSON *result = cJSON_CreateNull();
    if (!result) return false;
    const char *error = s_audio.set_enabled(cJSON_IsTrue(enabled), s_audio.ctx);
    if (error) {
        cJSON_Delete(result); dispatch->error_code = -32603;
        dispatch->error_message = "Audio update failed"; dispatch->error_data = error;
    } else dispatch->result = result;
    return true;
}

bool rpc_get_stream_quality(const jetkvm_control_context_t *context,
                                   const cJSON *params,
                                   rpc_dispatch_result_t *dispatch)
{
    (void)params;
    /* 0 is Auto, as in upstream. Anything else outside the range is a
     * firmware bug; report High rather than an impossible value. */
    double factor = context->stream_quality_factor;
    if (!isfinite(factor) || factor < 0.0) {
        factor = 1.0;
    }
    dispatch->result = cJSON_CreateNumber(factor);
    return dispatch->result != NULL;
}

bool rpc_get_supported_video_codecs(const jetkvm_control_context_t *context,
                                    const cJSON *params,
                                    rpc_dispatch_result_t *dispatch)
{
    (void)context;
    (void)params;
    const char *codecs[] = { "h264" };
    dispatch->result = cJSON_CreateStringArray(codecs, 1);
    return dispatch->result != NULL;
}

bool rpc_get_video_codec(const jetkvm_control_context_t *context,
                                const cJSON *params,
                                rpc_dispatch_result_t *dispatch)
{
    (void)params;
    dispatch->result = cJSON_CreateString(
        safe_string(context->video_codec_preference, "auto"));
    return dispatch->result != NULL;
}

bool rpc_set_video_codec(const jetkvm_control_context_t *context,
                                const cJSON *params,
                                rpc_dispatch_result_t *dispatch)
{
    const cJSON *codec_item = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "codec") : NULL;
    if (!cJSON_IsString(codec_item) || !codec_item->valuestring ||
        (strcmp(codec_item->valuestring, "auto") != 0 &&
         strcmp(codec_item->valuestring, "h264") != 0 &&
         strcmp(codec_item->valuestring, "h265") != 0)) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "codec must be auto, h264, or h265";
        return true;
    }
    if (strcmp(codec_item->valuestring, "h265") == 0) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "H.265 is unsupported on ESP32-P4";
        return true;
    }
    if (!context->set_video_codec_preference) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Video codec preference unavailable";
        return true;
    }
    switch (context->set_video_codec_preference(
        codec_item->valuestring, context->set_video_codec_preference_ctx)) {
    case JETKVM_VIDEO_CODEC_CHANGE_ACCEPTED:
        dispatch->result = cJSON_CreateNull();
        return dispatch->result != NULL;
    case JETKVM_VIDEO_CODEC_CHANGE_INVALID:
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "Invalid video codec preference";
        return true;
    case JETKVM_VIDEO_CODEC_CHANGE_UNSUPPORTED:
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "Video codec is unsupported on this device";
        return true;
    case JETKVM_VIDEO_CODEC_CHANGE_FAILED:
    default:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Failed to save video codec preference";
        return true;
    }
}

bool rpc_set_video_stream_paused(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch)
{
    const cJSON *paused = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "paused") : NULL;
    if (!cJSON_IsBool(paused) || params->child != paused || paused->next) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data =
            "paused must be the only parameter and must be boolean";
        return true;
    }
    if (!context->set_video_stream_paused ||
        !context->set_video_stream_paused(
            cJSON_IsTrue(paused), context->set_video_stream_paused_ctx)) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Video stream pause control unavailable";
        return true;
    }
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}

bool rpc_set_stream_quality(const jetkvm_control_context_t *context,
                                   const cJSON *params,
                                   rpc_dispatch_result_t *dispatch)
{
    const cJSON *factor_item = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "factor") : NULL;
    if (!cJSON_IsNumber(factor_item) || !isfinite(factor_item->valuedouble) ||
        factor_item->valuedouble < 0.0) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "factor must be 0 (Auto) or a positive finite number";
        return true;
    }
    if (!context->queue_quality_change) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Stream quality control unavailable";
        return true;
    }
    switch (context->queue_quality_change(
                factor_item->valuedouble,
                context->queue_quality_change_ctx)) {
    case JETKVM_QUALITY_CHANGE_ACCEPTED:
        dispatch->result = cJSON_CreateNull();
        return dispatch->result != NULL;
    case JETKVM_QUALITY_CHANGE_INVALID:
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "Unsupported stream quality factor";
        return true;
    case JETKVM_QUALITY_CHANGE_BUSY:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Video control queue busy";
        return true;
    default:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Stream quality control unavailable";
        return true;
    }
}

bool rpc_set_cloud_url(const jetkvm_control_context_t *context,
                              const cJSON *params,
                              rpc_dispatch_result_t *dispatch)
{
    const cJSON *api_url = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "apiUrl") : NULL;
    const cJSON *app_url = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "appUrl") : NULL;
    if (!cJSON_IsString(api_url) || !api_url->valuestring[0] ||
        !cJSON_IsString(app_url) || !app_url->valuestring[0]) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "apiUrl and appUrl must be non-empty strings";
        return true;
    }
    if (!context->set_cloud_urls) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Cloud configuration unavailable";
        return true;
    }
    switch (context->set_cloud_urls(api_url->valuestring,
                                    app_url->valuestring,
                                    context->set_cloud_urls_ctx)) {
    case JETKVM_CLOUD_CONFIG_CHANGE_ACCEPTED:
        dispatch->result = cJSON_CreateNull();
        return dispatch->result != NULL;
    case JETKVM_CLOUD_CONFIG_CHANGE_INVALID:
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "Unsupported cloud URL";
        return true;
    default:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Failed to save cloud configuration";
        return true;
    }
}

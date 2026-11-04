/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "control_protocol_internal.h"

#include <math.h>
#include <string.h>

const jetkvm_edid_context_t *edid_context(
    const jetkvm_control_context_t *context)
{
    if (context && (context->edid.get || context->edid.set || context->edid.get_preset)) {
        return &context->edid;
    }
    return &s_default_edid;
}

bool rpc_get_edid_presets(const jetkvm_control_context_t *context,
                           const cJSON *params,
                           rpc_dispatch_result_t *dispatch)
{
    (void)params;
    const jetkvm_edid_context_t *control = edid_context(context);
    cJSON *result = cJSON_CreateArray();
    if (!result) {
        return false;
    }
    for (size_t index = 0U; ; ++index) {
        const char *name = NULL;
        char edid[JETKVM_CONTROL_EDID_HEX_CAPACITY] = { 0 };
        if (!control->get_preset ||
            !control->get_preset(index, &name, edid, sizeof(edid), control->ctx)) {
            cJSON_Delete(result);
            dispatch->error_code = -32603;
            dispatch->error_message = "Internal error";
            dispatch->error_data = "EDID presets unavailable";
            return true;
        }
        if (!name) {
            dispatch->result = result;
            return true;
        }
        cJSON *preset = cJSON_CreateObject();
        if (!preset || !cJSON_AddStringToObject(preset, "name", name) ||
            !cJSON_AddStringToObject(preset, "edid", edid) ||
            !cJSON_AddItemToArray(result, preset)) {
            cJSON_Delete(preset);
            cJSON_Delete(result);
            return false;
        }
    }
}

bool rpc_get_edid(const jetkvm_control_context_t *context,
                         const cJSON *params,
                         rpc_dispatch_result_t *dispatch)
{
    (void)params;
    const jetkvm_edid_context_t *control = edid_context(context);
    char edid[JETKVM_CONTROL_EDID_HEX_CAPACITY] = { 0 };
    size_t length = 0U;
    if (!control->get ||
        !control->get(edid, sizeof(edid), control->ctx) ||
        !memchr(edid, '\0', sizeof(edid))) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "EDID control unavailable";
        return true;
    }
    length = strlen(edid);
    if (length != 256U && length != 512U) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Invalid active EDID";
        return true;
    }
    dispatch->result = cJSON_CreateString(edid);
    return dispatch->result != NULL;
}

bool rpc_set_edid(const jetkvm_control_context_t *context,
                         const cJSON *params,
                         rpc_dispatch_result_t *dispatch)
{
    const cJSON *edid = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "edid") : NULL;
    if (!cJSON_IsString(edid) || !edid->valuestring) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "edid must be a hexadecimal string";
        return true;
    }
    size_t length = strlen(edid->valuestring);
    if (length != 256U && length != 512U) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "edid must contain one or two 128-byte blocks";
        return true;
    }
    for (size_t i = 0U; i < length; ++i) {
        const unsigned char value = (unsigned char)edid->valuestring[i];
        if (!((value >= '0' && value <= '9') ||
              (value >= 'a' && value <= 'f') ||
              (value >= 'A' && value <= 'F'))) {
            dispatch->error_code = -32602;
            dispatch->error_message = "Invalid params";
            dispatch->error_data = "edid contains a non-hexadecimal character";
            return true;
        }
    }

    const jetkvm_edid_context_t *control = edid_context(context);
    if (!control->set) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "EDID control unavailable";
        return true;
    }
    switch (control->set(edid->valuestring, control->ctx)) {
    case JETKVM_EDID_CHANGE_ACCEPTED:
        dispatch->result = cJSON_CreateNull();
        return dispatch->result != NULL;
    case JETKVM_EDID_CHANGE_INVALID:
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "EDID is unsupported on this device";
        return true;
    case JETKVM_EDID_CHANGE_BUSY:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "EDID control is busy";
        return true;
    case JETKVM_EDID_CHANGE_UNAVAILABLE:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "EDID control unavailable";
        return true;
    case JETKVM_EDID_CHANGE_FAILED:
    default:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Failed to apply EDID";
        return true;
    }
}

const jetkvm_host_display_idle_context_t *host_display_idle_context(
    const jetkvm_control_context_t *context)
{
    if (context &&
        (context->host_display_idle.get || context->host_display_idle.set)) {
        return &context->host_display_idle;
    }
    return &s_default_host_display_idle;
}

bool rpc_get_host_display_idle(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    (void)params;
    const jetkvm_host_display_idle_context_t *control =
        host_display_idle_context(context);
    bool enabled = false;
    if (!control->get || !control->get(&enabled, control->ctx)) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Host display idle control unavailable";
        return true;
    }
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddBoolToObject(result, "enabled", enabled)) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

bool rpc_set_host_display_idle(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const cJSON *enabled = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "enabled") : NULL;
    if (!cJSON_IsBool(enabled)) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "enabled must be a boolean";
        return true;
    }
    const jetkvm_host_display_idle_context_t *control =
        host_display_idle_context(context);
    if (!control->set) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Host display idle control unavailable";
        return true;
    }
    switch (control->set(cJSON_IsTrue(enabled), control->ctx)) {
    case JETKVM_HOST_DISPLAY_IDLE_CHANGE_ACCEPTED:
        dispatch->result = cJSON_CreateNull();
        return dispatch->result != NULL;
    case JETKVM_HOST_DISPLAY_IDLE_CHANGE_UNAVAILABLE:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Host display idle control unavailable";
        return true;
    case JETKVM_HOST_DISPLAY_IDLE_CHANGE_FAILED:
    default:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Failed to save host display idle mode";
        return true;
    }
}

const jetkvm_video_sleep_context_t *video_sleep_context(
    const jetkvm_control_context_t *context)
{
    if (context && (context->video_sleep.get || context->video_sleep.set)) {
        return &context->video_sleep;
    }
    return &s_default_video_sleep;
}

bool rpc_get_video_sleep_mode(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    (void)params;
    const jetkvm_video_sleep_context_t *control =
        video_sleep_context(context);
    bool enabled = false;
    int32_t duration = -1;
    if (!control->get) {
        cJSON *unsupported = cJSON_CreateObject();
        if (!unsupported ||
            !cJSON_AddBoolToObject(unsupported, "supported", false) ||
            !cJSON_AddBoolToObject(unsupported, "enabled", false) ||
            !cJSON_AddNumberToObject(unsupported, "duration", -1)) {
            cJSON_Delete(unsupported);
            return false;
        }
        dispatch->result = unsupported;
        return true;
    }
    if (!control->get(&enabled, &duration, control->ctx)) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Video sleep mode unavailable";
        return true;
    }
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddBoolToObject(result, "supported", true) ||
        !cJSON_AddBoolToObject(result, "enabled", enabled) ||
        !cJSON_AddNumberToObject(result, "duration", duration)) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

bool rpc_set_video_sleep_mode(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const cJSON *duration_item = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "duration") : NULL;
    if (!cJSON_IsNumber(duration_item) ||
        !isfinite(duration_item->valuedouble) ||
        floor(duration_item->valuedouble) != duration_item->valuedouble ||
        duration_item->valuedouble > (double)INT32_MAX) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "duration must be an integer number of seconds";
        return true;
    }
    int32_t duration = duration_item->valuedouble < 0.0 ? -1 :
        (int32_t)duration_item->valuedouble;
    const jetkvm_video_sleep_context_t *control =
        video_sleep_context(context);
    if (!control->set) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Video sleep mode unavailable";
        return true;
    }
    switch (control->set(duration, control->ctx)) {
    case JETKVM_VIDEO_SLEEP_CHANGE_ACCEPTED:
        dispatch->result = cJSON_CreateNull();
        return dispatch->result != NULL;
    case JETKVM_VIDEO_SLEEP_CHANGE_INVALID:
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "Unsupported video sleep duration";
        return true;
    case JETKVM_VIDEO_SLEEP_CHANGE_UNAVAILABLE:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Video sleep mode unavailable";
        return true;
    case JETKVM_VIDEO_SLEEP_CHANGE_FAILED:
    default:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Failed to save video sleep mode";
        return true;
    }
}

const jetkvm_keyboard_layout_context_t *keyboard_layout_context(
    const jetkvm_control_context_t *context)
{
    if (context && (context->keyboard_layout.get ||
                    context->keyboard_layout.set)) {
        return &context->keyboard_layout;
    }
    return &s_default_keyboard_layout;
}

bool rpc_get_keyboard_layout(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    (void)params;
    const jetkvm_keyboard_layout_context_t *layout_context =
        keyboard_layout_context(context);
    char layout[JETKVM_CONTROL_KEYBOARD_LAYOUT_CAPACITY] = { 0 };
    if (!layout_context->get ||
        !layout_context->get(layout, sizeof(layout), layout_context->ctx) ||
        !layout[0] || !memchr(layout, '\0', sizeof(layout))) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Keyboard layout unavailable";
        return true;
    }
    dispatch->result = cJSON_CreateString(layout);
    return dispatch->result != NULL;
}

bool rpc_set_keyboard_layout(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const cJSON *layout = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "layout") : NULL;
    if (!cJSON_IsString(layout) || !layout->valuestring[0]) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "layout must be a non-empty string";
        return true;
    }

    const jetkvm_keyboard_layout_context_t *layout_context =
        keyboard_layout_context(context);
    if (!layout_context->set) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Keyboard layout unavailable";
        return true;
    }
    switch (layout_context->set(layout->valuestring, layout_context->ctx)) {
    case JETKVM_KEYBOARD_LAYOUT_CHANGE_ACCEPTED:
        dispatch->result = cJSON_CreateNull();
        return dispatch->result != NULL;
    case JETKVM_KEYBOARD_LAYOUT_CHANGE_INVALID:
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "Unsupported keyboard layout";
        return true;
    default:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "Failed to save keyboard layout";
        return true;
    }
}


static bool display_result(rpc_dispatch_result_t *dispatch, const char *error)
{
    if (error) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = error;
        return true;
    }
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}

bool rpc_get_backlight_settings(const jetkvm_control_context_t *context,
    const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    (void)context;
    if (!require_no_rpc_params(params, dispatch)) return true;
    jetkvm_backlight_settings_t settings;
    const jetkvm_local_display_context_t *display = &s_default_local_display;
    if (!display->get_backlight || !display->get_backlight(&settings, display->ctx))
        return display_result(dispatch, "Local display unavailable");
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddNumberToObject(result, "max_brightness", settings.max_brightness) ||
        !cJSON_AddNumberToObject(result, "dim_after", settings.dim_after) ||
        !cJSON_AddNumberToObject(result, "off_after", settings.off_after)) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

static const cJSON *display_params(const cJSON *params)
{
    if (!cJSON_IsObject(params) || !params->child || params->child->next ||
        !params->child->string || strcmp(params->child->string, "params")) return NULL;
    return cJSON_IsObject(params->child) ? params->child : NULL;
}

bool rpc_set_backlight_settings(const jetkvm_control_context_t *context,
    const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    (void)context;
    const cJSON *value = display_params(params);
    int64_t brightness, dim, off;
    if (!value || cJSON_GetArraySize(value) != 3 ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(value, "max_brightness"), 0, 255, &brightness) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(value, "dim_after"), 0, INT32_MAX, &dim) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(value, "off_after"), 0, INT32_MAX, &off)) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "params requires integer max_brightness (0..255) and dim_after/off_after seconds (0..2147483647)";
        return true;
    }
    const jetkvm_backlight_settings_t settings = {brightness, dim, off};
    const jetkvm_local_display_context_t *display = &s_default_local_display;
    return display_result(dispatch, display->set_backlight ?
        display->set_backlight(&settings, display->ctx) : "Local display unavailable");
}

bool rpc_get_display_rotation(const jetkvm_control_context_t *context,
    const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    (void)context;
    if (!require_no_rpc_params(params, dispatch)) return true;
    uint16_t rotation;
    const jetkvm_local_display_context_t *display = &s_default_local_display;
    if (!display->get_rotation || !display->get_rotation(&rotation, display->ctx))
        return display_result(dispatch, "Local display unavailable");
    const char *text = rotation == 0 ? "0" : rotation == 90 ? "90" : rotation == 180 ? "180" : "270";
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddStringToObject(result, "rotation", text)) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

bool rpc_set_display_rotation(const jetkvm_control_context_t *context,
    const cJSON *params, rpc_dispatch_result_t *dispatch)
{
    (void)context;
    const cJSON *value = display_params(params);
    const cJSON *rotation = cJSON_GetObjectItemCaseSensitive(value, "rotation");
    const char *text = cJSON_IsString(rotation) ? rotation->valuestring : "";
    int degrees = !strcmp(text, "0") ? 0 : !strcmp(text, "90") ? 90 :
                  !strcmp(text, "180") ? 180 : !strcmp(text, "270") ? 270 : -1;
    if (!value || cJSON_GetArraySize(value) != 1 || degrees < 0) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "params.rotation must be 0, 90, 180 or 270 as a string";
        return true;
    }
    const jetkvm_local_display_context_t *display = &s_default_local_display;
    return display_result(dispatch, display->set_rotation ?
        display->set_rotation(degrees, display->ctx) : "Local display unavailable");
}

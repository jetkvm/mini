/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "control_protocol_internal.h"

#include <string.h>

static void format_usb_id(uint16_t value, char output[7])
{
    static const char hex[] = "0123456789abcdef";
    output[0] = '0';
    output[1] = 'x';
    output[2] = hex[(value >> 12U) & 0x0fU];
    output[3] = hex[(value >> 8U) & 0x0fU];
    output[4] = hex[(value >> 4U) & 0x0fU];
    output[5] = hex[value & 0x0fU];
    output[6] = '\0';
}

static bool dispatch_rpc_hid_event(const jetkvm_control_context_t *context,
                                   const jetkvm_hidrpc_event_t *event,
                                   rpc_dispatch_result_t *dispatch)
{
    if (!context->hidrpc.on_event) {
        return set_hid_unavailable(dispatch, "USB HID input unavailable");
    }
    context->hidrpc.on_event(event, context->hidrpc.ctx);
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}

bool rpc_get_keyboard_led_state(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    (void)params;
    jetkvm_keyboard_led_state_t state = { 0 };
    if (!context->get_keyboard_led_state ||
        !context->get_keyboard_led_state(&state, context->hid_state_ctx)) {
        return set_hid_unavailable(dispatch,
                                   "Keyboard LED state unavailable");
    }

    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddBoolToObject(result, "num_lock", state.num_lock) ||
        !cJSON_AddBoolToObject(result, "caps_lock", state.caps_lock) ||
        !cJSON_AddBoolToObject(result, "scroll_lock", state.scroll_lock) ||
        !cJSON_AddBoolToObject(result, "compose", state.compose) ||
        !cJSON_AddBoolToObject(result, "kana", state.kana) ||
        !cJSON_AddBoolToObject(result, "shift", state.shift)) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

bool rpc_get_key_down_state(const jetkvm_control_context_t *context,
                                   const cJSON *params,
                                   rpc_dispatch_result_t *dispatch)
{
    (void)params;
    jetkvm_hidrpc_keyboard_report_t state = { 0 };
    if (!context->get_key_down_state ||
        !context->get_key_down_state(&state, context->hid_state_ctx)) {
        return set_hid_unavailable(dispatch, "Key-down state unavailable");
    }

    cJSON *result = cJSON_CreateObject();
    cJSON *modifier = result ?
        cJSON_AddNumberToObject(result, "modifier", state.modifier) : NULL;
    cJSON *keys = result ? cJSON_AddArrayToObject(result, "keys") : NULL;
    bool added = result && modifier && keys;
    for (size_t i = 0; added && i < JETKVM_HIDRPC_KEY_COUNT; ++i) {
        cJSON *key = cJSON_CreateNumber(state.keys[i]);
        if (!key || !cJSON_AddItemToArray(keys, key)) {
            cJSON_Delete(key);
            added = false;
        }
    }
    if (!added) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

static const char *usb_state_name(jetkvm_usb_state_t state)
{
    switch (state) {
    case JETKVM_USB_STATE_CONFIGURED:
        return "configured";
    case JETKVM_USB_STATE_ATTACHED:
        return "attached";
    case JETKVM_USB_STATE_ADDRESSED:
        return "addressed";
    case JETKVM_USB_STATE_SUSPENDED:
        return "suspended";
    case JETKVM_USB_STATE_NOT_ATTACHED:
        return "not attached";
    default:
        return NULL;
    }
}

bool rpc_get_usb_state(const jetkvm_control_context_t *context,
                              const cJSON *params,
                              rpc_dispatch_result_t *dispatch)
{
    (void)params;
    jetkvm_usb_state_t state = JETKVM_USB_STATE_NOT_ATTACHED;
    if (!context->get_usb_state ||
        !context->get_usb_state(&state, context->hid_state_ctx)) {
        return set_hid_unavailable(dispatch, "USB state unavailable");
    }
    const char *name = usb_state_name(state);
    if (!name) {
        return set_hid_unavailable(dispatch, "USB state unavailable");
    }
    dispatch->result = cJSON_CreateString(name);
    return dispatch->result != NULL;
}

static bool get_usb_configuration_snapshot(
    const jetkvm_control_context_t *context,
    jetkvm_usb_configuration_snapshot_t *snapshot,
    rpc_dispatch_result_t *dispatch)
{
    memset(snapshot, 0, sizeof(*snapshot));
    if (!context->usb_configuration.get ||
        !context->usb_configuration.get(
            snapshot, context->usb_configuration.ctx) ||
        memchr(snapshot->serial_number, '\0',
               sizeof(snapshot->serial_number)) == NULL ||
        memchr(snapshot->manufacturer, '\0',
               sizeof(snapshot->manufacturer)) == NULL ||
        memchr(snapshot->product, '\0', sizeof(snapshot->product)) == NULL) {
        (void)set_hid_unavailable(dispatch,
                                  "USB configuration unavailable");
        return false;
    }
    return true;
}

static bool rpc_single_object_parameter(const cJSON *params,
                                        const char *name,
                                        const cJSON **value)
{
    if (!cJSON_IsObject(params) || params->child == NULL ||
        params->child->next != NULL || params->child->string == NULL ||
        strcmp(params->child->string, name) != 0 ||
        !cJSON_IsObject(params->child)) {
        return false;
    }
    *value = params->child;
    return true;
}

static int usb_hex_digit_value(char digit)
{
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return -1;
}

static bool parse_usb_descriptor_id(const cJSON *item, uint16_t *value)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strlen(item->valuestring) != 6U || item->valuestring[0] != '0' ||
        item->valuestring[1] != 'x') {
        return false;
    }

    uint16_t parsed = 0;
    for (size_t i = 2U; i < 6U; ++i) {
        int nibble = usb_hex_digit_value(item->valuestring[i]);
        if (nibble < 0) {
            return false;
        }
        parsed = (uint16_t)((parsed << 4U) | (uint16_t)nibble);
    }
    if (parsed == 0U) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool copy_usb_descriptor_string(const cJSON *item, char *output)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    size_t length = strlen(item->valuestring);
    if (length > JETKVM_USB_DESCRIPTOR_CONFIG_STRING_MAX_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char byte = (unsigned char)item->valuestring[i];
        if (byte < 0x20U || byte > 0x7eU) {
            return false;
        }
    }
    memcpy(output, item->valuestring, length + 1U);
    return true;
}

static bool parse_usb_descriptor_config(
    const cJSON *object, jetkvm_usb_descriptor_config_t *config)
{
    enum {
        USB_CONFIG_VENDOR_ID = 1U << 0,
        USB_CONFIG_PRODUCT_ID = 1U << 1,
        USB_CONFIG_SERIAL_NUMBER = 1U << 2,
        USB_CONFIG_MANUFACTURER = 1U << 3,
        USB_CONFIG_PRODUCT = 1U << 4,
        USB_CONFIG_ALL = (1U << 5) - 1U,
    };
    unsigned seen = 0U;

    memset(config, 0, sizeof(*config));
    if (!cJSON_IsObject(object)) {
        return false;
    }
    for (const cJSON *item = object->child; item != NULL; item = item->next) {
        unsigned field = 0U;
        if (item->string == NULL) {
            return false;
        } else if (strcmp(item->string, "vendor_id") == 0) {
            field = USB_CONFIG_VENDOR_ID;
        } else if (strcmp(item->string, "product_id") == 0) {
            field = USB_CONFIG_PRODUCT_ID;
        } else if (strcmp(item->string, "serial_number") == 0) {
            field = USB_CONFIG_SERIAL_NUMBER;
        } else if (strcmp(item->string, "manufacturer") == 0) {
            field = USB_CONFIG_MANUFACTURER;
        } else if (strcmp(item->string, "product") == 0) {
            field = USB_CONFIG_PRODUCT;
        } else {
            return false;
        }
        if ((seen & field) != 0U) {
            return false;
        }
        seen |= field;
    }
    if (seen != USB_CONFIG_ALL) {
        return false;
    }

    return parse_usb_descriptor_id(
               cJSON_GetObjectItemCaseSensitive(object, "vendor_id"),
               &config->vendor_id) &&
           parse_usb_descriptor_id(
               cJSON_GetObjectItemCaseSensitive(object, "product_id"),
               &config->product_id) &&
           copy_usb_descriptor_string(
               cJSON_GetObjectItemCaseSensitive(object, "serial_number"),
               config->serial_number) &&
           copy_usb_descriptor_string(
               cJSON_GetObjectItemCaseSensitive(object, "manufacturer"),
               config->manufacturer) &&
           copy_usb_descriptor_string(
               cJSON_GetObjectItemCaseSensitive(object, "product"),
               config->product);
}

static bool parse_usb_devices(const cJSON *object,
                              jetkvm_usb_devices_t *devices)
{
    enum {
        USB_DEVICE_ABSOLUTE_MOUSE = 1U << 0,
        USB_DEVICE_RELATIVE_MOUSE = 1U << 1,
        USB_DEVICE_KEYBOARD = 1U << 2,
        USB_DEVICE_MASS_STORAGE = 1U << 3,
        USB_DEVICE_SERIAL_CONSOLE = 1U << 4,
        USB_DEVICE_AUDIO = 1U << 5,
    };
    unsigned seen = 0U;

    memset(devices, 0, sizeof(*devices));
    if (!cJSON_IsObject(object)) {
        return false;
    }
    for (const cJSON *item = object->child; item != NULL; item = item->next) {
        unsigned field = 0U;
        bool *destination = NULL;
        if (item->string == NULL) {
            return false;
        } else if (strcmp(item->string, "absolute_mouse") == 0) {
            field = USB_DEVICE_ABSOLUTE_MOUSE;
            destination = &devices->absolute_mouse;
        } else if (strcmp(item->string, "relative_mouse") == 0) {
            field = USB_DEVICE_RELATIVE_MOUSE;
            destination = &devices->relative_mouse;
        } else if (strcmp(item->string, "keyboard") == 0) {
            field = USB_DEVICE_KEYBOARD;
            destination = &devices->keyboard;
        } else if (strcmp(item->string, "mass_storage") == 0) {
            field = USB_DEVICE_MASS_STORAGE;
            destination = &devices->mass_storage;
        } else if (strcmp(item->string, "serial_console") == 0) {
            field = USB_DEVICE_SERIAL_CONSOLE;
            destination = &devices->serial_console;
        } else if (strcmp(item->string, "audio") == 0) {
            field = USB_DEVICE_AUDIO;
            destination = &devices->audio;
        } else {
            return false;
        }
        if ((seen & field) != 0U || !cJSON_IsBool(item)) {
            return false;
        }
        seen |= field;
        *destination = cJSON_IsTrue(item);
    }
    /* Go's encoding/json binding initializes omitted bool fields to false.
     * Preserve that behavior; the browser currently sends all six fields. */
    return true;
}

static bool parse_usb_device_name(const char *name,
                                  jetkvm_usb_device_t *device)
{
    if (strcmp(name, "absoluteMouse") == 0) {
        *device = JETKVM_USB_DEVICE_ABSOLUTE_MOUSE;
    } else if (strcmp(name, "relativeMouse") == 0) {
        *device = JETKVM_USB_DEVICE_RELATIVE_MOUSE;
    } else if (strcmp(name, "keyboard") == 0) {
        *device = JETKVM_USB_DEVICE_KEYBOARD;
    } else if (strcmp(name, "massStorage") == 0) {
        *device = JETKVM_USB_DEVICE_MASS_STORAGE;
    } else if (strcmp(name, "serialConsole") == 0) {
        *device = JETKVM_USB_DEVICE_SERIAL_CONSOLE;
    } else if (strcmp(name, "audio") == 0) {
        *device = JETKVM_USB_DEVICE_AUDIO;
    } else {
        return false;
    }
    return true;
}

static bool dispatch_usb_configuration_change(
    jetkvm_usb_configuration_change_result_t result,
    rpc_dispatch_result_t *dispatch)
{
    switch (result) {
    case JETKVM_USB_CONFIGURATION_CHANGE_COMPLETED:
        dispatch->result = cJSON_CreateNull();
        return dispatch->result != NULL;
    case JETKVM_USB_CONFIGURATION_CHANGE_INVALID:
        return set_invalid_hid_params(
            dispatch, "USB configuration change rejected invalid values");
    case JETKVM_USB_CONFIGURATION_CHANGE_UNSUPPORTED:
        return set_invalid_hid_params(
            dispatch, "USB configuration change is unsupported");
    case JETKVM_USB_CONFIGURATION_CHANGE_BUSY:
        return set_hid_unavailable(
            dispatch, "USB configuration change already in progress");
    case JETKVM_USB_CONFIGURATION_CHANGE_UNAVAILABLE:
        return set_hid_unavailable(
            dispatch, "USB configuration control unavailable");
    case JETKVM_USB_CONFIGURATION_CHANGE_FAILED:
    default:
        return set_hid_unavailable(dispatch,
                                   "USB configuration change failed");
    }
}

bool rpc_set_usb_config(const jetkvm_control_context_t *context,
                               const cJSON *params,
                               rpc_dispatch_result_t *dispatch)
{
    const cJSON *value = NULL;
    jetkvm_usb_descriptor_config_t config;
    if (!rpc_single_object_parameter(params, "usbConfig", &value) ||
        !parse_usb_descriptor_config(value, &config)) {
        return set_invalid_hid_params(
            dispatch,
            "usbConfig must contain valid vendor_id, product_id, serial_number, manufacturer, and product fields");
    }
    if (!context->usb_configuration.set_descriptor_config) {
        return set_hid_unavailable(dispatch,
                                   "USB configuration control unavailable");
    }
    return dispatch_usb_configuration_change(
        context->usb_configuration.set_descriptor_config(
            &config, context->usb_configuration.ctx),
        dispatch);
}

bool rpc_set_usb_devices(const jetkvm_control_context_t *context,
                                const cJSON *params,
                                rpc_dispatch_result_t *dispatch)
{
    const cJSON *value = NULL;
    jetkvm_usb_devices_t devices;
    if (!rpc_single_object_parameter(params, "devices", &value) ||
        !parse_usb_devices(value, &devices)) {
        return set_invalid_hid_params(
            dispatch,
            "devices must contain only boolean USB device fields");
    }
    if (!context->usb_configuration.set_devices) {
        return set_hid_unavailable(dispatch,
                                   "USB configuration control unavailable");
    }
    return dispatch_usb_configuration_change(
        context->usb_configuration.set_devices(
            &devices, context->usb_configuration.ctx),
        dispatch);
}

bool rpc_set_usb_device_state(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const cJSON *device_item = NULL;
    const cJSON *enabled_item = NULL;
    unsigned seen = 0U;
    if (!cJSON_IsObject(params)) {
        return set_invalid_hid_params(
            dispatch,
            "device and enabled must be the only parameters");
    }
    for (const cJSON *item = params->child; item != NULL; item = item->next) {
        if (item->string != NULL && strcmp(item->string, "device") == 0 &&
            (seen & 1U) == 0U) {
            device_item = item;
            seen |= 1U;
        } else if (item->string != NULL &&
                   strcmp(item->string, "enabled") == 0 &&
                   (seen & 2U) == 0U) {
            enabled_item = item;
            seen |= 2U;
        } else {
            return set_invalid_hid_params(
                dispatch,
                "device and enabled must be the only parameters");
        }
    }

    jetkvm_usb_device_t device = JETKVM_USB_DEVICE_ABSOLUTE_MOUSE;
    if (seen != 3U || !cJSON_IsString(device_item) ||
        device_item->valuestring == NULL || !cJSON_IsBool(enabled_item) ||
        !parse_usb_device_name(device_item->valuestring, &device)) {
        return set_invalid_hid_params(
            dispatch,
            "device must be a supported name and enabled must be boolean");
    }
    if (!context->usb_configuration.set_device_state) {
        return set_hid_unavailable(dispatch,
                                   "USB configuration control unavailable");
    }
    return dispatch_usb_configuration_change(
        context->usb_configuration.set_device_state(
            device, cJSON_IsTrue(enabled_item),
            context->usb_configuration.ctx),
        dispatch);
}

bool rpc_get_usb_devices(const jetkvm_control_context_t *context,
                                const cJSON *params,
                                rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    jetkvm_usb_configuration_snapshot_t snapshot;
    if (!get_usb_configuration_snapshot(context, &snapshot, dispatch)) {
        return true;
    }

    cJSON *result = cJSON_CreateObject();
    if (!result ||
        !cJSON_AddBoolToObject(result, "absolute_mouse",
                              snapshot.absolute_mouse) ||
        !cJSON_AddBoolToObject(result, "relative_mouse",
                              snapshot.relative_mouse) ||
        !cJSON_AddBoolToObject(result, "keyboard", snapshot.keyboard) ||
        !cJSON_AddBoolToObject(result, "mass_storage",
                              snapshot.mass_storage) ||
        !cJSON_AddBoolToObject(result, "serial_console",
                              snapshot.serial_console) ||
        !cJSON_AddBoolToObject(result, "audio", snapshot.audio)) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

bool rpc_get_usb_config(const jetkvm_control_context_t *context,
                               const cJSON *params,
                               rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    jetkvm_usb_configuration_snapshot_t snapshot;
    if (!get_usb_configuration_snapshot(context, &snapshot, dispatch)) {
        return true;
    }

    char vendor_id[7];
    char product_id[7];
    format_usb_id(snapshot.vendor_id, vendor_id);
    format_usb_id(snapshot.product_id, product_id);

    cJSON *result = cJSON_CreateObject();
    if (!result ||
        !cJSON_AddStringToObject(result, "vendor_id", vendor_id) ||
        !cJSON_AddStringToObject(result, "product_id", product_id) ||
        !cJSON_AddStringToObject(result, "serial_number",
                                snapshot.serial_number) ||
        !cJSON_AddStringToObject(result, "manufacturer",
                                snapshot.manufacturer) ||
        !cJSON_AddStringToObject(result, "product", snapshot.product)) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

bool rpc_get_usb_emulation_state(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    jetkvm_usb_configuration_snapshot_t snapshot;
    if (!get_usb_configuration_snapshot(context, &snapshot, dispatch)) {
        return true;
    }
    dispatch->result = cJSON_CreateBool(snapshot.emulation_enabled);
    return dispatch->result != NULL;
}

bool rpc_set_usb_emulation_state(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const cJSON *enabled = cJSON_IsObject(params) ?
        cJSON_GetObjectItemCaseSensitive(params, "enabled") : NULL;
    if (!cJSON_IsBool(enabled) || params->child != enabled ||
        enabled->next != NULL) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "enabled must be the only parameter and must be boolean";
        return true;
    }
    if (!context->usb_configuration.set_emulation_enabled) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "USB emulation control unavailable";
        return true;
    }
    switch (context->usb_configuration.set_emulation_enabled(
        cJSON_IsTrue(enabled), context->usb_configuration.ctx)) {
    case JETKVM_USB_EMULATION_CHANGE_ACCEPTED:
        dispatch->result = cJSON_CreateNull();
        return dispatch->result != NULL;
    case JETKVM_USB_EMULATION_CHANGE_BUSY:
        dispatch->error_code = -32000;
        dispatch->error_message = "USB emulation control busy";
        dispatch->error_data = "Another control operation is in progress; try again";
        return true;
    case JETKVM_USB_EMULATION_CHANGE_UNAVAILABLE:
    default:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "USB emulation control unavailable";
        return true;
    }
}

jetkvm_usb_debugging_context_t s_default_usb_debugging;

void jetkvm_control_set_default_usb_debugging_context(
    const jetkvm_usb_debugging_context_t *context)
{
    s_default_usb_debugging = context ? *context :
        (jetkvm_usb_debugging_context_t){ 0 };
}

/* {"enabled": the stored setting, "active": whether the USB Serial/JTAG
 * port is connected now}. They differ until the setting takes effect at the
 * next restart. */
static bool usb_debugging_state(rpc_dispatch_result_t *dispatch)
{
    const jetkvm_usb_debugging_context_t *usb = &s_default_usb_debugging;
    bool enabled = false;
    bool active = false;
    if (!usb->get || !usb->get(&enabled, &active, usb->ctx)) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "USB debugging setting unavailable";
        return true;
    }
    cJSON *result = cJSON_CreateObject();
    if (!result || !cJSON_AddBoolToObject(result, "enabled", enabled) ||
        !cJSON_AddBoolToObject(result, "active", active)) {
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

bool rpc_get_usb_debugging_state(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    (void)context;
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    return usb_debugging_state(dispatch);
}

bool rpc_set_usb_debugging_state(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    (void)context;
    const cJSON *enabled = rpc_param(params, "enabled");
    if (!cJSON_IsBool(enabled) || params->child != enabled ||
        enabled->next != NULL) {
        dispatch->error_code = -32602;
        dispatch->error_message = "Invalid params";
        dispatch->error_data = "enabled must be the only parameter and must be boolean";
        return true;
    }
    const jetkvm_usb_debugging_context_t *usb = &s_default_usb_debugging;
    const char *error = usb->set ? usb->set(cJSON_IsTrue(enabled), usb->ctx) :
                                   "USB debugging setting unavailable";
    if (error) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = error;
        return true;
    }
    return usb_debugging_state(dispatch);
}

bool rpc_wake_host(const jetkvm_control_context_t *context,
                          const cJSON *params,
                          rpc_dispatch_result_t *dispatch)
{
    (void)params;
    if (!context->wake_host) {
        return set_hid_unavailable(dispatch, "USB wake unavailable");
    }

    switch (context->wake_host(context->hid_state_ctx)) {
    case JETKVM_USB_WAKE_SUCCEEDED:
    case JETKVM_USB_WAKE_NOOP:
        dispatch->result = cJSON_CreateNull();
        return dispatch->result != NULL;
    case JETKVM_USB_WAKE_NOT_ENABLED:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data =
            "USB remote wakeup was not enabled by the host";
        return true;
    case JETKVM_USB_WAKE_BUSY:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "USB wake request already in progress";
        return true;
    case JETKVM_USB_WAKE_UNAVAILABLE:
        return set_hid_unavailable(dispatch, "USB wake unavailable");
    case JETKVM_USB_WAKE_FAILED:
    default:
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "USB remote wakeup failed";
        return true;
    }
}

bool rpc_keyboard_report(const jetkvm_control_context_t *context,
                                const cJSON *params,
                                rpc_dispatch_result_t *dispatch)
{
    int64_t modifier = 0;
    const cJSON *keys = rpc_param(params, "keys");
    if (!json_integer_in_range(rpc_param(params, "modifier"), 0, UINT8_MAX,
                               &modifier) ||
        !cJSON_IsArray(keys) ||
        cJSON_GetArraySize(keys) != JETKVM_HIDRPC_KEY_COUNT) {
        return set_invalid_hid_params(
            dispatch, "modifier must be uint8 and keys must contain 6 uint8 values");
    }

    jetkvm_hidrpc_event_t event = {
        .type = JETKVM_HIDRPC_EVENT_KEYBOARD_REPORT,
        .data.keyboard.modifier = (uint8_t)modifier,
    };
    for (size_t i = 0; i < JETKVM_HIDRPC_KEY_COUNT; ++i) {
        int64_t key = 0;
        if (!json_integer_in_range(cJSON_GetArrayItem(keys, (int)i), 0,
                                   UINT8_MAX, &key)) {
            return set_invalid_hid_params(
                dispatch, "modifier must be uint8 and keys must contain 6 uint8 values");
        }
        event.data.keyboard.keys[i] = (uint8_t)key;
    }
    return dispatch_rpc_hid_event(context, &event, dispatch);
}

bool rpc_keypress_report(const jetkvm_control_context_t *context,
                                const cJSON *params,
                                rpc_dispatch_result_t *dispatch)
{
    int64_t key = 0;
    const cJSON *press = rpc_param(params, "press");
    if (!json_integer_in_range(rpc_param(params, "key"), 0, UINT8_MAX, &key) ||
        !cJSON_IsBool(press)) {
        return set_invalid_hid_params(
            dispatch, "key must be uint8 and press must be boolean");
    }
    jetkvm_hidrpc_event_t event = {
        .type = JETKVM_HIDRPC_EVENT_KEYPRESS_REPORT,
        .data.keypress = {
            .key = (uint8_t)key,
            .pressed = cJSON_IsTrue(press),
        },
    };
    return dispatch_rpc_hid_event(context, &event, dispatch);
}

bool rpc_abs_mouse_report(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch)
{
    int64_t x = 0;
    int64_t y = 0;
    int64_t buttons = 0;
    if (!json_integer_in_range(rpc_param(params, "x"), INT32_MIN, INT32_MAX,
                               &x) ||
        !json_integer_in_range(rpc_param(params, "y"), INT32_MIN, INT32_MAX,
                               &y) ||
        !json_integer_in_range(rpc_param(params, "buttons"), 0, UINT8_MAX,
                               &buttons)) {
        return set_invalid_hid_params(
            dispatch, "x/y must be int32 and buttons must be uint8");
    }
    jetkvm_hidrpc_event_t event = {
        .type = JETKVM_HIDRPC_EVENT_ABS_MOUSE_REPORT,
        .data.abs_mouse = {
            .x = (int32_t)x,
            .y = (int32_t)y,
            .buttons = (uint8_t)buttons,
        },
    };
    return dispatch_rpc_hid_event(context, &event, dispatch);
}

bool rpc_rel_mouse_report(const jetkvm_control_context_t *context,
                                 const cJSON *params,
                                 rpc_dispatch_result_t *dispatch)
{
    int64_t dx = 0;
    int64_t dy = 0;
    int64_t buttons = 0;
    if (!json_integer_in_range(rpc_param(params, "dx"), INT8_MIN, INT8_MAX,
                               &dx) ||
        !json_integer_in_range(rpc_param(params, "dy"), INT8_MIN, INT8_MAX,
                               &dy) ||
        !json_integer_in_range(rpc_param(params, "buttons"), 0, UINT8_MAX,
                               &buttons)) {
        return set_invalid_hid_params(
            dispatch, "dx/dy must be int8 and buttons must be uint8");
    }
    jetkvm_hidrpc_event_t event = {
        .type = JETKVM_HIDRPC_EVENT_REL_MOUSE_REPORT,
        .data.rel_mouse = {
            .dx = (int8_t)dx,
            .dy = (int8_t)dy,
            .buttons = (uint8_t)buttons,
        },
    };
    return dispatch_rpc_hid_event(context, &event, dispatch);
}

bool rpc_wheel_report(const jetkvm_control_context_t *context,
                             const cJSON *params,
                             rpc_dispatch_result_t *dispatch)
{
    int64_t wheel_y = 0;
    int64_t wheel_x = 0;
    if (!json_integer_in_range(rpc_param(params, "wheelY"), INT8_MIN,
                               INT8_MAX, &wheel_y) ||
        !json_integer_in_range(rpc_param(params, "wheelX"), INT8_MIN,
                               INT8_MAX, &wheel_x)) {
        return set_invalid_hid_params(
            dispatch, "wheelY/wheelX must be int8");
    }
    jetkvm_hidrpc_event_t event = {
        .type = JETKVM_HIDRPC_EVENT_WHEEL_REPORT,
        .data.wheel = {
            .wheel_y = (int8_t)wheel_y,
            .wheel_x = (int8_t)wheel_x,
        },
    };
    return dispatch_rpc_hid_event(context, &event, dispatch);
}

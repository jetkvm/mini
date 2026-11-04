/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "rpc_events.h"

#include <stdio.h>
#include <string.h>

static const char *json_bool(bool value)
{
    return value ? "true" : "false";
}

static const char *usb_state_name(webrtc_rpc_usb_state_t state)
{
    switch (state) {
    case WEBRTC_RPC_USB_ATTACHED:
        return "attached";
    case WEBRTC_RPC_USB_ADDRESSED:
        return "addressed";
    case WEBRTC_RPC_USB_CONFIGURED:
        return "configured";
    case WEBRTC_RPC_USB_SUSPENDED:
        return "suspended";
    default:
        return "not attached";
    }
}

webrtc_rpc_usb_state_t webrtc_rpc_usb_state_from_bus(
    bool initialized, bool connected, bool mounted, bool suspended)
{
    if (!initialized || !connected) {
        return WEBRTC_RPC_USB_NOT_ATTACHED;
    }
    if (suspended) {
        return WEBRTC_RPC_USB_SUSPENDED;
    }
    if (mounted) {
        return WEBRTC_RPC_USB_CONFIGURED;
    }
    /* TinyUSB cannot distinguish attached from addressed before mount. */
    return WEBRTC_RPC_USB_ATTACHED;
}

uint32_t webrtc_rpc_event_hid_diff(
    const webrtc_rpc_hid_state_t *previous,
    const webrtc_rpc_hid_state_t *current,
    bool previous_valid)
{
    if (!current) {
        return 0;
    }
    if (!previous_valid || !previous) {
        return WEBRTC_RPC_EVENT_HID_MASK;
    }

    uint32_t changed = 0;
    if (previous->usb != current->usb) {
        changed |= WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_USB_STATE);
    }
    const uint8_t shift_mask = 0x02U | 0x20U;
    if (previous->keyboard_leds != current->keyboard_leds ||
        (previous->modifier & shift_mask) !=
            (current->modifier & shift_mask)) {
        changed |= WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_KEYBOARD_LEDS);
    }
    if (previous->modifier != current->modifier ||
        previous->keyboard_revision != current->keyboard_revision ||
        memcmp(previous->keys, current->keys, sizeof(current->keys)) != 0) {
        changed |= WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_KEYS_DOWN);
    }
    return changed;
}

static bool finish_render(int written, size_t capacity, size_t *output_size)
{
    if (written < 0 || (size_t)written >= capacity) {
        return false;
    }
    *output_size = (size_t)written;
    return true;
}

bool webrtc_rpc_event_render(
    webrtc_rpc_event_kind_t kind,
    const webrtc_rpc_event_snapshot_t *snapshot,
    char *output, size_t output_capacity, size_t *output_size)
{
    if (!snapshot || !output || output_capacity == 0 || !output_size) {
        return false;
    }
    *output_size = 0;
    int written = -1;
    switch (kind) {
    case WEBRTC_RPC_EVENT_VIDEO_INPUT: {
        /* Present only while an upload to the TF card holds video off. */
        const char *upload_pauses = snapshot->video.upload_pauses ?
            "\"uploadPausesVideo\":true," : "";
        if (snapshot->video.ready) {
            written = snprintf(
                output, output_capacity,
                "{\"jsonrpc\":\"2.0\",\"method\":\"videoInputState\","
                "\"params\":{\"ready\":true,\"streaming\":%u,"
                "%s\"width\":%u,\"height\":%u,\"fps\":%u}}",
                (unsigned)snapshot->video.streaming, upload_pauses,
                snapshot->video.width, snapshot->video.height,
                snapshot->video.fps);
        } else {
            written = snprintf(
                output, output_capacity,
                "{\"jsonrpc\":\"2.0\",\"method\":\"videoInputState\","
                "\"params\":{\"ready\":false,\"streaming\":%u,"
                "%s%s\"width\":0,\"height\":0,\"fps\":0}}",
                (unsigned)snapshot->video.streaming,
                snapshot->video.switching ? "" : "\"error\":\"no_signal\",",
                upload_pauses);
        }
        break;
    }
    case WEBRTC_RPC_EVENT_USB_STATE:
        written = snprintf(
            output, output_capacity,
            "{\"jsonrpc\":\"2.0\",\"method\":\"usbState\","
            "\"params\":\"%s\"}",
            usb_state_name(snapshot->hid.usb));
        break;
    case WEBRTC_RPC_EVENT_KEYBOARD_LEDS: {
        uint8_t leds = snapshot->hid.keyboard_leds;
        bool shift = (snapshot->hid.modifier & (0x02U | 0x20U)) != 0U;
        written = snprintf(
            output, output_capacity,
            "{\"jsonrpc\":\"2.0\",\"method\":\"keyboardLedState\","
            "\"params\":{\"num_lock\":%s,\"caps_lock\":%s,"
            "\"scroll_lock\":%s,\"compose\":%s,\"kana\":%s,"
            "\"shift\":%s}}",
            json_bool((leds & 0x01U) != 0U),
            json_bool((leds & 0x02U) != 0U),
            json_bool((leds & 0x04U) != 0U),
            json_bool((leds & 0x08U) != 0U),
            json_bool((leds & 0x10U) != 0U), json_bool(shift));
        break;
    }
    case WEBRTC_RPC_EVENT_KEYS_DOWN:
        written = snprintf(
            output, output_capacity,
            "{\"jsonrpc\":\"2.0\",\"method\":\"keysDownState\","
            "\"params\":{\"modifier\":%u,\"keys\":[%u,%u,%u,%u,%u,%u]}}",
            snapshot->hid.modifier,
            snapshot->hid.keys[0], snapshot->hid.keys[1],
            snapshot->hid.keys[2], snapshot->hid.keys[3],
            snapshot->hid.keys[4], snapshot->hid.keys[5]);
        break;
    default:
        return false;
    }
    return finish_render(written, output_capacity, output_size);
}

bool webrtc_rpc_event_add_params(
    char *output, size_t output_capacity, size_t *output_size,
    webrtc_rpc_event_members_fn render)
{
    if (!output || !output_size || !render) {
        return false;
    }
    size_t size = *output_size;
    if (size < 2U || size >= output_capacity ||
        output[size - 2U] != '}' || output[size - 1U] != '}') {
        return false;
    }
    /* The members go where params' brace was; the two braces follow them. */
    char *members = output + size - 2U;
    size_t room = output_capacity - size;
    size_t added = 0;
    bool fits = render(members, room, &added) && added < room;
    if (!fits) {
        added = 0;
    }
    memcpy(members + added, "}}", 3U);
    *output_size = size + added;
    return fits;
}

bool webrtc_hidrpc_state_render(
    webrtc_hidrpc_state_kind_t kind,
    const webrtc_rpc_hid_state_t *hid,
    bool macro_active, bool macro_is_paste,
    uint8_t *output, size_t output_capacity, size_t *output_size)
{
    if (!hid || !output || !output_size) {
        return false;
    }
    *output_size = 0;
    switch (kind) {
    case WEBRTC_HIDRPC_STATE_KEYBOARD_LEDS:
        if (output_capacity < 2U) {
            return false;
        }
        output[0] = 0x32U;
        output[1] = hid->keyboard_leds;
        if (hid->modifier & (0x02U | 0x20U)) {
            output[1] |= 0x40U;
        }
        *output_size = 2U;
        return true;
    case WEBRTC_HIDRPC_STATE_KEYS_DOWN:
        if (output_capacity < 2U + sizeof(hid->keys)) {
            return false;
        }
        output[0] = 0x33U;
        output[1] = hid->modifier;
        memcpy(output + 2U, hid->keys, sizeof(hid->keys));
        *output_size = 2U + sizeof(hid->keys);
        return true;
    case WEBRTC_HIDRPC_STATE_KEYBOARD_MACRO:
        if (output_capacity < 3U) {
            return false;
        }
        output[0] = 0x34U;
        output[1] = macro_active ? 1U : 0U;
        output[2] = macro_is_paste ? 1U : 0U;
        *output_size = 3U;
        return true;
    default:
        return false;
    }
}

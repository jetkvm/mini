/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "control_protocol_internal.h"

#include <string.h>

static bool parse_mac_address(const char *text, uint8_t mac[6])
{
    if (text == NULL || strlen(text) != 17U) {
        return false;
    }
    for (size_t i = 0U; i < 6U; ++i) {
        size_t offset = i * 3U;
        int high = hex_nibble(text[offset]);
        int low = hex_nibble(text[offset + 1U]);
        if (high < 0 || low < 0 ||
            (i < 5U && text[offset + 2U] != ':')) {
            return false;
        }
        mac[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool valid_optional_ipv4(const char *text)
{
    if (text == NULL) {
        return false;
    }
    if (text[0] == '\0') {
        return true;
    }
    const char *cursor = text;
    for (unsigned part = 0U; part < 4U; ++part) {
        unsigned value = 0U;
        unsigned digits = 0U;
        while (*cursor >= '0' && *cursor <= '9') {
            value = value * 10U + (unsigned)(*cursor - '0');
            cursor++;
            digits++;
            if (digits > 3U || value > 255U) {
                return false;
            }
        }
        if (digits == 0U) {
            return false;
        }
        if (part < 3U) {
            if (*cursor != '.') {
                return false;
            }
            cursor++;
        }
    }
    return *cursor == '\0';
}

bool rpc_get_timezones(const jetkvm_control_context_t *context,
                              const cJSON *params,
                              rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    if (!context->jiggler.get_config) {
        return set_hid_unavailable(dispatch, "Mouse jiggler unavailable");
    }
    cJSON *result = cJSON_CreateArray();
    cJSON *utc = cJSON_CreateString(JETKVM_CONTROL_JIGGLER_DEFAULT_TIMEZONE);
    if (!result || !utc || !cJSON_AddItemToArray(result, utc)) {
        cJSON_Delete(utc);
        cJSON_Delete(result);
        return false;
    }
    dispatch->result = result;
    return true;
}

static bool wol_device_is_bounded_and_valid(
    const jetkvm_control_wol_device_t *device)
{
    if (device == NULL ||
        memchr(device->name, '\0', sizeof(device->name)) == NULL ||
        memchr(device->mac_address, '\0',
               sizeof(device->mac_address)) == NULL ||
        memchr(device->broadcast_ip, '\0',
               sizeof(device->broadcast_ip)) == NULL ||
        device->name[0] == '\0') {
        return false;
    }
    uint8_t mac[6];
    return parse_mac_address(device->mac_address, mac) &&
           valid_optional_ipv4(device->broadcast_ip);
}

bool rpc_get_wake_on_lan_devices(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    if (!require_no_rpc_params(params, dispatch)) {
        return true;
    }
    jetkvm_control_wol_device_list_t list = {0};
    if (!context->wol_devices.get ||
        !context->wol_devices.get(&list, context->wol_devices.ctx) ||
        list.count > JETKVM_CONTROL_WOL_MAX_DEVICES) {
        return set_hid_unavailable(
            dispatch, "Wake-on-LAN devices unavailable");
    }

    cJSON *result = cJSON_CreateArray();
    if (!result) {
        return false;
    }
    for (size_t i = 0U; i < list.count; ++i) {
        const jetkvm_control_wol_device_t *device = &list.devices[i];
        if (!wol_device_is_bounded_and_valid(device)) {
            cJSON_Delete(result);
            return set_hid_unavailable(
                dispatch, "Wake-on-LAN device storage is invalid");
        }
        cJSON *item = cJSON_CreateObject();
        if (!item ||
            !cJSON_AddStringToObject(item, "name", device->name) ||
            !cJSON_AddStringToObject(item, "macAddress",
                                    device->mac_address) ||
            (device->broadcast_ip[0] != '\0' &&
             !cJSON_AddStringToObject(item, "broadcastIP",
                                     device->broadcast_ip)) ||
            !cJSON_AddItemToArray(result, item)) {
            cJSON_Delete(item);
            cJSON_Delete(result);
            return false;
        }
    }
    dispatch->result = result;
    return true;
}

bool rpc_set_wake_on_lan_devices(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const cJSON *wrapper = rpc_param(params, "params");
    const cJSON *devices = rpc_param(wrapper, "devices");
    size_t outer_count = 0U;
    for (const cJSON *item = cJSON_IsObject(params) ? params->child : NULL;
         item != NULL; item = item->next) {
        if (item != wrapper) {
            return set_invalid_hid_params(
                dispatch, "params.devices expected");
        }
        outer_count++;
    }
    size_t wrapper_count = 0U;
    for (const cJSON *item = cJSON_IsObject(wrapper) ? wrapper->child : NULL;
         item != NULL; item = item->next) {
        if (item != devices) {
            return set_invalid_hid_params(
                dispatch, "params.devices expected");
        }
        wrapper_count++;
    }
    if (outer_count != 1U || wrapper_count != 1U ||
        !cJSON_IsArray(devices) ||
        cJSON_GetArraySize(devices) > (int)JETKVM_CONTROL_WOL_MAX_DEVICES) {
        return set_invalid_hid_params(
            dispatch, "params.devices must contain at most 16 devices");
    }

    jetkvm_control_wol_device_list_t list = {0};
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, devices) {
        const cJSON *name = rpc_param(item, "name");
        const cJSON *mac_address = rpc_param(item, "macAddress");
        const cJSON *broadcast_ip = rpc_param(item, "broadcastIP");
        size_t member_count = 0U;
        for (const cJSON *member = cJSON_IsObject(item) ? item->child : NULL;
             member != NULL; member = member->next) {
            if (member != name && member != mac_address &&
                member != broadcast_ip) {
                return set_invalid_hid_params(
                    dispatch, "invalid Wake-on-LAN device");
            }
            member_count++;
        }
        jetkvm_control_wol_device_t *device =
            &list.devices[list.count];
        if (!cJSON_IsObject(item) || member_count < 2U ||
            member_count > 3U ||
            !copy_json_string(name, device->name, sizeof(device->name),
                              false) ||
            !copy_json_string(mac_address, device->mac_address,
                              sizeof(device->mac_address), false) ||
            (broadcast_ip != NULL &&
             !copy_json_string(broadcast_ip, device->broadcast_ip,
                               sizeof(device->broadcast_ip), true)) ||
            !wol_device_is_bounded_and_valid(device)) {
            return set_invalid_hid_params(
                dispatch, "invalid Wake-on-LAN device");
        }
        list.count++;
    }

    if (!context->wol_devices.set) {
        return set_hid_unavailable(
            dispatch, "Wake-on-LAN devices unavailable");
    }
    if (!context->wol_devices.set(&list, context->wol_devices.ctx)) {
        return set_hid_unavailable(
            dispatch, "failed to persist Wake-on-LAN devices");
    }
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}

bool rpc_send_wol_magic_packet(
    const jetkvm_control_context_t *context, const cJSON *params,
    rpc_dispatch_result_t *dispatch)
{
    const cJSON *mac_item = rpc_param(params, "macAddress");
    const cJSON *broadcast_item = rpc_param(params, "broadcastIP");
    size_t count = 0U;
    for (const cJSON *item = cJSON_IsObject(params) ? params->child : NULL;
         item != NULL; item = item->next) {
        if (item != mac_item && item != broadcast_item) {
            return set_invalid_hid_params(
                dispatch, "macAddress and optional broadcastIP expected");
        }
        count++;
    }
    uint8_t mac[6];
    if (!cJSON_IsString(mac_item) || mac_item->valuestring == NULL ||
        !parse_mac_address(mac_item->valuestring, mac) || count < 1U ||
        count > 2U ||
        (broadcast_item != NULL &&
         (!cJSON_IsString(broadcast_item) ||
          broadcast_item->valuestring == NULL))) {
        return set_invalid_hid_params(
            dispatch, "invalid MAC address or broadcast IP");
    }
    if (!context->send_wol) {
        return set_hid_unavailable(dispatch, "Wake-on-LAN unavailable");
    }
    const char *broadcast = broadcast_item != NULL ?
        broadcast_item->valuestring : "";
    if (!context->send_wol(mac, broadcast, context->network_ctx)) {
        dispatch->error_code = -32603;
        dispatch->error_message = "Internal error";
        dispatch->error_data = "failed to send Wake-on-LAN packet";
        return true;
    }
    dispatch->result = cJSON_CreateNull();
    return dispatch->result != NULL;
}

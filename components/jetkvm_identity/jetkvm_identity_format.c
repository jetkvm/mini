/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_identity.h"

bool jetkvm_device_id_format(
    const uint8_t mac[JETKVM_DEVICE_ID_MAC_BYTES],
    char *output, size_t output_capacity)
{
    static const char hex[] = "0123456789abcdef";
    if (!mac || !output || output_capacity < JETKVM_DEVICE_ID_CAPACITY) {
        return false;
    }
    for (size_t i = 0; i < JETKVM_DEVICE_ID_MAC_BYTES; ++i) {
        output[i * 2U] = hex[mac[i] >> 4U];
        output[i * 2U + 1U] = hex[mac[i] & 0x0fU];
    }
    output[JETKVM_DEVICE_ID_LENGTH] = '\0';
    return true;
}

bool jetkvm_custom_mac_valid(const uint8_t mac[JETKVM_DEVICE_ID_MAC_BYTES])
{
    if (!mac || (mac[0] & 0x01U)) {
        return false;
    }
    bool zero = true, ones = true;
    for (size_t i = 0; i < JETKVM_DEVICE_ID_MAC_BYTES; ++i) {
        zero = zero && mac[i] == 0x00U;
        ones = ones && mac[i] == 0xffU;
    }
    return !zero && !ones;
}

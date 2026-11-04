/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JETKVM_DEVICE_ID_MAC_BYTES 6U
#define JETKVM_DEVICE_ID_LENGTH (JETKVM_DEVICE_ID_MAC_BYTES * 2U)
#define JETKVM_DEVICE_ID_CAPACITY (JETKVM_DEVICE_ID_LENGTH + 1U)

/** Format the six-byte factory MAC as a lowercase, separator-free device ID. */
bool jetkvm_device_id_format(
    const uint8_t mac[JETKVM_DEVICE_ID_MAC_BYTES],
    char *output, size_t output_capacity);

/** Read the immutable factory eFuse MAC and return its stable JetKVM device ID. */
esp_err_t jetkvm_device_id_get(char *output, size_t output_capacity);

/**
 * True when `mac` can be the factory MAC of an interface: not all zero, not
 * all 0xff and unicast. The eFuse field USER_DATA.MAC_CUSTOM has no CRC on
 * the P4, so this is the only check of what the factory wrote.
 */
bool jetkvm_custom_mac_valid(const uint8_t mac[JETKVM_DEVICE_ID_MAC_BYTES]);

/**
 * Read the factory MAC that production writes to eFuse USER_DATA.MAC_CUSTOM.
 * Returns true and fills `mac` only when the field holds a valid MAC (see
 * jetkvm_custom_mac_valid). The device ID does not use this MAC.
 */
bool jetkvm_custom_mac_get(uint8_t mac[JETKVM_DEVICE_ID_MAC_BYTES]);

#ifdef __cplusplus
}
#endif

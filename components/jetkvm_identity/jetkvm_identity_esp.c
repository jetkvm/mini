/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_identity.h"

#include <string.h>

#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_mac.h"
#include "sdkconfig.h"

esp_err_t jetkvm_device_id_get(char *output, size_t output_capacity)
{
    if (!output || output_capacity < JETKVM_DEVICE_ID_CAPACITY) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mac[JETKVM_DEVICE_ID_MAC_BYTES];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        output[0] = '\0';
        return err;
    }
    if (!jetkvm_device_id_format(mac, output, output_capacity)) {
        output[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

bool jetkvm_custom_mac_get(uint8_t mac[JETKVM_DEVICE_ID_MAC_BYTES])
{
    if (!mac) {
        return false;
    }
#if CONFIG_JETKVM_BENCH_CUSTOM_MAC
    /* Bench builds only: a locally administered address in place of eFuse. */
    static const uint8_t bench_mac[JETKVM_DEVICE_ID_MAC_BYTES] = {
        0x02, 0x4a, 0x4b, 0x00, 0x00, 0x01,
    };
    memcpy(mac, bench_mac, sizeof(bench_mac));
    return true;
#else
    /* The field that esp_efuse_mac_get_custom() reads. Reading it directly
     * keeps IDF's error line for an empty field out of the log of every
     * unit without a factory MAC. */
    uint8_t value[JETKVM_DEVICE_ID_MAC_BYTES] = {0};
    if (esp_efuse_get_field_size(ESP_EFUSE_USER_DATA_MAC_CUSTOM) != sizeof(value) * 8U ||
        esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA_MAC_CUSTOM, value, sizeof(value) * 8U) != ESP_OK ||
        !jetkvm_custom_mac_valid(value)) {
        return false;
    }
    memcpy(mac, value, sizeof(value));
    return true;
#endif
}

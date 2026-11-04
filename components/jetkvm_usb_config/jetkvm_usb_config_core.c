/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_usb_config.h"

#include <string.h>

static size_t bounded_length(const char *value, size_t capacity)
{
    if (!value) {
        return capacity;
    }
    size_t length = 0U;
    while (length < capacity && value[length] != '\0') {
        ++length;
    }
    return length;
}

static bool printable_ascii(const char *value, bool allow_empty)
{
    size_t length = bounded_length(value, JETKVM_USB_STRING_CAPACITY);
    if (length >= JETKVM_USB_STRING_CAPACITY ||
        (!allow_empty && length == 0U)) {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        unsigned char byte = (unsigned char)value[i];
        if (byte < 0x20U || byte > 0x7eU) {
            return false;
        }
    }
    return true;
}

static bool bounded_string_equal(const char *left, const char *right)
{
    size_t left_length = bounded_length(left, JETKVM_USB_STRING_CAPACITY);
    size_t right_length = bounded_length(right, JETKVM_USB_STRING_CAPACITY);
    return left_length < JETKVM_USB_STRING_CAPACITY &&
           right_length < JETKVM_USB_STRING_CAPACITY &&
           left_length == right_length &&
           memcmp(left, right, left_length) == 0;
}

static void copy_string(char destination[JETKVM_USB_STRING_CAPACITY],
                        const char *source)
{
    size_t length = bounded_length(source, JETKVM_USB_STRING_CAPACITY);
    memset(destination, 0, JETKVM_USB_STRING_CAPACITY);
    memcpy(destination, source, length);
}

void jetkvm_usb_config_defaults(jetkvm_usb_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->schema_version = JETKVM_USB_CONFIG_SCHEMA_VERSION;
    config->vendor_id = JETKVM_USB_DEFAULT_VENDOR_ID;
    config->product_id = JETKVM_USB_DEFAULT_PRODUCT_ID;
    memcpy(config->serial_number, JETKVM_USB_DEFAULT_SERIAL_NUMBER,
           sizeof(JETKVM_USB_DEFAULT_SERIAL_NUMBER));
    memcpy(config->manufacturer, JETKVM_USB_DEFAULT_MANUFACTURER,
           sizeof(JETKVM_USB_DEFAULT_MANUFACTURER));
    memcpy(config->product, JETKVM_USB_DEFAULT_PRODUCT,
           sizeof(JETKVM_USB_DEFAULT_PRODUCT));
    config->keyboard = true;
    config->absolute_mouse = true;
    config->relative_mouse = true;
    config->mass_storage = true;
    config->serial_console = false;
    config->audio = true;
}

bool jetkvm_usb_config_migrate_default_identity(jetkvm_usb_config_t *config)
{
    if (!config || config->vendor_id != JETKVM_USB_LEGACY_DEFAULT_VENDOR_ID ||
        config->product_id != JETKVM_USB_LEGACY_DEFAULT_PRODUCT_ID ||
        !bounded_string_equal(config->manufacturer,
                              JETKVM_USB_DEFAULT_MANUFACTURER) ||
        !bounded_string_equal(config->product, JETKVM_USB_DEFAULT_PRODUCT)) {
        return false;
    }
    config->vendor_id = JETKVM_USB_DEFAULT_VENDOR_ID;
    config->product_id = JETKVM_USB_DEFAULT_PRODUCT_ID;
    return true;
}

bool jetkvm_usb_config_validate(const jetkvm_usb_config_t *config)
{
    return config &&
           config->schema_version == JETKVM_USB_CONFIG_SCHEMA_VERSION &&
           config->vendor_id != 0U && config->product_id != 0U &&
           printable_ascii(config->serial_number, true) &&
           printable_ascii(config->manufacturer, true) &&
           printable_ascii(config->product, true) &&
           (config->keyboard || config->absolute_mouse ||
            config->relative_mouse || config->mass_storage || config->audio) &&
           !config->serial_console;
}

bool jetkvm_usb_config_equal(const jetkvm_usb_config_t *left,
                             const jetkvm_usb_config_t *right)
{
    return left && right &&
           left->schema_version == right->schema_version &&
           left->vendor_id == right->vendor_id &&
           left->product_id == right->product_id &&
           bounded_string_equal(left->serial_number, right->serial_number) &&
           bounded_string_equal(left->manufacturer, right->manufacturer) &&
           bounded_string_equal(left->product, right->product) &&
           left->keyboard == right->keyboard &&
           left->absolute_mouse == right->absolute_mouse &&
           left->relative_mouse == right->relative_mouse &&
           left->mass_storage == right->mass_storage &&
           left->serial_console == right->serial_console &&
           left->audio == right->audio;
}

esp_err_t jetkvm_usb_config_set_identity(
    jetkvm_usb_config_t *config,
    uint16_t vendor_id, uint16_t product_id,
    const char *serial_number, const char *manufacturer,
    const char *product)
{
    if (!config || vendor_id == 0U || product_id == 0U ||
        !printable_ascii(serial_number, true) ||
        !printable_ascii(manufacturer, true) ||
        !printable_ascii(product, true)) {
        return ESP_ERR_INVALID_ARG;
    }

    jetkvm_usb_config_t updated = *config;
    updated.schema_version = JETKVM_USB_CONFIG_SCHEMA_VERSION;
    updated.vendor_id = vendor_id;
    updated.product_id = product_id;
    copy_string(updated.serial_number, serial_number);
    copy_string(updated.manufacturer, manufacturer);
    copy_string(updated.product, product);
    *config = updated;
    return ESP_OK;
}

esp_err_t jetkvm_usb_config_set_devices(
    jetkvm_usb_config_t *config,
    bool keyboard, bool absolute_mouse, bool relative_mouse,
    bool mass_storage, bool serial_console, bool audio)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (serial_console ||
        (!keyboard && !absolute_mouse && !relative_mouse && !mass_storage && !audio)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    jetkvm_usb_config_t updated = *config;
    updated.schema_version = JETKVM_USB_CONFIG_SCHEMA_VERSION;
    updated.keyboard = keyboard;
    updated.absolute_mouse = absolute_mouse;
    updated.relative_mouse = relative_mouse;
    updated.mass_storage = mass_storage;
    updated.serial_console = false;
    updated.audio = audio;
    *config = updated;
    return ESP_OK;
}

bool jetkvm_usb_config_generation_is_newer(uint32_t candidate,
                                            uint32_t reference)
{
    if (candidate == 0U || reference == 0U || candidate == reference) {
        return false;
    }
    uint32_t distance = candidate - reference;
    return distance < UINT32_C(0x80000000);
}

uint32_t jetkvm_usb_config_generation_next(uint32_t current)
{
    return current == UINT32_MAX ? 1U : current + 1U;
}

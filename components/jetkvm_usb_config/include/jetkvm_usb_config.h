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

#define JETKVM_USB_CONFIG_SCHEMA_VERSION 1U
#define JETKVM_USB_CONFIG_RECORD_VERSION 1U
#define JETKVM_USB_STRING_CAPACITY 32U

/* JetKVM's own USB ID. The IP-KVM OS Services Linux udev rule matches it to
 * grant desktop users access to the device. */
#define JETKVM_USB_DEFAULT_VENDOR_ID 0x399bU
#define JETKVM_USB_DEFAULT_PRODUCT_ID 0x0001U
/* The default before JetKVM had its own ID. Loading a stored record that
 * still carries it with the default strings moves it to the current
 * default; see jetkvm_usb_config_migrate_default_identity. */
#define JETKVM_USB_LEGACY_DEFAULT_VENDOR_ID 0x1d6bU
#define JETKVM_USB_LEGACY_DEFAULT_PRODUCT_ID 0x0104U
#define JETKVM_USB_DEFAULT_SERIAL_NUMBER ""
#define JETKVM_USB_DEFAULT_MANUFACTURER "JetKVM"
#define JETKVM_USB_DEFAULT_PRODUCT "USB Emulation Device"

/**
 * Complete persistent USB identity and function selection.
 *
 * The P4 firmware implements the three HID functions, mass storage, and USB
 * audio; all five default to true. The CDC serial console is represented for
 * protocol parity but is not implemented, so it defaults to false and
 * validation rejects attempts to enable it.
 */
typedef struct {
    uint32_t schema_version;
    uint16_t vendor_id;
    uint16_t product_id;
    char serial_number[JETKVM_USB_STRING_CAPACITY];
    char manufacturer[JETKVM_USB_STRING_CAPACITY];
    char product[JETKVM_USB_STRING_CAPACITY];
    bool keyboard;
    bool absolute_mouse;
    bool relative_mouse;
    bool mass_storage;
    bool serial_console;
    bool audio;
} jetkvm_usb_config_t;

typedef enum {
    JETKVM_USB_CONFIG_SLOT_NONE = 0,
    JETKVM_USB_CONFIG_SLOT_A = 1,
    JETKVM_USB_CONFIG_SLOT_B = 2,
} jetkvm_usb_config_slot_t;

/**
 * Runtime metadata for the A/B record store.
 *
 * A slot error is ESP_OK when that slot contained a valid record and
 * ESP_ERR_NVS_NOT_FOUND when it was empty. Other values retain the reason a
 * slot was rejected. `degraded` is true when at least one non-empty slot was
 * rejected. Corrupt records are recovery information, not a load failure.
 */
typedef struct {
    jetkvm_usb_config_slot_t active_slot;
    uint32_t generation;
    bool defaults_used;
    bool degraded;
    esp_err_t slot_a_error;
    esp_err_t slot_b_error;
} jetkvm_usb_config_store_state_t;

/** Populate upstream-compatible identity and P4-safe function defaults. */
void jetkvm_usb_config_defaults(jetkvm_usb_config_t *config);

/** Validate identity strings, IDs, function flags, and the current schema. */
bool jetkvm_usb_config_validate(const jetkvm_usb_config_t *config);

/** Compare two validated or partially constructed configurations by value. */
bool jetkvm_usb_config_equal(const jetkvm_usb_config_t *left,
                             const jetkvm_usb_config_t *right);

/**
 * Atomically replace the USB identity fields.
 *
 * Strings are at most 31 printable ASCII bytes and may be empty, matching the
 * Go JSON/configfs API. IDs must be non-zero.
 */
esp_err_t jetkvm_usb_config_set_identity(
    jetkvm_usb_config_t *config,
    uint16_t vendor_id, uint16_t product_id,
    const char *serial_number, const char *manufacturer,
    const char *product);

/**
 * Atomically replace the enabled USB functions.
 *
 * Enabling the CDC serial console, or disabling every implemented function
 * (keyboard, absolute mouse, relative mouse, mass storage, and audio),
 * returns ESP_ERR_NOT_SUPPORTED and leaves `config` unchanged. The all-off
 * combination cannot produce a valid USB descriptor set.
 */
esp_err_t jetkvm_usb_config_set_devices(
    jetkvm_usb_config_t *config,
    bool keyboard, bool absolute_mouse, bool relative_mouse,
    bool mass_storage, bool serial_console, bool audio);

/**
 * Move the legacy default identity (1d6b:0104 with the default manufacturer
 * and product strings) to the current default ID, keeping the serial number.
 * Any other identity, including a custom one, is left unchanged. Returns
 * true when `config` changed.
 */
bool jetkvm_usb_config_migrate_default_identity(jetkvm_usb_config_t *config);

/** RFC-1982-style uint32 generation ordering used by the A/B store. */
bool jetkvm_usb_config_generation_is_newer(uint32_t candidate,
                                            uint32_t reference);

/** Return the next non-zero serial generation; UINT32_MAX wraps to one. */
uint32_t jetkvm_usb_config_generation_next(uint32_t current);

/**
 * Load the newest valid A/B record from NVS namespace `jkvm_usb`.
 *
 * Outputs are initialized to safe defaults before storage access. Missing,
 * corrupt, unknown-version, or otherwise invalid records are rejected per
 * slot. A valid older slot is used as fallback. If neither slot is valid,
 * safe defaults are returned. A loaded record with the legacy default
 * identity is migrated in memory only; the next save persists it.
 * Record-local recovery returns ESP_OK and is
 * described by `state`; namespace/open failures are returned to the caller
 * while still leaving safe defaults in the outputs.
 */
esp_err_t jetkvm_usb_config_load(
    jetkvm_usb_config_t *config,
    jetkvm_usb_config_store_state_t *state);

/**
 * Persist a validated snapshot to the slot opposite `state->active_slot`.
 *
 * The new record is committed and read back through NVS before `state` is
 * updated. Any write, commit, readback, or verification failure leaves the
 * caller's runtime metadata byte-for-byte unchanged. The owner can therefore
 * load before USB startup, apply a descriptor transaction, call this method,
 * and publish its runtime configuration only after persistence succeeds.
 */
esp_err_t jetkvm_usb_config_save(
    const jetkvm_usb_config_t *config,
    jetkvm_usb_config_store_state_t *state);

#ifdef __cplusplus
}
#endif

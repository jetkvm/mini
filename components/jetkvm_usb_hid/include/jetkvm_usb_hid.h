/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JETKVM_USB_HID_KEY_COUNT 6U
#define JETKVM_USB_HID_ABSOLUTE_MAX 32767
#define JETKVM_USB_HID_DESCRIPTOR_STRING_CAPACITY 128U
#define JETKVM_USB_HID_USB_STRING_MAX_LENGTH 31U
#define JETKVM_USB_HID_INTERFACE_DISABLED UINT8_MAX

/**
 * Descriptor configuration copied into component-owned static storage.
 *
 * ESP TinyUSB currently converts at most 31 ASCII characters from each USB
 * string.  The setter rejects longer, non-ASCII or control-character strings
 * instead of silently exposing a truncated/different identity to the host.
 * Callers may release all string storage as soon as the setter returns.
 */
typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    const char *serial_number;
    const char *manufacturer;
    const char *product;
    bool absolute_mouse;
    bool relative_mouse;
    bool keyboard;
    bool mass_storage;
    bool audio;
    /* The IP-KVM OS Services vendor interface (class 0xff/0x4a/0x01), with
     * its Microsoft OS 2.0 descriptors. The firmware must link the OS
     * Services class driver, which claims the interface. */
    bool os_services;
} jetkvm_usb_hid_descriptor_configuration_t;

/* The OS Services class driver's hooks into the USB device this component
 * owns. */
typedef struct {
    /*
     * Vendor control requests addressed to the OS Services interface, from
     * TinyUSB's task, once per control stage, exactly as TinyUSB's
     * tud_vendor_control_xfer_cb. request points to a tusb_control_request_t.
     * Returning false stalls the request. It must not block.
     */
    bool (*vendor_request)(uint8_t rhport, uint8_t stage, const void *request,
                           void *ctx);
    /*
     * TinyUSB is about to be uninstalled, and its task still runs. Once this
     * returns, the class driver must not call into TinyUSB until TinyUSB
     * calls the driver's init again. It may wait briefly for TinyUSB's task.
     */
    void (*stack_stopping)(void *ctx);
    void *ctx;
} jetkvm_usb_hid_os_services_hooks_t;

/*
 * TinyUSB calls this callback from its CPU0 task. The callback must not block
 * or call a HID report function synchronously.
 */
typedef void (*jetkvm_usb_hid_led_callback_t)(uint8_t leds, void *ctx);

typedef struct {
    bool initialized;
    /* TinyUSB has received a setup packet since the latest USB bus reset. */
    bool connected;
    /* The host selected the HID configuration (stronger than connected). */
    bool mounted;
    bool suspended;
    /* Valid only while suspended; cleared by wake, resume and unmount. */
    bool remote_wakeup_enabled;
    bool link_high_speed;
    uint8_t keyboard_leds;
    uint32_t reports_sent;
    uint32_t reports_busy;
    uint32_t mount_count;
    uint32_t unmount_count;
    uint32_t suspend_count;
    uint32_t resume_count;
    uint32_t wake_attempts;
    uint32_t wake_success;
    uint32_t wake_failures;
    uint64_t msc_read_calls;
    uint64_t msc_read_bytes;
    uint64_t msc_read_errors;
    uint64_t msc_read_total_us;
    /* One command spans its first READ(10) callback through CSW completion.
     * Intra-command gaps therefore contain USB IN transfer plus TinyUSB task
     * dispatch, while sequential-command gaps contain BOT CSW/next-CBW time. */
    uint64_t msc_read_commands;
    uint64_t msc_read_command_total_us;
    uint64_t msc_read_intra_gap_samples;
    uint64_t msc_read_intra_gap_total_us;
    uint64_t msc_read_sequential_command_gap_samples;
    uint64_t msc_read_sequential_command_gap_total_us;
    /* Non-READ SCSI commands can legitimately occur between sequential reads;
     * their BOT time remains included in the next sequential command gap. */
    uint64_t msc_read_non_read_commands;
    /* Bus/reset recovery that terminates a timing sequence. */
    uint64_t msc_read_timing_resets;
    uint32_t msc_read_max_us;
    uint32_t msc_read_max_command_us;
    uint32_t msc_read_max_intra_gap_us;
    uint32_t msc_read_max_sequential_command_gap_us;
    uint32_t msc_read_min_size;
    uint32_t msc_read_max_size;
    uint64_t msc_write_calls;
    uint64_t msc_write_bytes;
    uint64_t msc_write_errors;
    uint64_t msc_write_total_us;
    uint64_t msc_write_commands;
    uint64_t msc_write_command_total_us;
    uint32_t msc_write_max_us;
    uint32_t msc_write_max_command_us;
    uint32_t msc_write_min_size;
    uint32_t msc_write_max_size;
    esp_err_t last_error;
} jetkvm_usb_hid_state_t;

typedef struct {
    uint8_t modifier;
    uint8_t keycodes[JETKVM_USB_HID_KEY_COUNT];
} jetkvm_usb_hid_keyboard_state_t;

/**
 * Read-only view of the target-host USB gadget. Numeric identifiers, strings
 * and interface flags match the currently selected component-owned descriptor
 * bank. Interface and TinyUSB instance values are UINT8_MAX when that function
 * is disabled. emulation_enabled is the live target-port driver-installed
 * state, not physical host attachment. This structure never describes the
 * independent USB Serial/JTAG debug port.
 */
typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    char serial_number[JETKVM_USB_HID_DESCRIPTOR_STRING_CAPACITY];
    char manufacturer[JETKVM_USB_HID_DESCRIPTOR_STRING_CAPACITY];
    char product[JETKVM_USB_HID_DESCRIPTOR_STRING_CAPACITY];
    bool absolute_mouse;
    bool relative_mouse;
    bool keyboard;
    bool mass_storage;
    bool serial_console;
    bool audio;
    bool os_services;
    bool emulation_enabled;
    uint8_t keyboard_interface;
    uint8_t absolute_mouse_interface;
    uint8_t relative_mouse_interface;
    uint8_t keyboard_instance;
    uint8_t absolute_mouse_instance;
    uint8_t relative_mouse_instance;
    uint8_t os_services_interface;
} jetkvm_usb_hid_configuration_snapshot_t;

/*
 * Select descriptors for the next init. This is accepted only while the
 * target-port TinyUSB driver is stopped. NULL restores the compiled defaults.
 * Descriptor bytes, mappings and strings are built in the inactive one of two
 * static banks, so TinyUSB never retains a pointer to caller-owned or rewritten
 * storage. The configurable keyboard, absolute mouse and relative mouse are
 * emitted in that stable scan order, followed by JetKVM's always-present wake
 * HID function. Enabled interface/instance numbers are dense and deterministic;
 * disabling all three input functions leaves only the wake interface.
 */
esp_err_t jetkvm_usb_hid_set_descriptor_configuration(
    const jetkvm_usb_hid_descriptor_configuration_t *configuration);

/*
 * Installs the composite TinyUSB device on ESP32-P4 USB2 HS/UTMI: the HID
 * functions, and mass storage, USB audio, and the OS Services vendor
 * interface when the descriptor configuration enables them. The TinyUSB
 * worker is pinned to CPU0. Initialization never sends an input report.
 */
esp_err_t jetkvm_usb_hid_init(void);

/*
 * True when the host finds a CD-ROM image that has just been mounted as a
 * new medium of the drive that it has. Two things must hold. The host has
 * the virtual-media drive as a CD-ROM drive: the target-port driver runs
 * with mass storage, and the enumeration it started with presented a CD-ROM
 * drive (a CD-ROM image was mounted then, or nothing). And the host asks the
 * drive for its medium: it has asked in the last 3 s, and since the drive
 * was last emptied. Every other mount needs a re-enumeration, which presents
 * the drive again.
 */
bool jetkvm_usb_hid_cdrom_drive_takes_medium(void);

/* Best-effort release of all keys/buttons followed by driver teardown. */
esp_err_t jetkvm_usb_hid_deinit(void);

esp_err_t jetkvm_usb_hid_get_state(jetkvm_usb_hid_state_t *state);

/*
 * True while TinyUSB's task has events of the bus that it has not handled.
 * A report waits for the report before it, and that one is done only when
 * TinyUSB's task has handled its completion. So a report that was not sent
 * in time while this is true may have waited for this device, with a host
 * that reads: it says nothing about the host. Also true when the driver
 * cannot be asked, because it stops or another task reads its state.
 */
bool jetkvm_usb_hid_bus_events_waiting(void);

/* Copies a coherent, caller-owned view without reconfiguring USB hardware. */
esp_err_t jetkvm_usb_hid_get_configuration_snapshot(
    jetkvm_usb_hid_configuration_snapshot_t *snapshot);

/*
 * Requests the standard USB remote-wakeup signal. This succeeds only while
 * the HID device is initialized, connected, configured, suspended and the
 * host enabled DEVICE_REMOTE_WAKEUP. A successful return means the controller
 * accepted the wake signal; a subsequent resume_count increment confirms that
 * the host actually resumed the bus.
 *
 * Concurrent calls never queue: a second call returns ESP_ERR_TIMEOUT. The
 * ESP32-P4 DWC2 implementation holds the wake signal for one scheduler tick,
 * so the call is bounded but may yield once.
 */
esp_err_t jetkvm_usb_hid_remote_wakeup(void);

/* Returns the last keyboard state successfully submitted to TinyUSB. */
esp_err_t jetkvm_usb_hid_get_keyboard_state(
    jetkvm_usb_hid_keyboard_state_t *state);

/* A NULL keycodes pointer is treated as six zero keycodes (release). */
esp_err_t jetkvm_usb_hid_keyboard_report(
    uint8_t modifier,
    const uint8_t keycodes[JETKVM_USB_HID_KEY_COUNT]);

/*
 * Updates one HID usage in the current 6KRO state and submits the complete
 * keyboard report. Modifier usages 0xE0..0xE7 map to modifier bits 0..7.
 */
esp_err_t jetkvm_usb_hid_keypress(uint8_t usage, bool pressed);

/* Coordinates are clamped to the JetKVM-compatible range 0..32767. */
esp_err_t jetkvm_usb_hid_absolute_mouse_report(
    int32_t x,
    int32_t y,
    uint8_t buttons);

/* JetKVM absolute-mouse report ID 2: vertical wheel and horizontal AC Pan. */
esp_err_t jetkvm_usb_hid_absolute_wheel_report(
    int8_t wheel_y,
    int8_t wheel_x);

/*
 * JetKVM-compatible wheel route: use the absolute-mouse wheel report exactly
 * once when that function is enabled; otherwise use the relative-mouse report.
 * A keyboard-only configuration treats wheel input as a successful no-op.
 */
esp_err_t jetkvm_usb_hid_wheel_report(int8_t wheel_y, int8_t wheel_x);

/*
 * Relative mouse reports carry eight buttons, X/Y, vertical wheel and
 * horizontal AC Pan. Deltas are never repeated by this component.
 */
esp_err_t jetkvm_usb_hid_relative_mouse_report(
    int8_t dx,
    int8_t dy,
    uint8_t buttons,
    int8_t wheel_y,
    int8_t wheel_x);

/*
 * Sends zero keyboard and mouse-button state without moving the absolute
 * pointer. Each interface has a bounded 20 ms submission deadline. On error,
 * state which was not submitted remains latched so the caller can retry the
 * release instead of silently losing a key-up or button-up transition.
 */
esp_err_t jetkvm_usb_hid_release_all(void);

/*
 * Registers the OS Services class driver's hooks. Passing NULL unregisters
 * them; vendor requests on the interface then stall.
 */
esp_err_t jetkvm_usb_hid_set_os_services_hooks(
    const jetkvm_usb_hid_os_services_hooks_t *hooks);

/* Passing NULL unregisters the callback. */
esp_err_t jetkvm_usb_hid_set_led_callback(
    jetkvm_usb_hid_led_callback_t callback,
    void *ctx);

#ifdef __cplusplus
}
#endif

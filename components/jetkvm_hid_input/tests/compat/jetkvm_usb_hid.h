#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define JETKVM_USB_HID_KEY_COUNT 6U
#define JETKVM_USB_HID_DESCRIPTOR_STRING_CAPACITY 128U

typedef void (*jetkvm_usb_hid_led_callback_t)(uint8_t leds, void *ctx);

typedef struct {
    bool initialized;
    bool connected;
    bool mounted;
    bool suspended;
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
    esp_err_t last_error;
} jetkvm_usb_hid_state_t;

typedef struct {
    uint8_t modifier;
    uint8_t keycodes[JETKVM_USB_HID_KEY_COUNT];
} jetkvm_usb_hid_keyboard_state_t;

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
    bool emulation_enabled;
} jetkvm_usb_hid_configuration_snapshot_t;

esp_err_t jetkvm_usb_hid_get_state(jetkvm_usb_hid_state_t *state);
bool jetkvm_usb_hid_bus_events_waiting(void);
esp_err_t jetkvm_usb_hid_get_configuration_snapshot(
    jetkvm_usb_hid_configuration_snapshot_t *snapshot);
esp_err_t jetkvm_usb_hid_remote_wakeup(void);
esp_err_t jetkvm_usb_hid_get_keyboard_state(
    jetkvm_usb_hid_keyboard_state_t *state);
esp_err_t jetkvm_usb_hid_keyboard_report(
    uint8_t modifier, const uint8_t keycodes[JETKVM_USB_HID_KEY_COUNT]);
esp_err_t jetkvm_usb_hid_keypress(uint8_t usage, bool pressed);
esp_err_t jetkvm_usb_hid_absolute_mouse_report(int32_t x, int32_t y,
                                                uint8_t buttons);
esp_err_t jetkvm_usb_hid_absolute_wheel_report(int8_t wheel_y,
                                                int8_t wheel_x);
esp_err_t jetkvm_usb_hid_wheel_report(int8_t wheel_y, int8_t wheel_x);
esp_err_t jetkvm_usb_hid_relative_mouse_report(int8_t dx, int8_t dy,
                                                uint8_t buttons,
                                                int8_t wheel_y,
                                                int8_t wheel_x);
esp_err_t jetkvm_usb_hid_release_all(void);
esp_err_t jetkvm_usb_hid_set_led_callback(
    jetkvm_usb_hid_led_callback_t callback, void *ctx);

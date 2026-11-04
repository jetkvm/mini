#pragma once

#include <stddef.h>
#include <stdint.h>

#include "jetkvm_usb_hid.h"

typedef struct {
    int64_t timestamp_us;
    uint8_t modifier;
    uint8_t keys[JETKVM_USB_HID_KEY_COUNT];
} fake_keyboard_report_t;

void fake_usb_reset_log(void);
size_t fake_usb_report_count(void);
fake_keyboard_report_t fake_usb_report_at(size_t index);
unsigned fake_usb_release_all_count(void);
void fake_usb_set_state(jetkvm_usb_hid_state_t state);
void fake_usb_set_wake_result(esp_err_t result);
unsigned fake_usb_wake_call_count(void);
void fake_usb_emit_led(uint8_t leds);
/* What jetkvm_usb_hid_keypress() returns; ESP_OK sends the report. */
void fake_usb_set_keypress_result(esp_err_t result);
/* What jetkvm_usb_hid_bus_events_waiting() returns. */
void fake_usb_set_bus_events_waiting(bool waiting);

typedef struct { bool relative; int32_t x, y; uint8_t buttons; } fake_mouse_report_t;
size_t fake_usb_mouse_count(void);
fake_mouse_report_t fake_usb_mouse_at(size_t index);

void fake_queue_fail_next_send(void);

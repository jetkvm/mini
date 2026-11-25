"""Scroll on the relative mouse while a button is held.

Compiles the production wheel and relative mouse report functions against a
model of the report queue, then checks the bytes that go to TinyUSB.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


def function(source, name):
    match = re.search(r"^esp_err_t " + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise ValueError("Missing production function: " + name)
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


MODEL = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NOT_SUPPORTED 0x106
#define JETKVM_USB_HID_INTERFACE_DISABLED UINT8_MAX
typedef int portMUX_TYPE;
static portMUX_TYPE s_state_lock;
static int lock_depth;
#define portENTER_CRITICAL(lock) ((void)(lock), ++lock_depth)
#define portEXIT_CRITICAL(lock) ((void)(lock), --lock_depth)
typedef enum {
    HID_FUNCTION_ABSOLUTE_MOUSE,
    HID_FUNCTION_RELATIVE_MOUSE,
} hid_function_t;
typedef struct { int unused; } report_deadline_t;
typedef struct __attribute__((packed)) {
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel_y;
    int8_t wheel_x;
} relative_mouse_report_t;
static struct { uint8_t relative_buttons; } s_ctx;
static int s_report_mutex[2];
static uint8_t sent[5];
static unsigned sent_count;
static void xSemaphoreGive(int mutex) { (void)mutex; }
static void record_error(esp_err_t error) { (void)error; }
static bool active_function_enabled(hid_function_t function)
{
    return function == HID_FUNCTION_RELATIVE_MOUSE;
}
static uint8_t active_instance_for_function(hid_function_t function)
{
    return function == HID_FUNCTION_RELATIVE_MOUSE
        ? 1U : JETKVM_USB_HID_INTERFACE_DISABLED;
}
static esp_err_t lock_instance(uint8_t instance, report_deadline_t *deadline)
{
    (void)instance;
    (void)deadline;
    return ESP_OK;
}
static esp_err_t queue_report_locked(uint8_t instance, uint8_t report_id,
                                     const void *report, uint16_t size,
                                     const report_deadline_t *deadline)
{
    (void)deadline;
    assert(instance == 1U && report_id == 0U && size == sizeof(sent));
    assert(lock_depth == 0);
    memcpy(sent, report, sizeof(sent));
    sent_count++;
    return ESP_OK;
}
static esp_err_t jetkvm_usb_hid_absolute_wheel_report(int8_t y, int8_t x)
{
    (void)y;
    (void)x;
    assert(!"relative mouse only");
    return ESP_OK;
}
esp_err_t jetkvm_usb_hid_wheel_report(int8_t wheel_y, int8_t wheel_x);
esp_err_t jetkvm_usb_hid_relative_mouse_report(int8_t dx, int8_t dy,
                                               uint8_t buttons,
                                               int8_t wheel_y,
                                               int8_t wheel_x);
'''

SCENARIOS = r'''
static void expect(uint8_t buttons, int8_t x, int8_t y, int8_t wy, int8_t wx)
{
    const uint8_t want[5] = {buttons, (uint8_t)x, (uint8_t)y,
                             (uint8_t)wy, (uint8_t)wx};
    assert(memcmp(sent, want, sizeof(want)) == 0);
    assert(s_ctx.relative_buttons == buttons);
    assert(lock_depth == 0);
}

int main(void)
{
    assert(jetkvm_usb_hid_relative_mouse_report(10, -5, 1, 0, 0) == ESP_OK);
    expect(1, 10, -5, 0, 0);
    assert(jetkvm_usb_hid_wheel_report(1, -1) == ESP_OK);
    expect(1, 0, 0, 1, -1);
    assert(jetkvm_usb_hid_relative_mouse_report(-127, 127, 0xff, 0, 0) == ESP_OK);
    expect(0xff, -127, 127, 0, 0);
    assert(jetkvm_usb_hid_wheel_report(-1, 1) == ESP_OK);
    expect(0xff, 0, 0, -1, 1);
    assert(jetkvm_usb_hid_relative_mouse_report(0, 0, 0, 0, 0) == ESP_OK);
    expect(0, 0, 0, 0, 0);
    assert(jetkvm_usb_hid_wheel_report(1, 0) == ESP_OK);
    expect(0, 0, 0, 1, 0);
    assert(sent_count == 6);
    return 0;
}
'''


class RelativeWheelButtonTests(unittest.TestCase):
    def test_wheel_keeps_held_buttons(self):
        source = Path(os.environ.get(
            "USB_HID_SOURCE",
            Path(__file__).parents[1] / "jetkvm_usb_hid.c")).read_text()
        with tempfile.TemporaryDirectory() as temp:
            test_source = Path(temp) / "wheel.c"
            test_source.write_text(
                MODEL
                + function(source, "jetkvm_usb_hid_wheel_report")
                + function(source, "jetkvm_usb_hid_relative_mouse_report")
                + SCENARIOS)
            binary = Path(temp) / "wheel"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test_source), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True,
                                    text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()

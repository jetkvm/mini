#!/usr/bin/env python3
"""Static safety/contract checks for the P4-only USB HID component."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "jetkvm_usb_hid.c").read_text(encoding="utf-8")
HEADER = (ROOT / "include" / "jetkvm_usb_hid.h").read_text(encoding="utf-8")


def body(symbol: str) -> str:
    match = re.search(rf"\b{re.escape(symbol)}\s*\([^)]*\)\s*\{{", SOURCE)
    assert match, f"missing function {symbol}"
    depth = 1
    cursor = match.end()
    while depth and cursor < len(SOURCE):
        depth += SOURCE[cursor] == "{"
        depth -= SOURCE[cursor] == "}"
        cursor += 1
    assert depth == 0, f"unterminated function {symbol}"
    return SOURCE[match.end() : cursor - 1]


descriptor_builder = body("build_configuration_descriptor")
assert "TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP" in descriptor_builder
assert "bank->interface_count" in descriptor_builder
assert "bank->configuration_length" in descriptor_builder
assert "append_hid_function_descriptor" in descriptor_builder

function_builder = body("append_hid_function_descriptor")
assert function_builder.count("TUD_HID_INOUT_DESCRIPTOR") == 1
assert function_builder.count("TUD_HID_DESCRIPTOR") == 3
assert "descriptor[7] = HID_ITF_PROTOCOL_MOUSE" in function_builder
assert "subclass 0" in function_builder
assert "HID_FUNCTION_WAKE" in function_builder
assert "s_wake_report_descriptor" in function_builder
assert "ENDPOINT_WAKE" in function_builder

assert "CONFIG_TINYUSB_HID_COUNT != 4" in SOURCE
assert "CONFIG_TINYUSB_CDC_ENABLED" in SOURCE
assert "CONFIG_USJ_ENABLE_USB_SERIAL_JTAG" in SOURCE
assert "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED" in SOURCE

init = body("jetkvm_usb_hid_init")
assert "config.port = TINYUSB_PORT_HIGH_SPEED_0" in init
assert "TINYUSB_TASK_CUSTOM(8192, 5, 0)" in init
assert "config.phy.skip_setup = false" in init
assert "descriptor_bank->full_speed_configuration" in init
assert "descriptor_bank->high_speed_configuration" in init

event_callback = body("tinyusb_event_callback")
assert "event->suspended.remote_wakeup" in event_callback
assert "mount_count++" in event_callback
assert "unmount_count++" in event_callback
assert "suspend_count++" in event_callback
assert "resume_count++" in event_callback

refresh = body("refresh_bus_state")
assert "xSemaphoreTake(lifecycle_mutex, 0)" in refresh
assert refresh.index("xSemaphoreTake(lifecycle_mutex, 0)") < refresh.index(
    "tud_connected()"
)
assert "xSemaphoreGive(lifecycle_mutex)" in refresh

wake = body("jetkvm_usb_hid_remote_wakeup")
for required in (
    "state.initialized",
    "state.connected",
    "state.mounted",
    "state.suspended",
    "state.remote_wakeup_enabled",
    "!s_ctx.deinitializing",
    "!s_ctx.stopping",
    "xSemaphoreTake(wakeup_mutex, 0)",
    "tinyusb_remote_wakeup()",
    "wake_attempts++",
    "wake_success++",
    "wake_failures++",
):
    assert required in wake, f"wake contract is missing {required}"

deinit = body("jetkvm_usb_hid_deinit")
assert "xSemaphoreTake(s_wakeup_mutex, stop_wait)" in deinit
assert deinit.index("xSemaphoreTake(s_wakeup_mutex, stop_wait)") < deinit.index(
    "tinyusb_driver_uninstall()"
)

for state_field in (
    "bool connected;",
    "bool remote_wakeup_enabled;",
    "uint32_t mount_count;",
    "uint32_t unmount_count;",
    "uint32_t suspend_count;",
    "uint32_t resume_count;",
    "uint32_t wake_attempts;",
    "uint32_t wake_success;",
    "uint32_t wake_failures;",
):
    assert state_field in HEADER, f"public state is missing {state_field}"

print("jetkvm_usb_hid remote-wakeup static contract: PASS")

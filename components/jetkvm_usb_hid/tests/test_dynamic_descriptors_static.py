#!/usr/bin/env python3
"""Static contracts for dynamic HID descriptor banks and routing."""

from pathlib import Path
import re


COMPONENT = Path(__file__).resolve().parents[1]
SOURCE = (COMPONENT / "jetkvm_usb_hid.c").read_text(encoding="utf-8")
HEADER = (COMPONENT / "include" / "jetkvm_usb_hid.h").read_text(encoding="utf-8")


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


# The public API copies transient caller strings and exposes deterministic
# mapping values for disabled or compacted interfaces.
for public_contract in (
    "JETKVM_USB_HID_USB_STRING_MAX_LENGTH 31U",
    "JETKVM_USB_HID_INTERFACE_DISABLED UINT8_MAX",
    "jetkvm_usb_hid_descriptor_configuration_t",
    "jetkvm_usb_hid_set_descriptor_configuration",
    "jetkvm_usb_hid_wheel_report",
    "uint8_t keyboard_interface;",
    "uint8_t absolute_mouse_interface;",
    "uint8_t relative_mouse_interface;",
    "uint8_t keyboard_instance;",
    "uint8_t absolute_mouse_instance;",
    "uint8_t relative_mouse_instance;",
):
    assert public_contract in HEADER, f"missing public contract: {public_contract}"

# Two whole static banks retain all descriptor/string/mapping storage. The
# inactive bank is selected with xor only after a successful build.
assert "DESCRIPTOR_BANK_COUNT = 2" in SOURCE
assert "static hid_descriptor_bank_t s_descriptor_banks[DESCRIPTOR_BANK_COUNT]" in SOURCE
bank_type = re.search(
    r"typedef struct \{(.*?)\} hid_descriptor_bank_t;", SOURCE, re.S
)
assert bank_type, "missing descriptor bank"
for field in (
    "tusb_desc_device_t device;",
    "full_speed_configuration[CONFIG_DESCRIPTOR_MAX_LENGTH]",
    "high_speed_configuration[CONFIG_DESCRIPTOR_MAX_LENGTH]",
    "string_storage[STRING_COUNT][USB_STRING_STORAGE_CAPACITY]",
    "string_descriptors[STRING_COUNT]",
    "function_by_instance[HID_INSTANCE_CAPACITY]",
    "instance_by_function[HID_FUNCTION_COUNT]",
    "interface_by_function[HID_FUNCTION_COUNT]",
):
    assert field in bank_type.group(1), f"descriptor bank missing {field}"

selector = body("select_descriptor_configuration")
assert "s_selected_descriptor_bank ^ 1U" in selector
assert selector.index("build_descriptor_bank") < selector.index(
    "s_selected_descriptor_bank = destination_bank"
)
assert "s_descriptor_bank_ready = true" in selector

# Validate before copying and match esp_tinyusb's 31-byte conversion limit.
string_copy = body("copy_usb_string")
for required in (
    "source == NULL",
    "JETKVM_USB_HID_USB_STRING_MAX_LENGTH",
    "character < 0x20U",
    "character > 0x7eU",
    "ESP_ERR_INVALID_SIZE",
    "destination[length] = '\\0'",
):
    assert required in string_copy, f"USB string validation missing {required}"

# The accepted scan order is keyboard -> absolute -> relative -> wake. The
# first three are configurable; wake is always present like upstream JetKVM.
function_enum = re.search(r"typedef enum \{(.*?)\} hid_function_t;", SOURCE, re.S)
assert function_enum
enum_text = function_enum.group(1)
assert enum_text.index("HID_FUNCTION_KEYBOARD") < enum_text.index(
    "HID_FUNCTION_ABSOLUTE_MOUSE"
) < enum_text.index("HID_FUNCTION_RELATIVE_MOUSE") < enum_text.index(
    "HID_FUNCTION_WAKE"
)
builder = body("build_descriptor_bank")
assert "!configuration->keyboard && !configuration->absolute_mouse" not in builder
function_enabled = body("descriptor_function_enabled")
assert "case HID_FUNCTION_WAKE:" in function_enabled
assert "return true" in function_enabled
for mapping in (
    "const uint8_t index = bank->interface_count++",
    "bank->interface_by_function[function] = index",
    "bank->instance_by_function[function] = index",
    "bank->function_by_instance[index] = function",
):
    assert mapping in builder, f"dense mapping missing {mapping}"

function_descriptor = body("append_hid_function_descriptor")
assert "descriptor[7] = HID_ITF_PROTOCOL_MOUSE" in function_descriptor
assert "descriptor[6] = HID_ITF_PROTOCOL_MOUSE" not in function_descriptor
assert "offsetof(tusb_desc_interface_t, bInterfaceSubClass) == 6" in SOURCE
assert "offsetof(tusb_desc_interface_t, bInterfaceProtocol) == 7" in SOURCE

# TinyUSB callbacks and every transmit path resolve the runtime mapping rather
# than assuming fixed instance numbers.
report_descriptor = body("tud_hid_descriptor_report_cb")
assert "active_function_for_instance(instance)" in report_descriptor
assert "HID_FUNCTION_WAKE" in report_descriptor
assert "s_wake_report_descriptor" in report_descriptor
for sender, function in (
    ("jetkvm_usb_hid_keyboard_report", "HID_FUNCTION_KEYBOARD"),
    ("jetkvm_usb_hid_keypress", "HID_FUNCTION_KEYBOARD"),
    ("jetkvm_usb_hid_absolute_mouse_report", "HID_FUNCTION_ABSOLUTE_MOUSE"),
    ("jetkvm_usb_hid_absolute_wheel_report", "HID_FUNCTION_ABSOLUTE_MOUSE"),
    ("jetkvm_usb_hid_relative_mouse_report", "HID_FUNCTION_RELATIVE_MOUSE"),
):
    sender_body = body(sender)
    assert f"active_instance_for_function({function})" in sender_body
    assert "JETKVM_USB_HID_INTERFACE_DISABLED" in sender_body
    assert "ESP_ERR_NOT_SUPPORTED" in sender_body

# Match Go's single-route wheel behavior: absolute wins; relative is fallback.
wheel = body("jetkvm_usb_hid_wheel_report")
absolute_check = "active_function_enabled(HID_FUNCTION_ABSOLUTE_MOUSE)"
relative_check = "active_function_enabled(HID_FUNCTION_RELATIVE_MOUSE)"
assert wheel.index(absolute_check) < wheel.index(relative_check)
assert wheel.count("jetkvm_usb_hid_absolute_wheel_report") == 1
assert wheel.count("jetkvm_usb_hid_relative_mouse_report") == 1
assert "return ESP_OK" in wheel
assert "ESP_ERR_NOT_SUPPORTED" not in wheel

# Default descriptors retain all three P4 functions; no code may select the
# independent FS/debug PHY, GPIOs, or eFuse settings.
defaults = body("compiled_default_descriptor_configuration")
for enabled in (".keyboard = true", ".absolute_mouse = true", ".relative_mouse = true"):
    assert enabled in defaults
for forbidden in (
    "config.port = TINYUSB_PORT_FULL_SPEED_0",
    "GPIO_NUM_24",
    "GPIO_NUM_25",
    "GPIO_NUM_26",
    "GPIO_NUM_27",
    "PHY_SEL",
    "efuse",
):
    assert forbidden not in SOURCE, f"target HID component touches forbidden {forbidden}"

# Windows fails to enumerate a USB 2.1 device (OS Services) whose serial
# number string is empty, so an empty serial number reports no string.
bank_builder = body("build_descriptor_bank")
assert re.search(
    r"\.iSerialNumber\s*=\s*configuration->serial_number\[0\]\s*!=\s*'\\0'\s*\?\s*"
    r"STRING_SERIAL\s*:\s*0", bank_builder
), "an empty serial number must give iSerialNumber 0"

print("jetkvm_usb_hid dynamic descriptor contract: PASS")

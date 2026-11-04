#!/usr/bin/env python3
"""Static contract for the read-only target-host USB configuration view."""

from pathlib import Path
import re


COMPONENT = Path(__file__).resolve().parents[1]
REPO = COMPONENT.parents[1]
SOURCE = (COMPONENT / "jetkvm_usb_hid.c").read_text(encoding="utf-8")
HEADER = (COMPONENT / "include" / "jetkvm_usb_hid.h").read_text(encoding="utf-8")
CONTROL_SOURCE = "\n".join(
    (REPO / "components" / "control_protocol" / name).read_text(
        encoding="utf-8"
    )
    for name in ("jetkvm_control.c", "rpc_usb.c")
)
CONTROL_HEADER = (
    REPO / "components" / "control_protocol" / "include" / "jetkvm_control.h"
).read_text(encoding="utf-8")
BRIDGE_SOURCE = (
    REPO / "components" / "jetkvm_hid_input" / "jetkvm_hid_input.c"
).read_text(encoding="utf-8")
TRANSPORT_SOURCE = "\n".join(
    (REPO / "components" / "webrtc_transport" / name).read_text(
        encoding="utf-8"
    )
    for name in (
        "webrtc_transport.c",
        "webrtc_data_channels.c",
        "webrtc_transport_rpc_adapters.c",
        "rpc_worker.c",
    )
)


def body(source: str, symbol: str) -> str:
    match = re.search(rf"\b{re.escape(symbol)}\s*\([^)]*\)\s*\{{", source)
    assert match, f"missing function {symbol}"
    depth = 1
    cursor = match.end()
    while depth and cursor < len(source):
        depth += source[cursor] == "{"
        depth -= source[cursor] == "}"
        cursor += 1
    assert depth == 0, f"unterminated function {symbol}"
    return source[match.end() : cursor - 1]


assert "jetkvm_usb_hid_configuration_snapshot_t" in HEADER
assert "jetkvm_usb_hid_get_configuration_snapshot" in HEADER
assert "independent USB Serial/JTAG debug port" in HEADER

product_id_formula = re.search(
    r"#define JETKVM_USB_HID_DEFAULT_PRODUCT_ID(.*?)(?:\n\n|\n/\*)",
    SOURCE,
    re.S,
)
assert product_id_formula, "missing esp_tinyusb-compatible PID formula"
for usb_class in ("CDC", "MSC", "HID", "MIDI", "AUDIO", "VENDOR"):
    assert f"CFG_TUD_{usb_class}" in product_id_formula.group(1)

getter = body(SOURCE, "jetkvm_usb_hid_get_configuration_snapshot")
for required in (
    "ensure_descriptor_bank_ready()",
    "s_descriptor_banks[s_selected_descriptor_bank]",
    "bank.device.idVendor",
    "bank.device.idProduct",
    "bank.string_storage[STRING_SERIAL]",
    "bank.string_storage[STRING_MANUFACTURER]",
    "bank.string_storage[STRING_PRODUCT]",
    "bank.instance_by_function[HID_FUNCTION_KEYBOARD]",
    "bank.instance_by_function[HID_FUNCTION_ABSOLUTE_MOUSE]",
    "bank.instance_by_function[HID_FUNCTION_RELATIVE_MOUSE]",
    "JETKVM_USB_HID_INTERFACE_DISABLED",
    "snapshot->mass_storage = bank.mass_storage",
    "snapshot->serial_console = false",
    "snapshot->audio = bank.audio",
    "snapshot->emulation_enabled = emulation_enabled",
    "portENTER_CRITICAL(&s_state_lock)",
):
    assert required in getter, f"configuration getter is missing {required}"
for forbidden in (
    "tinyusb_driver_install",
    "tinyusb_driver_uninstall",
    "tinyusb_remote_wakeup",
    "tud_hid_n_report",
):
    assert forbidden not in getter, f"read-only getter mutates hardware via {forbidden}"

setter = body(SOURCE, "jetkvm_usb_hid_set_descriptor_configuration")
assert "select_descriptor_configuration(configuration)" in setter
selector = body(SOURCE, "select_descriptor_configuration")
for lifecycle_guard in (
    "s_ctx.state.initialized",
    "s_ctx.initializing",
    "s_ctx.deinitializing",
    "s_ctx.stopping",
    "s_ctx.configuring",
):
    assert lifecycle_guard in selector
for forbidden in ("tinyusb_driver_install", "tinyusb_driver_uninstall"):
    assert forbidden not in selector

default_config = body(SOURCE, "compiled_default_descriptor_configuration")
for enabled in (
    ".keyboard = true",
    ".absolute_mouse = true",
    ".relative_mouse = true",
    ".mass_storage = true",
):
    assert enabled in default_config

assert "jetkvm_usb_configuration_snapshot_t" in CONTROL_HEADER
assert "jetkvm_usb_configuration_context_t usb_configuration" in CONTROL_HEADER
for method, handler in (
    ("getUsbDevices", "rpc_get_usb_devices"),
    ("getUsbConfig", "rpc_get_usb_config"),
    ("getUsbEmulationState", "rpc_get_usb_emulation_state"),
):
    assert f'{{ "{method}", {handler} }}' in CONTROL_SOURCE
    assert "require_no_rpc_params(params, dispatch)" in body(
        CONTROL_SOURCE, handler
    )

bridge = body(BRIDGE_SOURCE, "jetkvm_hid_input_get_usb_configuration")
assert "jetkvm_usb_hid_get_configuration_snapshot(&usb)" in bridge
assert ".emulation_enabled = usb.emulation_enabled" in bridge
transport_getter = body(TRANSPORT_SOURCE, "rpc_get_usb_configuration")
assert "jetkvm_usb_service_get_configuration(&configuration)" in transport_getter
assert "jetkvm_usb_service_get_snapshot(&service)" in transport_getter
for published_field in (
    "snapshot->vendor_id = configuration.vendor_id",
    "snapshot->product_id = configuration.product_id",
    "snapshot->absolute_mouse = configuration.absolute_mouse",
    "snapshot->relative_mouse = configuration.relative_mouse",
    "snapshot->keyboard = configuration.keyboard",
):
    assert published_field in transport_getter
assert ".get = rpc_get_usb_configuration" in TRANSPORT_SOURCE
assert ".set_descriptor_config = rpc_set_usb_descriptor_config" in TRANSPORT_SOURCE
assert ".set_devices = rpc_set_usb_devices_adapter" in TRANSPORT_SOURCE
assert ".set_device_state = rpc_set_usb_device_state_adapter" in TRANSPORT_SOURCE
assert ".set_emulation_enabled = set_emulation" in TRANSPORT_SOURCE

print("jetkvm_usb_hid read-only configuration contract: PASS")

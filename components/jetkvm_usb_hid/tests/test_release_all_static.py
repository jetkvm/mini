#!/usr/bin/env python3
"""Static contract for idempotent all-up HID release."""

from pathlib import Path
import re


SOURCE = (
    Path(__file__).resolve().parents[1] / "jetkvm_usb_hid.c"
).read_text(encoding="utf-8")

match = re.search(
    r"esp_err_t jetkvm_usb_hid_release_all\(void\)\n\{(.*?)\n\}",
    SOURCE,
    re.DOTALL,
)
assert match is not None, "release_all implementation is missing"
body = match.group(1)

# The decision is based on state committed only after TinyUSB accepted a
# report. A detach/reset callback also clears these same shadows.
for shadow in (
    "s_ctx.keyboard_modifier",
    "s_ctx.keyboard_keys",
    "s_ctx.absolute_pointer.buttons",
    "s_ctx.relative_buttons",
):
    assert shadow in body, f"release_all does not snapshot {shadow}"

assert re.search(
    r"if \(!keyboard_released &&\s*"
    r"active_function_enabled\(HID_FUNCTION_KEYBOARD\)\)",
    body,
)
assert re.search(
    r"if \(absolute_buttons != 0U &&\s*"
    r"active_function_enabled\(HID_FUNCTION_ABSOLUTE_MOUSE\)\)",
    body,
)
assert re.search(
    r"if \(relative_buttons != 0U &&\s*"
    r"active_function_enabled\(HID_FUNCTION_RELATIVE_MOUSE\)\)",
    body,
)

# A true down state retains the original fail-closed transmit paths.
for call in (
    "jetkvm_usb_hid_keyboard_report(0, NULL)",
    "jetkvm_usb_hid_absolute_mouse_report(",
    "jetkvm_usb_hid_relative_mouse_report(0, 0, 0, 0, 0)",
):
    assert call in body, f"release path disappeared: {call}"

print("idempotent release-all static contract: OK")

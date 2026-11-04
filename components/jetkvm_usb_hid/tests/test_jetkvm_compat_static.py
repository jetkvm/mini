#!/usr/bin/env python3
"""Pin host-visible HID descriptors to the current JetKVM implementation.

Fixture provenance: jetkvm/kvm commit
b3c29a44d9e2862b8ff7530830781803ce27b060, files
internal/usbgadget/hid_keyboard.go, hid_mouse_absolute.go,
hid_mouse_relative.go and config.go.
"""

from hashlib import sha256
from pathlib import Path
import re


SOURCE = (Path(__file__).resolve().parents[1] / "jetkvm_usb_hid.c").read_text(
    encoding="utf-8"
)

EXPECTED = {
    "s_keyboard_report_descriptor": (
        63,
        "f3e62a32638f14efff361d5f49be6a23cce36f13ff900522aedd41935b3c3721",
    ),
    "s_absolute_mouse_report_descriptor": (
        93,
        "31ed91fe5a53642cefa538d1376b92c2e6034e533648aa21a3d6ac156a8acb94",
    ),
    "s_relative_mouse_report_descriptor": (
        61,
        "92155ddf022d4091f5e15eea5871107cd6ff7a4e6ada0ef931bd14b86c5632d0",
    ),
    "s_wake_report_descriptor": (
        21,
        "a003424f933a7c9fce0ae07b49c213ec6b9fe76b5ebd71dd906aaeba80c30ab6",
    ),
}


def descriptor(symbol: str) -> bytes:
    match = re.search(rf"{symbol}\[\] = \{{(.*?)\n\}};", SOURCE, re.DOTALL)
    assert match, f"missing {symbol}"
    body = re.sub(r"/\*.*?\*/", "", match.group(1), flags=re.DOTALL)
    return bytes(int(item, 16) for item in re.findall(r"0x[0-9a-fA-F]+", body))


for name, (expected_length, expected_hash) in EXPECTED.items():
    value = descriptor(name)
    assert len(value) == expected_length, (name, len(value), expected_length)
    assert sha256(value).hexdigest() == expected_hash, name

# Pin the upstream HID topology plus the P4 composite MSC extension: keyboard,
# absolute mouse, relative mouse, wake HID, then one bulk-only MSC interface.
for contract in (
    "HID_FUNCTION_KEYBOARD = 0",
    "HID_FUNCTION_ABSOLUTE_MOUSE",
    "HID_FUNCTION_RELATIVE_MOUSE",
    "HID_FUNCTION_WAKE",
    "ENDPOINT_KEYBOARD = 1",
    "ENDPOINT_ABSOLUTE_MOUSE",
    "ENDPOINT_RELATIVE_MOUSE",
    "ENDPOINT_WAKE",
    "ENDPOINT_MSC",
    "TUD_MSC_DESCRIPTOR",
    "ENDPOINT_MSC, (0x80U | ENDPOINT_MSC)",
    "case HID_FUNCTION_WAKE:",
    "return true;",
):
    assert contract in SOURCE, f"missing JetKVM HID topology contract: {contract}"

print("jetkvm_usb_hid upstream compatibility contract: PASS")

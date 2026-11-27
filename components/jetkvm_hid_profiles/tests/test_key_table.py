#!/usr/bin/env python3
from pathlib import Path
import re

REPO = Path(__file__).resolve().parents[3]
mapping = (REPO / "frontend/jetkvm-ui/src/keyboardMappings.ts").read_text()
match = re.search(
    r"export const keys = \{(.*?)\n\} as Record<string, number>;",
    mapping,
    re.S,
)
assert match
frontend = re.findall(r"^\s{2}([A-Za-z0-9_]+):", match.group(1), re.M)
frontend_codes = [
    int(code, 16)
    for code in re.findall(
        r"^\s{2}[A-Za-z0-9_]+:\s*(0x[0-9a-fA-F]+),", match.group(1), re.M)
]

include = (
    REPO / "components/control_protocol/keyboard_key_names.inc"
).read_text()
firmware = re.findall(r'^\s*"([A-Za-z0-9_]+)",$', include, re.M)

assert firmware == frontend, "firmware saved-macro key table drifted from the UI"
assert len(firmware) == 230

# The HID usage IDs that MQTT and runKeyboardMacro type, in the same order.
codes = (
    REPO / "components/control_protocol/keyboard_key_codes.inc"
).read_text()
firmware_codes = re.findall(
    r"^\s*(0x[0-9a-f]{2}), /\* ([A-Za-z0-9_]+) \*/$", codes, re.M)
assert [name for _, name in firmware_codes] == frontend, \
    "firmware key code table is not in the order of the key names"
assert [int(code, 16) for code, _ in firmware_codes] == frontend_codes, \
    "firmware key code table drifted from the UI"
assert len(codes.splitlines()) == len(firmware_codes)

modifiers = re.search(
    r"export const modifiers = \{(.*?)\n\} as Record<string, number>;",
    mapping,
    re.S,
)
assert modifiers
frontend_masks = dict(
    (name, int(mask, 16))
    for name, mask in re.findall(
        r"^\s{2}([A-Za-z0-9_]+):\s*(0x[0-9a-fA-F]+),", modifiers.group(1),
        re.M))
macros = (REPO / "components/control_protocol/rpc_macros.c").read_text()
hid = (REPO / "components/control_protocol/keyboard_macro_hid.c").read_text()
names = re.search(
    r"s_keyboard_macro_modifier_names\[\] = \{(.*?)\};", macros, re.S)
masks = re.search(
    r"s_keyboard_macro_modifier_masks\[\] = \{(.*?)\};", hid, re.S)
assert names and masks
modifier_names = re.findall(r'"([A-Za-z]+)"', names.group(1))
modifier_masks = [int(mask, 16) for mask in re.findall(r"0x([0-9a-fA-F]+)U",
                                                       masks.group(1))]
assert len(modifier_names) == len(modifier_masks) == 8
for name, mask in zip(modifier_names, modifier_masks):
    assert frontend_masks[name] == mask, f"modifier mask of {name} drifted"
print("Saved-macro key and modifier tables: OK")


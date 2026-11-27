#!/usr/bin/env python3
"""The USB string table fits the limit that esp_tinyusb is patched to.

tinyusb_driver_install returns ESP_ERR_NOT_SUPPORTED when string_count is
above USB_STRING_DESCRIPTOR_ARRAY_SIZE, and the Mini then has no USB at all.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "patches" / "esp_tinyusb-2.2.1-nine-strings.patch"
CMAKE = ROOT / "CMakeLists.txt"
HID = ROOT / "components" / "jetkvm_usb_hid" / "jetkvm_usb_hid.c"
PATCHED_HEADER = (ROOT / "managed_components" / "espressif__esp_tinyusb" /
                  "include_private" / "descriptors_control.h")
LIMIT = re.compile(r"^#define USB_STRING_DESCRIPTOR_ARRAY_SIZE\s+(\d+)", re.M)

patch_text = PATCH.read_text(encoding="utf-8")
added = "\n".join(line[1:] for line in patch_text.splitlines()
                  if line.startswith("+") and not line.startswith("+++"))
removed = "\n".join(line[1:] for line in patch_text.splitlines()
                    if line.startswith("-") and not line.startswith("---"))
assert LIMIT.search(removed).group(1) == "8", "patch must start from 8"
patched_limit = int(LIMIT.search(added).group(1))

assert ("apply_managed_component_patch("
        "patches/esp_tinyusb-2.2.1-nine-strings.patch)") in \
    CMAKE.read_text(encoding="utf-8")

# Count the entries of the string index enum up to STRING_COUNT.
source = HID.read_text(encoding="utf-8")
enum = re.search(r"enum \{\s*STRING_LANGID = 0,(.*?)STRING_COUNT,", source, re.S)
assert enum, "missing string index enum"
names = re.findall(r"^\s*(STRING_[A-Z_]+),", enum.group(1), re.M)
string_count = 1 + len(names)
assert string_count <= patched_limit, (
    f"{string_count} USB strings, esp_tinyusb is patched to {patched_limit}")
# The configuration name is last, so the interface string indexes stay.
assert names[-1] == "STRING_CONFIGURATION", names
assert names.index("STRING_MSC_CDROM") == 6, names

# After `idf.py reconfigure`, the managed component carries the patch.
if PATCHED_HEADER.exists():
    header_limit = int(LIMIT.search(
        PATCHED_HEADER.read_text(encoding="utf-8")).group(1))
    assert header_limit == patched_limit, (
        f"{PATCHED_HEADER} has {header_limit}; run idf.py reconfigure")

print("esp_tinyusb nine-strings patch: PASS")

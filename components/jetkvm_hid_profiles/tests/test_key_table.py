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

include = (
    REPO / "components/control_protocol/keyboard_key_names.inc"
).read_text()
firmware = re.findall(r'^\s*"([A-Za-z0-9_]+)",$', include, re.M)

assert firmware == frontend, "firmware saved-macro key table drifted from the UI"
assert len(firmware) == 230
print("Saved-macro key table: OK")


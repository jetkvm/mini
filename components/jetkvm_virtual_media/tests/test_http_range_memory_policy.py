#!/usr/bin/env python3
"""Static ownership contract for HTTPS virtual-media TLS headroom."""

from pathlib import Path
import re


COMPONENT = Path(__file__).resolve().parents[1]
SOURCE = (COMPONENT / "http_range_client.c").read_text(encoding="utf-8")
CMAKE = (COMPONENT / "CMakeLists.txt").read_text(encoding="utf-8")

for contract in (
    '"https-media-check"',
    '"https-media-range"',
    '"https-media-connect"',
    "jetkvm_memory_policy_wait_reserve_tls",
    "jetkvm_memory_policy_wait_reserve_tls_open_connection",
    "jetkvm_memory_policy_release(&tls_memory)",
    "jetkvm_memory_policy_control_alloc(sizeof(*created))",
    "jetkvm_memory_policy_control_alloc(url_size + 1U)",
    "jetkvm_memory_policy_control_free(reader->url)",
    "jetkvm_memory_policy_control_free(reader)",
):
    assert contract in SOURCE, f"missing HTTPS memory contract: {contract}"

reader_struct = SOURCE[
    SOURCE.index("struct jetkvm_http_range_reader") :
    SOURCE.index("static bool url_uses_tls")
]
assert "memory_policy_reservation" not in reader_struct, (
    "HTTPS reader must not pin a lifetime TLS ticket"
)
assert "jetkvm_memory_policy" in CMAKE
assert not re.search(r"\b(?:calloc|strdup)\s*\(", SOURCE)

print("HTTPS virtual-media TLS memory ownership: PASS")

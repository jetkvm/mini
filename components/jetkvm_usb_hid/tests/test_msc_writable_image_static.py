#!/usr/bin/env python3
"""Static TinyUSB contract for writable local Disk images."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parents[1]
SOURCE = (ROOT / "jetkvm_usb_hid.c").read_text(encoding="utf-8")
HEADER = (ROOT / "include/jetkvm_usb_hid.h").read_text(encoding="utf-8")
CMAKE = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
TINYUSB_PATCH = REPO / "patches" / "tinyusb-0.19.0-msc-write-cache.patch"


def body(symbol: str, next_symbol: str) -> str:
    start = SOURCE.index(symbol)
    end = SOURCE.index(next_symbol, start)
    return SOURCE[start:end]


writable = body("bool tud_msc_is_writable_cb", "void tud_msc_capacity_cb")
write = body("int32_t tud_msc_write10_cb", "int32_t tud_msc_scsi_cb")
scsi = body("int32_t tud_msc_scsi_cb", "uint16_t tud_hid_get_report_cb")
start_stop = body("bool tud_msc_start_stop_cb", "int32_t tud_msc_read10_cb")

assert "jetkvm_virtual_media_is_writable()" in writable
assert "tud_msc_has_write_cache_cb" in writable

for required in (
    "tud_msc_is_writable_cb(lun)",
    "SCSI_SENSE_DATA_PROTECT",
    "SCSI_SENSE_ILLEGAL_REQUEST",
    "jetkvm_virtual_media_write(generation, byte_offset, buffer",
    "s_ctx.state.msc_write_bytes += buffer_size",
    "s_ctx.state.msc_write_errors++",
):
    assert required in write, f"WRITE(10) contract missing: {required}"

assert "scsi_command[0] == 0x35U" in scsi
assert "scsi_command[0] == 0x91U" in scsi
assert "scsi_command[0] == 0x5aU" in scsi
assert "response[10]" in scsi and "0x04U" in scsi
assert "jetkvm_virtual_media_sync()" in scsi
assert "jetkvm_virtual_media_eject(generation)" in start_stop
assert "jetkvm_virtual_media_sync()" in start_stop

for field in (
    "msc_write_calls", "msc_write_bytes", "msc_write_errors",
    "msc_write_commands", "msc_write_min_size", "msc_write_max_size",
):
    assert field in HEADER, f"USB write telemetry missing: {field}"

assert TINYUSB_PATCH.is_file(), "managed TinyUSB write-cache patch is missing"
tinyusb_patch = TINYUSB_PATCH.read_text(encoding="utf-8")
assert "tud_msc_has_write_cache_cb" in tinyusb_patch
assert "page_code == 0x08" in tinyusb_patch
assert "mode_resp.flags" in tinyusb_patch
assert CMAKE.count("tinyusb-0.19.0-msc-write-cache.patch") == 1

print("jetkvm_usb_hid writable MSC image contract: PASS")

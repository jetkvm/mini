#!/usr/bin/env python3
"""Static safety contract for writable local virtual-disk images."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "jetkvm_virtual_media.c").read_text(encoding="utf-8")
HEADER = (ROOT / "include/jetkvm_virtual_media.h").read_text(encoding="utf-8")


def body(symbol: str, next_symbol: str) -> str:
    start = SOURCE.index(symbol)
    end = SOURCE.index(next_symbol, start)
    return SOURCE[start:end]


mount = body("esp_err_t jetkvm_virtual_media_mount(",
             "static esp_err_t check_trimmed_url")
write = body("esp_err_t jetkvm_virtual_media_write(",
             "esp_err_t jetkvm_virtual_media_sync(")
sync = body("esp_err_t jetkvm_virtual_media_sync(",
            "esp_err_t jetkvm_virtual_media_get_io_stats(")
unmount = body("static esp_err_t unmount_medium(",
               "esp_err_t jetkvm_virtual_media_read(")

assert "s_ctx.image_size % JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE == 0U" in body(
    "static bool storage_image_writable_locked(", "}\n"), \
    "a Disk image with a partial last block must stay write-protected"

for required in (
    "mode == JETKVM_VIRTUAL_MEDIA_MODE_DISK",
    "open_mode |= FA_WRITE",
    "FA_OPEN_EXISTING",
):
    assert required in mount, f"mount missing writable-disk guard: {required}"

for required in (
    "storage_image_writable_locked()",
    "offset + size > s_ctx.image_size",
    "offset % JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE != 0U",
    "size % JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE != 0U",
    "prefetch_invalidate_locked()",
    "f_write(&s_ctx.image",
    "written != size",
    "s_ctx.image_dirty = true",
):
    assert required in write, f"write path missing safety rule: {required}"

assert "storage_sync_locked()" in sync
assert "if (!s_ctx.image_open)" in sync

close = unmount.index("result = f_close(&s_ctx.image)")
close_error = unmount.index("if (result != FR_OK)", close)
clear_state = unmount.index("s_ctx.image_open = false", close_error)
assert close < close_error < clear_state
assert "return fat_result(result);" in unmount[close_error:clear_state]

for required in (
    "bool jetkvm_virtual_media_is_writable(void);",
    "esp_err_t jetkvm_virtual_media_write(",
    "esp_err_t jetkvm_virtual_media_sync(void);",
    "jetkvm_virtual_media_get_write_io_stats(",
):
    assert required in HEADER, f"public writable-media API missing: {required}"

assert "_Static_assert(FF_FS_READONLY == 0" in SOURCE
print("virtual-media writable local Disk contract: PASS")

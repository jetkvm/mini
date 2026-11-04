#!/usr/bin/env python3
"""Read what a JetKVM Mini app image says about itself.

Layout, per ESP-IDF's app image format and components/jetkvm_ota:

  0    esp_image_header_t          24 bytes
  24   esp_image_segment_header_t   8 bytes
  32   esp_app_desc_t             256 bytes; version[32] at +16
  288  jetkvm_ota_image_desc_t     64 bytes; magic "JKSK" then sku[32]

Usage: mini_image.py <image.bin>   prints "<version> <sku>"
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

APP_DESC_OFFSET = 24 + 8
APP_DESC_MAGIC = 0xABCD5432
VERSION_OFFSET = APP_DESC_OFFSET + 16
IMAGE_DESC_OFFSET = APP_DESC_OFFSET + 256
IMAGE_DESC_MAGIC = 0x4B534B4A  # "JKSK" little-endian, jetkvm_ota.h
SKU_OFFSET = IMAGE_DESC_OFFSET + 4
HEAD_BYTES = IMAGE_DESC_OFFSET + 64


def _cstring(data: bytes, offset: int, size: int) -> str:
    return data[offset : offset + size].split(b"\0", 1)[0].decode("ascii", "replace")


def app_version(head: bytes) -> str:
    """esp_app_desc_t.version, or '' when the app descriptor magic is absent."""
    if len(head) < APP_DESC_OFFSET + 4:
        return ""
    (magic,) = struct.unpack_from("<I", head, APP_DESC_OFFSET)
    if magic != APP_DESC_MAGIC:
        return ""
    return _cstring(head, VERSION_OFFSET, 32)


def image_sku(head: bytes) -> str:
    """The SKU marker the installer checks, or '' when the image has none."""
    if len(head) < HEAD_BYTES:
        return ""
    (magic,) = struct.unpack_from("<I", head, IMAGE_DESC_OFFSET)
    if magic != IMAGE_DESC_MAGIC:
        return ""
    return _cstring(head, SKU_OFFSET, 32)


def read_head(path: Path) -> bytes:
    with path.open("rb") as handle:
        return handle.read(HEAD_BYTES)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    head = read_head(Path(argv[1]))
    print(app_version(head), image_sku(head))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

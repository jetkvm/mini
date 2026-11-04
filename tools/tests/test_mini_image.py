import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import mini_image  # noqa: E402


def synthetic_image(version: bytes | None, sku: bytes | None, marker: bool = True) -> bytes:
    """An image head with the two descriptors laid out as the firmware does."""
    head = bytearray(b"\xff" * mini_image.HEAD_BYTES)
    if version is not None:
        struct.pack_into("<I", head, mini_image.APP_DESC_OFFSET, mini_image.APP_DESC_MAGIC)
        head[mini_image.VERSION_OFFSET : mini_image.VERSION_OFFSET + 32] = version.ljust(32, b"\0")
    if sku is not None:
        if marker:
            struct.pack_into("<I", head, mini_image.IMAGE_DESC_OFFSET, mini_image.IMAGE_DESC_MAGIC)
        head[mini_image.SKU_OFFSET : mini_image.SKU_OFFSET + 32] = sku.ljust(32, b"\0")
    return bytes(head)


class MiniImageTest(unittest.TestCase):
    def test_reads_version_and_sku(self):
        head = synthetic_image(b"0.5.0-p4.2", b"jetkvm-mini-wireless")
        self.assertEqual(mini_image.app_version(head), "0.5.0-p4.2")
        self.assertEqual(mini_image.image_sku(head), "jetkvm-mini-wireless")

    def test_offsets_match_the_firmware_layout(self):
        # 24-byte image header, 8-byte segment header, 256-byte esp_app_desc_t,
        # then the 64-byte jetkvm_ota_image_desc_t (jetkvm_ota.c).
        self.assertEqual(mini_image.VERSION_OFFSET, 48)
        self.assertEqual(mini_image.IMAGE_DESC_OFFSET, 288)
        self.assertEqual(mini_image.HEAD_BYTES, 352)

    def test_sku_without_marker_is_absent(self):
        head = synthetic_image(b"0.5.0-p4.2", b"jetkvm-mini-wireless", marker=False)
        self.assertEqual(mini_image.image_sku(head), "")

    def test_erased_flash_reads_as_nothing(self):
        head = b"\xff" * mini_image.HEAD_BYTES
        self.assertEqual(mini_image.app_version(head), "")
        self.assertEqual(mini_image.image_sku(head), "")

    def test_short_input_reads_as_nothing(self):
        head = synthetic_image(b"0.5.0-p4.2", b"jetkvm-mini-wireless")[:100]
        self.assertEqual(mini_image.app_version(head), "0.5.0-p4.2")
        self.assertEqual(mini_image.image_sku(head), "")


if __name__ == "__main__":
    unittest.main()

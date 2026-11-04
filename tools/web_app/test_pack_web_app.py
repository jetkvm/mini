#!/usr/bin/env python3

from __future__ import annotations

import binascii
import gzip
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import pack_web_app as packer


class WebAppPackTest(unittest.TestCase):
    def make_tree(self, root: Path) -> dict[str, bytes]:
        files = {
            "index.html": b"<!doctype html><script src='/static/assets/immutable/app-a1.js'></script>",
            "assets/immutable/app-a1.js": b"console.log('JetKVM');\n" * 40,
            "assets/theme.css": b"body { color: #fff; }\n" * 20,
            "favicon.png": b"\x89PNG\r\n\x1a\nraw-png-fixture",
            "fonts/ui.woff2": b"wOF2raw-font-fixture",
            "fonts/ui-alias.woff2": b"wOF2raw-font-fixture",
        }
        for relative, content in files.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        return files

    def test_pack_is_deterministic_and_response_metadata_matches(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = self.make_tree(root)
            first, manifest = packer.build_pack(root, 256 * 1024)
            second, second_manifest = packer.build_pack(root, 256 * 1024)
            self.assertEqual(first, second)
            self.assertEqual(manifest, second_manifest)
            parsed = packer.validate_pack(first, 256 * 1024)
            self.assertEqual(len(parsed), len(source))

            by_url = {entry["url"]: entry for entry in manifest["entries"]}
            index = by_url["/static/index.html"]
            self.assertEqual(index["mime"], "text/html; charset=utf-8")
            self.assertEqual(index["contentEncoding"], "gzip")
            self.assertEqual(index["cacheControl"], "no-cache")
            self.assertEqual(
                gzip.decompress(first[index["offset"] : index["offset"] + index["size"]]),
                source["index.html"],
            )

            script = by_url["/static/assets/immutable/app-a1.js"]
            self.assertEqual(script["contentEncoding"], "gzip")
            self.assertEqual(
                script["cacheControl"], "public, max-age=31536000, immutable"
            )
            png = by_url["/static/favicon.png"]
            font = by_url["/static/fonts/ui.woff2"]
            font_alias = by_url["/static/fonts/ui-alias.woff2"]
            self.assertIsNone(png["contentEncoding"])
            self.assertIsNone(font["contentEncoding"])
            self.assertEqual(
                first[png["offset"] : png["offset"] + png["size"]],
                source["favicon.png"],
            )
            self.assertEqual(
                first[font["offset"] : font["offset"] + font["size"]],
                source["fonts/ui.woff2"],
            )
            self.assertEqual(font_alias["offset"], font["offset"])
            self.assertEqual(font_alias["size"], font["size"])
            self.assertEqual(font_alias["crc32"], font["crc32"])
            self.assertEqual(
                first[
                    font_alias["offset"] : font_alias["offset"]
                    + font_alias["size"]
                ],
                source["fonts/ui-alias.woff2"],
            )
            self.assertEqual(manifest["uniquePayloadCount"], len(source) - 1)
            self.assertEqual(
                manifest["deduplicatedBytes"], len(source["fonts/ui-alias.woff2"])
            )
            self.assertEqual(png["cacheControl"], "public, max-age=300")
            self.assertTrue(index["etag"].startswith('"sha256-'))

    def test_pack_rejects_partition_overflow(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_tree(root)
            with self.assertRaisesRegex(packer.PackError, "exceeding partition size"):
                packer.build_pack(root, 128)

    def test_reference_parser_rejects_manifest_and_range_bounds(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_tree(root)
            image, _ = packer.build_pack(root, 256 * 1024)

            corrupt_manifest = bytearray(image)
            corrupt_manifest[packer.HEADER_SIZE] ^= 0x01
            with self.assertRaisesRegex(packer.PackError, "manifest CRC32"):
                packer.validate_pack(bytes(corrupt_manifest), 256 * 1024)

            corrupt_entry = bytearray(image)
            struct.pack_into("<I", corrupt_entry, packer.HEADER_SIZE + 4, len(image) + 4)
            fields = list(packer.HEADER.unpack_from(corrupt_entry))
            entries_offset = fields[4]
            strings_offset = fields[6]
            strings_size = fields[7]
            fields[11] = binascii.crc32(
                corrupt_entry[entries_offset : strings_offset + strings_size]
            ) & 0xFFFFFFFF
            fields[12] = 0
            header = bytearray(packer.HEADER.pack(*fields))
            fields[12] = binascii.crc32(header) & 0xFFFFFFFF
            corrupt_entry[: packer.HEADER_SIZE] = packer.HEADER.pack(*fields)
            with self.assertRaisesRegex(packer.PackError, "asset.*bounds"):
                packer.validate_pack(bytes(corrupt_entry), 256 * 1024)

    def test_missing_index_and_symlink_fail_clearly(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "other.js").write_text("x", encoding="utf-8")
            with self.assertRaisesRegex(packer.PackError, "missing required index.html"):
                packer.build_pack(root, 4096)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "index.html").write_text("ok", encoding="utf-8")
            (root / "linked").symlink_to(root / "index.html")
            with self.assertRaisesRegex(packer.PackError, "Symbolic|symbolic"):
                packer.build_pack(root, 8192)


if __name__ == "__main__":
    unittest.main()

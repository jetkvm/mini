#!/usr/bin/env python3
"""Build the deterministic raw-flash web asset pack used by components/web_app."""

from __future__ import annotations

import argparse
import binascii
import gzip
import hashlib
import io
import json
import os
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import struct
import sys
from typing import Any, Iterable


MAGIC = b"JKWPACK\0"
VERSION = 1
HEADER = struct.Struct("<8s14I")
ENTRY = struct.Struct("<6IH4BH")
HEADER_SIZE = 64
ENTRY_SIZE = 32
DATA_ALIGNMENT = 4096
ASSET_ALIGNMENT = 4

ENCODING_IDENTITY = 0
ENCODING_GZIP = 1

CACHE_NO_CACHE = 0
CACHE_IMMUTABLE = 1
CACHE_SHORT = 2

MIME: dict[str, tuple[int, str, bool]] = {
    ".html": (1, "text/html; charset=utf-8", True),
    ".htm": (1, "text/html; charset=utf-8", True),
    ".css": (2, "text/css; charset=utf-8", True),
    ".js": (3, "text/javascript; charset=utf-8", True),
    ".mjs": (3, "text/javascript; charset=utf-8", True),
    ".json": (4, "application/json; charset=utf-8", True),
    ".map": (4, "application/json; charset=utf-8", True),
    ".svg": (5, "image/svg+xml", True),
    ".webmanifest": (6, "application/manifest+json", True),
    ".txt": (7, "text/plain; charset=utf-8", True),
    ".md": (7, "text/plain; charset=utf-8", True),
    ".woff2": (8, "font/woff2", False),
    ".png": (9, "image/png", False),
    ".jpg": (10, "image/jpeg", False),
    ".jpeg": (10, "image/jpeg", False),
    ".gif": (11, "image/gif", False),
    ".webp": (12, "image/webp", False),
    ".ico": (13, "image/x-icon", False),
    ".avif": (14, "image/avif", False),
    ".wasm": (15, "application/wasm", False),
    ".xml": (17, "application/xml; charset=utf-8", True),
}
DEFAULT_MIME = (16, "application/octet-stream", False)
SHORT_CACHE_EXTENSIONS = {
    ".jpg",
    ".jpeg",
    ".png",
    ".svg",
    ".gif",
    ".webp",
    ".ico",
    ".avif",
    ".woff2",
}

CACHE_HEADERS = {
    CACHE_NO_CACHE: "no-cache",
    CACHE_IMMUTABLE: "public, max-age=31536000, immutable",
    CACHE_SHORT: "public, max-age=300",
}


class PackError(ValueError):
    pass


@dataclass(frozen=True)
class SourceAsset:
    source: Path
    url: str
    suffix: str


@dataclass
class PackedAsset:
    source: Path
    url: str
    mime_id: int
    mime: str
    encoding_id: int
    encoding: str | None
    cache_id: int
    cache: str
    source_size: int
    stored: bytes
    content_sha256: bytes
    etag: str
    crc32: int
    path_offset: int = 0
    etag_offset: int = 0
    data_offset: int = 0


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def deterministic_gzip(data: bytes) -> bytes:
    output = io.BytesIO()
    with gzip.GzipFile(
        filename="", mode="wb", compresslevel=9, fileobj=output, mtime=0
    ) as compressor:
        compressor.write(data)
    return output.getvalue()


def discover_assets(root: Path) -> list[SourceAsset]:
    if not root.is_dir():
        raise PackError(f"frontend output directory does not exist: {root}")
    index = root / "index.html"
    if not index.is_file():
        raise PackError(f"frontend output is missing required index.html: {index}")

    assets: list[SourceAsset] = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise PackError(f"symbolic links are not allowed in the web pack: {path}")
        if not path.is_file():
            continue
        relative = PurePosixPath(path.relative_to(root).as_posix())
        relative_text = str(relative)
        if any(character in relative_text for character in ("?", "#", "\\", "\0")):
            raise PackError(f"unsupported character in asset path: {relative_text!r}")
        url = "/static/" + relative_text
        encoded = url.encode("utf-8")
        if len(encoded) > 0xFFFF:
            raise PackError(f"asset URL is too long for the manifest: {url}")
        assets.append(
            SourceAsset(source=path, url=url, suffix=path.suffix.lower())
        )
    assets.sort(key=lambda asset: asset.url.encode("utf-8"))
    return assets


def cache_policy(url: str, suffix: str) -> int:
    if url.startswith("/static/assets/immutable/"):
        return CACHE_IMMUTABLE
    if suffix in SHORT_CACHE_EXTENSIONS:
        return CACHE_SHORT
    return CACHE_NO_CACHE


def load_asset(asset: SourceAsset) -> PackedAsset:
    source_data = asset.source.read_bytes()
    mime_id, mime, is_text = MIME.get(asset.suffix, DEFAULT_MIME)
    if is_text:
        stored = deterministic_gzip(source_data)
        encoding_id = ENCODING_GZIP
        encoding = "gzip"
    else:
        stored = source_data
        encoding_id = ENCODING_IDENTITY
        encoding = None
    content_sha256 = hashlib.sha256(stored).digest()
    etag = f'"sha256-{content_sha256.hex()}"'
    cache_id = cache_policy(asset.url, asset.suffix)
    return PackedAsset(
        source=asset.source,
        url=asset.url,
        mime_id=mime_id,
        mime=mime,
        encoding_id=encoding_id,
        encoding=encoding,
        cache_id=cache_id,
        cache=CACHE_HEADERS[cache_id],
        source_size=len(source_data),
        stored=stored,
        content_sha256=content_sha256,
        etag=etag,
        crc32=binascii.crc32(stored) & 0xFFFFFFFF,
    )


def build_pack(root: Path, max_size: int) -> tuple[bytes, dict[str, Any]]:
    assets = [load_asset(asset) for asset in discover_assets(root)]
    if len(assets) > 0xFFFFFFFF:
        raise PackError("too many web assets")

    entries_offset = HEADER_SIZE
    entries_size = len(assets) * ENTRY_SIZE
    strings_offset = entries_offset + entries_size
    strings = bytearray()
    for asset in assets:
        path_bytes = asset.url.encode("utf-8")
        etag_bytes = asset.etag.encode("ascii")
        if len(etag_bytes) > 0xFF:
            raise PackError(f"ETag is too long for {asset.url}")
        asset.path_offset = strings_offset + len(strings)
        strings.extend(path_bytes)
        strings.append(0)
        asset.etag_offset = strings_offset + len(strings)
        strings.extend(etag_bytes)
        strings.append(0)

    data_offset = align_up(strings_offset + len(strings), DATA_ALIGNMENT)
    data = bytearray()
    # Hashes make candidate lookup cheap, but exact byte comparison is the
    # authority. A theoretical SHA-256 collision must never alias two distinct
    # HTTP representations.
    payloads: dict[tuple[int, bytes], list[PackedAsset]] = {}
    unique_payload_count = 0
    deduplicated_bytes = 0
    for asset in assets:
        key = (len(asset.stored), asset.content_sha256)
        duplicate = next(
            (
                candidate
                for candidate in payloads.get(key, [])
                if candidate.stored == asset.stored
            ),
            None,
        )
        if duplicate is not None:
            asset.data_offset = duplicate.data_offset
            deduplicated_bytes += len(asset.stored)
            continue
        aligned = align_up(len(data), ASSET_ALIGNMENT)
        data.extend(b"\0" * (aligned - len(data)))
        asset.data_offset = data_offset + len(data)
        data.extend(asset.stored)
        payloads.setdefault(key, []).append(asset)
        unique_payload_count += 1

    pack_size = data_offset + len(data)
    if pack_size > max_size:
        raise PackError(
            f"web pack is {pack_size} bytes, exceeding partition size {max_size} "
            f"by {pack_size - max_size} bytes"
        )
    if pack_size > 0xFFFFFFFF:
        raise PackError("web pack offsets exceed the version-1 32-bit format")

    entry_bytes = bytearray()
    for asset in assets:
        entry_bytes.extend(
            ENTRY.pack(
                asset.path_offset,
                asset.data_offset,
                len(asset.stored),
                asset.source_size,
                asset.etag_offset,
                asset.crc32,
                len(asset.url.encode("utf-8")),
                len(asset.etag.encode("ascii")),
                asset.mime_id,
                asset.encoding_id,
                asset.cache_id,
                0,
            )
        )
    manifest_crc = binascii.crc32(entry_bytes + strings) & 0xFFFFFFFF
    header_values = (
        MAGIC,
        VERSION,
        HEADER_SIZE,
        len(assets),
        entries_offset,
        entries_size,
        strings_offset,
        len(strings),
        data_offset,
        len(data),
        pack_size,
        manifest_crc,
        0,
        0,
        0,
    )
    header_without_crc = HEADER.pack(*header_values)
    header_crc = binascii.crc32(header_without_crc) & 0xFFFFFFFF
    header_values = header_values[:12] + (header_crc,) + header_values[13:]
    header = HEADER.pack(*header_values)

    pack = bytearray(header)
    pack.extend(entry_bytes)
    pack.extend(strings)
    pack.extend(b"\0" * (data_offset - len(pack)))
    pack.extend(data)
    if len(pack) != pack_size:
        raise AssertionError("internal pack size mismatch")

    manifest: dict[str, Any] = {
        "format": "jetkvm-web-pack",
        "version": VERSION,
        "packSize": pack_size,
        "partitionSize": max_size,
        "dataOffset": data_offset,
        "uniquePayloadCount": unique_payload_count,
        "deduplicatedBytes": deduplicated_bytes,
        "manifestCrc32": f"{manifest_crc:08x}",
        "headerCrc32": f"{header_crc:08x}",
        "entries": [
            {
                "url": asset.url,
                "offset": asset.data_offset,
                "size": len(asset.stored),
                "sourceSize": asset.source_size,
                "mime": asset.mime,
                "contentEncoding": asset.encoding,
                "cacheControl": asset.cache,
                "etag": asset.etag,
                "crc32": f"{asset.crc32:08x}",
            }
            for asset in assets
        ],
    }
    validate_pack(bytes(pack), max_size)
    return bytes(pack), manifest


def _checked_range(offset: int, size: int, limit: int, name: str) -> None:
    if offset < 0 or size < 0 or offset > limit or size > limit - offset:
        raise PackError(f"{name} is outside pack bounds")


def validate_pack(pack: bytes, partition_size: int) -> list[dict[str, Any]]:
    """Reference parser used by host tests and as a packer self-check."""
    if len(pack) < HEADER_SIZE:
        raise PackError("truncated pack header")
    unpacked = HEADER.unpack_from(pack)
    (
        magic,
        version,
        header_size,
        entry_count,
        entries_offset,
        entries_size,
        strings_offset,
        strings_size,
        data_offset,
        data_size,
        pack_size,
        manifest_crc,
        header_crc,
        reserved0,
        reserved1,
    ) = unpacked
    if magic != MAGIC or version != VERSION or header_size != HEADER_SIZE:
        raise PackError("bad pack identity or version")
    header_copy = bytearray(pack[:HEADER_SIZE])
    struct.pack_into("<I", header_copy, 52, 0)
    if binascii.crc32(header_copy) & 0xFFFFFFFF != header_crc:
        raise PackError("header CRC32 mismatch")
    if reserved0 or reserved1 or pack_size > partition_size or pack_size != len(pack):
        raise PackError("invalid pack size or reserved header fields")
    if entries_offset != HEADER_SIZE or entries_size != entry_count * ENTRY_SIZE:
        raise PackError("invalid entry table layout")
    _checked_range(entries_offset, entries_size, pack_size, "entry table")
    if strings_offset != entries_offset + entries_size:
        raise PackError("invalid string table offset")
    _checked_range(strings_offset, strings_size, pack_size, "string table")
    if data_offset < strings_offset + strings_size or data_size != pack_size - data_offset:
        raise PackError("invalid data section layout")
    manifest_bytes = pack[entries_offset : strings_offset + strings_size]
    if binascii.crc32(manifest_bytes) & 0xFFFFFFFF != manifest_crc:
        raise PackError("manifest CRC32 mismatch")

    parsed: list[dict[str, Any]] = []
    previous_url: bytes | None = None
    for index in range(entry_count):
        values = ENTRY.unpack_from(pack, entries_offset + index * ENTRY_SIZE)
        (
            path_offset,
            asset_offset,
            stored_size,
            source_size,
            etag_offset,
            content_crc,
            path_length,
            etag_length,
            mime_id,
            encoding_id,
            cache_id,
            reserved,
        ) = values
        _checked_range(path_offset, path_length + 1, strings_offset + strings_size, "path")
        _checked_range(etag_offset, etag_length + 1, strings_offset + strings_size, "ETag")
        if path_offset < strings_offset or etag_offset < strings_offset:
            raise PackError("manifest string points outside the string table")
        if pack[path_offset + path_length] or pack[etag_offset + etag_length]:
            raise PackError("manifest string is not NUL terminated")
        url = pack[path_offset : path_offset + path_length]
        etag = pack[etag_offset : etag_offset + etag_length]
        if not url.startswith(b"/static/") or (previous_url is not None and url <= previous_url):
            raise PackError("manifest URLs are invalid or not strictly sorted")
        previous_url = url
        _checked_range(asset_offset, stored_size, pack_size, "asset")
        if asset_offset < data_offset or reserved:
            raise PackError("invalid asset entry")
        if not (0 < mime_id < 18 and encoding_id < 2 and cache_id < 3):
            raise PackError("invalid response metadata enum")
        content = pack[asset_offset : asset_offset + stored_size]
        if binascii.crc32(content) & 0xFFFFFFFF != content_crc:
            raise PackError("asset CRC32 mismatch")
        parsed.append(
            {
                "url": url.decode("utf-8"),
                "etag": etag.decode("ascii"),
                "offset": asset_offset,
                "size": stored_size,
                "sourceSize": source_size,
                "mimeId": mime_id,
                "encodingId": encoding_id,
                "cacheId": cache_id,
            }
        )
    return parsed


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".tmp-{os.getpid()}")
    try:
        temporary.write_bytes(data)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def parse_size(value: str) -> int:
    try:
        parsed = int(value, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid integer size: {value}") from error
    if parsed <= 0:
        raise argparse.ArgumentTypeError("size must be positive")
    return parsed


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="Vite output directory")
    parser.add_argument("--output", type=Path, required=True, help="raw partition image")
    parser.add_argument(
        "--manifest", type=Path, required=True, help="human-readable JSON sidecar"
    )
    parser.add_argument(
        "--max-size", type=parse_size, required=True, help="partition size (decimal or 0x...)"
    )
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    try:
        pack, manifest = build_pack(args.input.resolve(), args.max_size)
        manifest_bytes = (
            json.dumps(manifest, indent=2, sort_keys=False, ensure_ascii=False) + "\n"
        ).encode("utf-8")
        atomic_write(args.output, pack)
        atomic_write(args.manifest, manifest_bytes)
    except (OSError, PackError) as error:
        print(f"web app pack failed: {error}", file=sys.stderr)
        return 2
    print(
        f"Packed {len(manifest['entries'])} URLs / "
        f"{manifest['uniquePayloadCount']} unique payloads into {len(pack)} bytes; "
        f"deduplicated {manifest['deduplicatedBytes']} payload bytes "
        f"({args.output})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

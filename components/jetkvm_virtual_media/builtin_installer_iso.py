#!/usr/bin/env python3
"""Build the installer disc that the firmware carries.

The disc is an ISO 9660 image with a Joliet tree, volume label JETKVM, and
one file, "Install OS Services.html": a page that redirects to the OS
Services download for this firmware version, with a link in case the browser
does not follow the redirect. Most of the image is zero, so the
firmware stores only its non-zero byte runs (--c-output) and fills the rest
with zeros when the host reads it (builtin_image.c). --iso-output writes the
whole image, which the host tests compare with the runs.
"""

from __future__ import annotations

import argparse
import html
import struct
import sys
import urllib.parse
from pathlib import Path

SECTOR = 2048
VOLUME_LABEL = "JETKVM"
PAGE_NAME = "Install OS Services.html"
PAGE_ISO_NAME = "INSTALL.HTM;1"
INSTALL_URL = "https://jetkvm.com/os-services/install"
# Zero sectors after the file, as genisoimage -pad adds, so that a host's
# read-ahead past the file stays inside the volume. Zeros cost no flash.
PAD_SECTORS = 150
# A run header is 8 bytes, so a gap of zeros up to that size is cheaper to
# store than a new run.
RUN_HEADER_SIZE = 8
# Fixed, so that the same version always gives the same image.
RECORD_DATE = bytes((126, 1, 1, 0, 0, 0, 0))  # 2026-01-01 00:00:00 UTC
VOLUME_DATE = b"2026010100000000\x00"
NO_DATE = b"0000000000000000\x00"

SYSTEM_AREA_SECTORS = 16
PVD_SECTOR = 16
SVD_SECTOR = 17
TERMINATOR_SECTOR = 18
ISO_L_PATH_SECTOR = 19
ISO_M_PATH_SECTOR = 20
JOLIET_L_PATH_SECTOR = 21
JOLIET_M_PATH_SECTOR = 22
ISO_ROOT_SECTOR = 23
JOLIET_ROOT_SECTOR = 24
FILE_SECTOR = 25
PATH_TABLE_SIZE = 10


def install_url(version: str) -> str:
    return f"{INSTALL_URL}?mini={urllib.parse.quote(version, safe='')}"


def page(version: str) -> bytes:
    url = html.escape(install_url(version), quote=True)
    return (
        "<!doctype html>\n"
        '<html lang="en">\n'
        "<head>\n"
        '<meta charset="utf-8">\n'
        f'<meta http-equiv="refresh" content="0; url={url}">\n'
        "<title>Install OS Services</title>\n"
        "</head>\n"
        "<body>\n"
        f'<p><a href="{url}">Install OS Services</a></p>\n'
        "</body>\n"
        "</html>\n"
    ).encode("utf-8")


def both16(value: int) -> bytes:
    return struct.pack("<H", value) + struct.pack(">H", value)


def both32(value: int) -> bytes:
    return struct.pack("<I", value) + struct.pack(">I", value)


def text(value: str, size: int, joliet: bool) -> bytes:
    """An identifier field padded with spaces, in UCS-2 for Joliet."""
    if joliet:
        encoded = value.encode("utf-16-be")
        pad = " ".encode("utf-16-be")
    else:
        encoded = value.encode("ascii")
        pad = b" "
    if len(encoded) > size:
        raise ValueError(f"{value!r} does not fit {size} bytes")
    encoded += pad * ((size - len(encoded)) // len(pad))
    return encoded + b" " * (size - len(encoded))


def directory_record(identifier: bytes, extent: int, length: int, directory: bool) -> bytes:
    record = (
        bytes((0, 0))
        + both32(extent)
        + both32(length)
        + RECORD_DATE
        + bytes((2 if directory else 0, 0, 0))
        + both16(1)
        + bytes((len(identifier),))
        + identifier
    )
    if len(identifier) % 2 == 0:
        record += b"\x00"
    return bytes((len(record),)) + record[1:]


def directory(root: int, name: bytes, file_size: int) -> bytes:
    records = (
        directory_record(b"\x00", root, SECTOR, True)
        + directory_record(b"\x01", root, SECTOR, True)
        + directory_record(name, FILE_SECTOR, file_size, False)
    )
    return records.ljust(SECTOR, b"\x00")


def path_table(root: int, big_endian: bool) -> bytes:
    order = ">" if big_endian else "<"
    return bytes((1, 0)) + struct.pack(f"{order}IH", root, 1) + b"\x00\x00"


def volume_descriptor(joliet: bool, volume_sectors: int) -> bytes:
    root = JOLIET_ROOT_SECTOR if joliet else ISO_ROOT_SECTOR
    l_path = JOLIET_L_PATH_SECTOR if joliet else ISO_L_PATH_SECTOR
    m_path = JOLIET_M_PATH_SECTOR if joliet else ISO_M_PATH_SECTOR
    descriptor = (
        bytes((2 if joliet else 1,))
        + b"CD001\x01\x00"
        + text("", 32, joliet)
        + text(VOLUME_LABEL, 32, joliet)
        + bytes(8)
        + both32(volume_sectors)
        # The escape sequence of UCS-2 level 3 names the Joliet tree.
        + (b"%/E".ljust(32, b"\x00") if joliet else bytes(32))
        + both16(1)
        + both16(1)
        + both16(SECTOR)
        + both32(PATH_TABLE_SIZE)
        + struct.pack("<I", l_path)
        + bytes(4)
        + struct.pack(">I", m_path)
        + bytes(4)
        + directory_record(b"\x00", root, SECTOR, True)
        + text("", 128, joliet)
        + text("", 128, joliet)
        + text("", 128, joliet)
        + text("", 128, joliet)
        + text("", 37, joliet)
        + text("", 37, joliet)
        + text("", 37, joliet)
        + VOLUME_DATE
        + VOLUME_DATE
        + NO_DATE
        + VOLUME_DATE
        + b"\x01\x00"
    )
    assert len(descriptor) == 883, len(descriptor)
    return descriptor.ljust(SECTOR, b"\x00")


def image(version: str) -> bytes:
    content = page(version)
    file_sectors = (len(content) + SECTOR - 1) // SECTOR
    volume_sectors = FILE_SECTOR + file_sectors + PAD_SECTORS
    terminator = (b"\xffCD001\x01").ljust(SECTOR, b"\x00")
    sectors = {
        PVD_SECTOR: volume_descriptor(False, volume_sectors),
        SVD_SECTOR: volume_descriptor(True, volume_sectors),
        TERMINATOR_SECTOR: terminator,
        ISO_L_PATH_SECTOR: path_table(ISO_ROOT_SECTOR, False),
        ISO_M_PATH_SECTOR: path_table(ISO_ROOT_SECTOR, True),
        JOLIET_L_PATH_SECTOR: path_table(JOLIET_ROOT_SECTOR, False),
        JOLIET_M_PATH_SECTOR: path_table(JOLIET_ROOT_SECTOR, True),
        ISO_ROOT_SECTOR: directory(
            ISO_ROOT_SECTOR, PAGE_ISO_NAME.encode("ascii"), len(content)
        ),
        JOLIET_ROOT_SECTOR: directory(
            JOLIET_ROOT_SECTOR, PAGE_NAME.encode("utf-16-be"), len(content)
        ),
    }
    data = bytearray(volume_sectors * SECTOR)
    for sector, payload in sectors.items():
        data[sector * SECTOR : sector * SECTOR + len(payload)] = payload
    data[FILE_SECTOR * SECTOR : FILE_SECTOR * SECTOR + len(content)] = content
    return bytes(data)


def runs(data: bytes) -> list[tuple[int, int]]:
    """The (offset, length) runs that hold every non-zero byte of `data`."""
    found: list[list[int]] = []
    for offset, byte in enumerate(data):
        if byte == 0:
            continue
        if found and offset - (found[-1][0] + found[-1][1]) <= RUN_HEADER_SIZE:
            found[-1][1] = offset + 1 - found[-1][0]
        else:
            found.append([offset, 1])
    return [(offset, length) for offset, length in found]


def c_source(version: str, data: bytes) -> str:
    sparse = runs(data)
    blob = b"".join(data[offset : offset + length] for offset, length in sparse)
    lines = [
        f"/* Generated by builtin_installer_iso.py for {version}; do not edit. */",
        '#include "builtin_image.h"',
        "",
        "static const jetkvm_builtin_image_run_t s_runs[] = {",
    ]
    lines += [f"    {{ {offset}U, {length}U }}," for offset, length in sparse]
    lines += ["};", "", "static const uint8_t s_data[] = {"]
    for start in range(0, len(blob), 16):
        chunk = blob[start : start + 16]
        lines.append("    " + " ".join(f"0x{byte:02x}," for byte in chunk))
    lines += [
        "};",
        "",
        "const jetkvm_builtin_image_t jetkvm_builtin_installer_image = {",
        f"    .size = {len(data)}U,",
        f"    .run_count = {len(sparse)}U,",
        "    .runs = s_runs,",
        "    .data = s_data,",
        "};",
        "",
    ]
    return "\n".join(lines)


def write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", required=True, help="the firmware version")
    parser.add_argument("--c-output", type=Path, help="C source with the non-zero runs")
    parser.add_argument("--iso-output", type=Path, help="the whole image")
    args = parser.parse_args(argv)
    if not args.version or not args.c_output and not args.iso_output:
        parser.error("give a version and at least one output")
    data = image(args.version)
    if args.c_output:
        write(args.c_output, c_source(args.version, data).encode("ascii"))
    if args.iso_output:
        write(args.iso_output, data)
    return 0


if __name__ == "__main__":
    sys.exit(main())

"""The built-in installer disc: the generator's image, read back through the
firmware's sparse read path, checked with xorriso and isoinfo, and the
firmware's contracts for the BUILTIN source."""

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

COMPONENT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(COMPONENT))
import builtin_installer_iso  # noqa: E402

VERSION = "0.6.1-rc.2+codelog"
URL = "https://jetkvm.com/os-services/install?mini=0.6.1-rc.2%2Bcodelog"
# The runs and their headers in flash; the image itself is about 350 KiB.
FLASH_LIMIT = 4096


def run(*command: str, **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(command, check=True, capture_output=True, **kwargs)


def tool(name: str) -> str | None:
    found = shutil.which(name)
    if found is None and os.environ.get("CI"):
        raise AssertionError(f"{name} is needed in CI")
    if found is None:
        print(f"note: {name} not found; its checks are skipped")
    return found


def source_body(source: str, start: str) -> str:
    begin = source.index(start)
    end = source.index("\n}\n", begin)
    return source[begin:end]


with tempfile.TemporaryDirectory() as scratch:
    work = Path(scratch)
    iso = work / "installer.iso"
    generated = work / "builtin_installer_image.c"
    run(
        sys.executable,
        str(COMPONENT / "builtin_installer_iso.py"),
        "--version",
        VERSION,
        "--c-output",
        str(generated),
        "--iso-output",
        str(iso),
    )
    image = iso.read_bytes()
    assert image == builtin_installer_iso.image(VERSION)
    assert len(image) % builtin_installer_iso.SECTOR == 0

    # The firmware's read path gives back the whole image from the runs.
    dump = work / "dump"
    run(
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        f"-I{COMPONENT}",
        str(COMPONENT / "builtin_image.c"),
        str(generated),
        str(COMPONENT / "tests/dump_builtin_image.c"),
        "-o", str(dump),
    )
    assert run(str(dump)).stdout == image, "the runs do not rebuild the image"

    sparse = builtin_installer_iso.runs(image)
    stored = sum(length for _, length in sparse)
    flash = stored + 8 * len(sparse)
    assert flash <= FLASH_LIMIT, f"{flash} bytes of flash"
    for (offset, length), (next_offset, _) in zip(sparse, sparse[1:]):
        assert offset + length < next_offset
    covered = bytearray(len(image))
    for offset, length in sparse:
        covered[offset : offset + length] = image[offset : offset + length]
    assert bytes(covered) == image, "a non-zero byte is outside the runs"
    print(f"image {len(image)} bytes, flash {flash} bytes in {len(sparse)} runs")

    page = builtin_installer_iso.page(VERSION).decode("utf-8")
    assert page.count(URL) == 2
    assert re.findall(r'href="([^"]+)"', page) == [URL]
    assert re.findall(r'http-equiv="refresh" content="0; url=([^"]+)"', page) == [URL]
    assert "src=" not in page and "url(" not in page and "@import" not in page
    assert len(re.findall(r"https?://", page)) == 2
    assert "<title>Install OS Services</title>" in page

    isoinfo = tool("isoinfo")
    if isoinfo:
        info = run(isoinfo, "-d", "-i", str(iso), text=True).stdout
        assert "Volume id: JETKVM\n" in info, info
        assert "Joliet with UCS level 3 found" in info, info
        joliet = run(isoinfo, "-J", "-f", "-i", str(iso), text=True).stdout
        assert joliet.splitlines() == ["/Install OS Services.html"], joliet
        primary = run(isoinfo, "-f", "-i", str(iso), text=True).stdout
        assert primary.splitlines() == ["/INSTALL.HTM;1"], primary
        extracted = run(
            isoinfo, "-J", "-i", str(iso), "-x", "/Install OS Services.html"
        ).stdout
        assert extracted.decode("utf-8") == page

    xorriso = tool("xorriso")
    if xorriso:
        pvd = run(xorriso, "-indev", str(iso), "-pvd_info", text=True).stdout
        assert re.search(r"^Volume Id\s*: JETKVM$", pvd, re.M), pvd
        out = work / "extracted"
        run(
            xorriso, "-osirrox", "on", "-joliet", "on", "-indev", str(iso),
            "-extract", "/", str(out),
        )
        assert sorted(path.name for path in out.iterdir()) == [
            "Install OS Services.html"
        ]
        assert (out / "Install OS Services.html").read_text("utf-8") == page

SOURCE = (COMPONENT / "jetkvm_virtual_media.c").read_text(encoding="utf-8")
mount = source_body(SOURCE, "esp_err_t jetkvm_virtual_media_mount_builtin_installer(")
assert "s_ctx.image_open" in mount and "ESP_ERR_INVALID_STATE" in mount
assert "s_ctx.mode = JETKVM_VIRTUAL_MEDIA_MODE_CDROM;" in mount
assert "s_ctx.image_writable = false;" in mount
assert "s_ctx.source = JETKVM_VIRTUAL_MEDIA_SOURCE_BUILTIN;" in mount
read = source_body(SOURCE, "esp_err_t jetkvm_virtual_media_read(")
assert "jetkvm_builtin_image_read(&jetkvm_builtin_installer_image" in read
# The card's removal ends only an image that is on the card.
removal = source_body(SOURCE, "static void remove_storage_locked(")
assert "s_ctx.source == JETKVM_VIRTUAL_MEDIA_SOURCE_STORAGE" in removal
# The disc is never written and never reads the card's scratch.
writable = source_body(SOURCE, "static bool storage_image_writable_locked(")
assert "s_ctx.source == JETKVM_VIRTUAL_MEDIA_SOURCE_STORAGE" in writable

print("built-in installer disc: OK")

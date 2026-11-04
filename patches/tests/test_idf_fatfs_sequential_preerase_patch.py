#!/usr/bin/env python3
"""Static and decision-model contract for optional sequential SD ERASE."""

from pathlib import Path
import os
import subprocess


ROOT = Path(__file__).resolve().parents[2]
PATCH_NAME = "esp-idf-6.0.2-fatfs-sequential-preerase.patch"
PATCH_PATH = ROOT / "patches" / PATCH_NAME
PATCH = PATCH_PATH.read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
VM = (
    ROOT / "components/jetkvm_virtual_media/jetkvm_virtual_media.c"
).read_text(encoding="utf-8")


registration = f"apply_idf_patch(patches/{PATCH_NAME})"
assert registration in CMAKE
assert CMAKE.index(registration) < CMAKE.index(
    "include($ENV{IDF_PATH}/tools/cmake/project.cmake)"
)
assert 'set(JETKVM_FATFS_SEQ_PREERASE "0" CACHE STRING' in CMAKE
assert "JETKVM_FATFS_SEQ_PREERASE=${JETKVM_FATFS_SEQ_PREERASE}" in CMAKE

new_file_gate = "if (opened && existing == 0U) {"
assert VM.count("f_expand(&s_ctx.upload, (FSIZE_t)size, 0)") == 1
assert new_file_gate in VM
assert VM.index(new_file_gate) < VM.index(
    "f_expand(&s_ctx.upload, (FSIZE_t)size, 0)"
)

# Only the non-allocating probe of a genuinely empty exFAT file may request
# the operation. The normal upload caller invokes that probe only for a new
# `.incomplete` file; an existing resume therefore cannot reach this hook.
for contract in (
    "if (!opt && fp->obj.objsize == 0 && fs->fs_type == FS_EXFAT",
    "CTRL_JETKVM_SD_SEQ_PREERASE",
    "clst2sect(fs, scl)",
    ".sector_count = (LBA_t)tcl * fs->csize",
    "(void)disk_ioctl",
):
    assert contract in PATCH

hook_start = PATCH.index("if (!opt && fp->obj.objsize == 0")
hook_end = PATCH.index("#endif", hook_start)
hook = PATCH[hook_start:hook_end]
assert "change_bitmap" not in hook
assert "fp->obj.objsize = fsz" not in hook
assert "fp->obj.sclust = scl" not in hook

# The native driver is deliberately narrower than sdmmc_erase_sectors itself.
# In particular, SDSPI, MMC, SDSC/CSD-v1 and cards without CCC_ERASE are out.
for contract in (
    "card->is_mem && !card->is_mmc",
    "!(card->host.flags & SDMMC_HOST_FLAG_SPI)",
    "card->ocr & SD_OCR_SDHC_CAP",
    "card->csd.csd_ver == SD_CSD_CSDVER_2_0",
    "card->csd.card_command_class & SD_CSD_CCC_ERASE",
    "card->csd.sector_size == 512",
    "card->ssr.alloc_unit_kb != 0",
    "card->ssr.erase_size_au != 0",
    "sdmmc_erase_sectors(card",
    "SDMMC_ERASE_ARG",
):
    assert contract in PATCH

# It records both requested and inward-aligned actual ranges, command latency,
# and the ESP result. Failures return through diskio but f_expand ignores them.
for field in (
    "request_lba=%",
    "request_count=%",
    '" lba=%"',
    '" count=%"',
    '" us=%"',
    '" result=0x%x"',
):
    assert field in PATCH
assert "request_count <= capacity - request_lba" in PATCH
assert "erase_end > erase_lba" in PATCH


def aligned_inner_extent(start: int, count: int, erase_unit: int):
    """Executable model of the driver's no-overreach alignment."""
    if count <= 0 or erase_unit <= 0:
        return None
    end = start + count
    aligned_start = ((start + erase_unit - 1) // erase_unit) * erase_unit
    aligned_end = (end // erase_unit) * erase_unit
    if aligned_end <= aligned_start:
        return None
    return aligned_start, aligned_end - aligned_start


unit = 32 * 1024 * 1024 // 512
extent = aligned_inner_extent(1234, 128 * 1024 * 1024 // 512, unit)
assert extent is not None
lba, count = extent
assert lba >= 1234
assert lba + count <= 1234 + 128 * 1024 * 1024 // 512
assert lba % unit == 0 and count % unit == 0
assert aligned_inner_extent(1234, 8 * 1024 * 1024 // 512, unit) is None


idf_path = Path(
    os.environ.get("IDF_PATH", Path.home() / ".espressif/v6.0.2/esp-idf")
)
if idf_path.exists():
    directions = []
    for extra in ([], ["--reverse"]):
        directions.append(
            subprocess.run(
                ["git", "apply", *extra, "--check", str(PATCH_PATH)],
                cwd=idf_path,
                check=False,
                capture_output=True,
            ).returncode
            == 0
        )
    assert directions.count(True) == 1, (
        "sequential pre-erase patch must apply cleanly in exactly one direction"
    )

print("ESP-IDF FatFs sequential pre-erase patch contract: PASS")

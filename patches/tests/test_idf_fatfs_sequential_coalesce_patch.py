#!/usr/bin/env python3
"""Static and state-model contract for append-only exFAT write coalescing."""

from pathlib import Path
import os
import subprocess


ROOT = Path(__file__).resolve().parents[2]
PATCH_NAME = "esp-idf-6.0.2-fatfs-sequential-coalesce.patch"
PATCH_PATH = ROOT / "patches" / PATCH_NAME
PATCH = PATCH_PATH.read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")


registration = f"apply_idf_patch(patches/{PATCH_NAME})"
assert registration in CMAKE
assert CMAKE.index(registration) < CMAKE.index(
    "include($ENV{IDF_PATH}/tools/cmake/project.cmake)"
)
assert 'set(JETKVM_FATFS_SEQ_WRITE_COALESCE "0" CACHE STRING' in CMAKE
assert (
    "JETKVM_FATFS_SEQ_WRITE_COALESCE=${JETKVM_FATFS_SEQ_WRITE_COALESCE}"
    in CMAKE
)

# The fast path is exFAT-only, contiguous-chain-only and strictly append-only.
for contract in (
    "fs->fs_type == FS_EXFAT",
    "fp->obj.stat == 2",
    "fp->fptr == fp->obj.objsize",
    "fp->cltbl == 0",
    "JETKVM_FATFS_SEQ_WRITE_MAX_BYTES (256U * 1024U)",
):
    assert contract in PATCH

# Reserve only the exact physical successor run. A later free run is not safe.
assert "DWORD first = fp->clust + 1" in PATCH
assert "DWORD found = find_bitmap(fs, first, extra)" in PATCH
assert "if (found == first)" in PATCH
assert "change_bitmap(fs, first, extra, 1)" in PATCH

# Logical size is deliberately untouched until FatFs's original successful
# loop increment. The feature remains production-disabled because exFAT bitmap
# updates and their compensating rollback are not an atomic transaction across
# a media error; the checks below qualify only the successful fast path and the
# presence of a best-effort rollback attempt.
assert "fp->obj.objsize =" not in PATCH
reserve_at = PATCH.index("change_bitmap(fs, first, extra, 1)")
write_at = PATCH.index("disk_write(fs->pdrv, wbuff, sect, cc)")
rollback_at = PATCH.index("seq_first_extra, seq_extra, 0")
advance_at = PATCH.index("fp->clust = seq_end_clust")
assert reserve_at < write_at < rollback_at < advance_at


def successful_transaction(cluster_sectors: int, sectors: int, contiguous: bool):
    """Small executable model of one successful sector-aligned append."""
    sectors = min(sectors, 256 * 1024 // 512)
    extra = (sectors - 1) // cluster_sectors
    reserved = extra if contiguous else 0
    issued = sectors if reserved == extra else min(sectors, cluster_sectors)
    return issued, issued


# This card has 64-sector clusters. The normal 64 KiB append can become one
# 128-sector command when the explicitly experimental switch is enabled.
assert successful_transaction(64, 128, True) == (128, 128)
# Fragmentation falls back to the stock one-cluster command.
assert successful_transaction(64, 128, False) == (64, 64)
# Generic huge callers are bounded to a 256 KiB command.
assert successful_transaction(64, 4096, True)[0] == 512


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
        "sequential coalescing patch must apply cleanly in exactly one direction"
    )

print("ESP-IDF FatFs sequential coalescing patch contract: PASS")

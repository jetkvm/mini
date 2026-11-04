#!/usr/bin/env python3
"""Lock the complete retry contract for ESP-IDF FatFs ``f_close``.

Keeping the dynamically allocated FIL buffer is necessary but not sufficient:
``f_sync`` must also retain ``FA_MODIFIED`` when the final filesystem sync
fails, otherwise a second ``f_close`` skips the failed operation and falsely
invalidates the file object as if it had been durably closed.
"""

import os
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
PATCH_NAME = "esp-idf-6.0.2-fatfs-close-retry.patch"
PATCH = (ROOT / "patches" / PATCH_NAME).read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

assert f"apply_idf_patch(patches/{PATCH_NAME})" in CMAKE
apply_at = CMAKE.index(f"apply_idf_patch(patches/{PATCH_NAME})")
assert CMAKE.index(
    "apply_idf_patch(patches/esp-idf-6.0.2-fatfs-exfat-config.patch)"
) < apply_at
assert apply_at < CMAKE.index("include($ENV{IDF_PATH}/tools/cmake/project.cmake)")
assert "components/fatfs/src/ff.c" in PATCH

# Both the exFAT xdir path and the FAT directory-entry path must leave the file
# modified when sync_fs fails. This makes the next f_close call enter f_sync
# again instead of returning a false success without retrying CTRL_SYNC/window
# writeback.
retry_blocks = re.findall(
    r"res = sync_fs\(fs\);[^\n]*\n"
    r"-\s*fp->flag &= \(BYTE\)~FA_MODIFIED;\n"
    r"\+\s*if \(res == FR_OK\) \{\n"
    r"\+\s*fp->flag &= \(BYTE\)~FA_MODIFIED;\n"
    r"\+\s*\}",
    PATCH,
)
assert len(retry_blocks) == 2, (
    "close retry must guard FA_MODIFIED clearing after sync_fs in both "
    "exFAT and FAT paths"
)

# A failed close must retain the per-file sector buffer as well as the modified
# bit and valid FIL. On success, normal invalidation/free behavior is unchanged.
assert "if (res == FR_OK && fp->buf)" in PATCH
assert "failed f_sync leaves the FIL valid" in PATCH
assert PATCH.count("@@ -") == 3


def close_attempt(state: dict[str, bool | int], sync_ok: bool) -> bool:
    """Executable model of the two patched state transitions."""

    if state["modified"]:
        state["sync_calls"] = int(state["sync_calls"]) + 1
        if sync_ok:
            state["modified"] = False
    if not sync_ok:
        return False
    state["valid"] = False
    state["buffer"] = False
    return True


state: dict[str, bool | int] = {
    "modified": True,
    "valid": True,
    "buffer": True,
    "sync_calls": 0,
}
assert not close_attempt(state, sync_ok=False)
assert state == {
    "modified": True,
    "valid": True,
    "buffer": True,
    "sync_calls": 1,
}
assert close_attempt(state, sync_ok=True)
assert state == {
    "modified": False,
    "valid": False,
    "buffer": False,
    "sync_calls": 2,
}


# When an ESP-IDF 6.0.2 checkout is available, require the complete patch to be
# in exactly one valid state: cleanly forward-applicable or already applied.
idf_path_text = os.environ.get("IDF_PATH")
idf_path = (
    Path(idf_path_text)
    if idf_path_text
    else Path.home() / ".espressif" / "v6.0.2" / "esp-idf"
)
if (idf_path / "components/fatfs/src/ff.c").is_file():
    patch_path = ROOT / "patches" / PATCH_NAME

    def applies(*extra: str) -> bool:
        completed = subprocess.run(
            ["git", "apply", *extra, "--check", str(patch_path)],
            cwd=idf_path,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        return completed.returncode == 0

    forward = applies()
    reverse = applies("--reverse")
    assert forward != reverse, (
        "complete FatFs retry patch must either apply cleanly or already be "
        "applied to the selected ESP-IDF 6.0.2 tree"
    )

print("ESP-IDF FatFs complete retryable close patch contract: PASS")

#!/usr/bin/env python3
"""Lock the ESP-IDF SDMMC TX cache-direction correction."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PATCH_NAME = "esp-idf-6.0.2-sdmmc-tx-cache-sync.patch"
PATCH = (ROOT / "patches" / PATCH_NAME).read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

assert f"apply_idf_patch(patches/{PATCH_NAME})" in CMAKE
assert CMAKE.index(f"apply_idf_patch(patches/{PATCH_NAME})") < CMAKE.index(
    "include($ENV{IDF_PATH}/tools/cmake/project.cmake)"
)

# The patch must not remove or alter the mandatory pre-DMA writeback.
assert "-        ret = esp_cache_msync" not in PATCH
assert "-        cache_sync_len = cmdinfo->buflen" not in PATCH
# Only DMA reads modify the caller's buffer and need a post-DMA invalidate.
assert (
    "+    if (cmdinfo->data && (cmdinfo->flags & SCF_CMD_READ)) {" in PATCH
)
assert "ESP_CACHE_MSYNC_FLAG_DIR_M2C" in PATCH
assert PATCH.count("SCF_CMD_READ") == 1

print("ESP-IDF SDMMC TX cache-sync patch contract: PASS")

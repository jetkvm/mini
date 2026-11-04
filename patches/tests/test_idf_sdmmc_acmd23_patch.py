#!/usr/bin/env python3
"""Static contract for the opt-in SD ACMD23 pre-erase."""

from pathlib import Path
import os
import subprocess


ROOT = Path(__file__).resolve().parents[2]
PATCH_NAME = "esp-idf-6.0.2-sdmmc-acmd23-preerase.patch"
PATCH_PATH = ROOT / "patches" / PATCH_NAME
PATCH = PATCH_PATH.read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
VM_CMAKE = (ROOT / "components/jetkvm_virtual_media/CMakeLists.txt").read_text(
    encoding="utf-8"
)
VM_SOURCE = (ROOT / "components/jetkvm_virtual_media/jetkvm_virtual_media.c").read_text(
    encoding="utf-8"
)
VM_HEADER = (
    ROOT / "components/jetkvm_virtual_media/include/jetkvm_virtual_media.h"
).read_text(encoding="utf-8")
HTTP_STATS = (ROOT / "components/webrtc_transport/webrtc_stats_json.c").read_text(
    encoding="utf-8"
)


registration = f"apply_idf_patch(patches/{PATCH_NAME})"
assert registration in CMAKE
assert CMAKE.index(registration) < CMAKE.index(
    "include($ENV{IDF_PATH}/tools/cmake/project.cmake)"
)
assert 'set(JETKVM_TF_SD_ACMD23 "0" CACHE STRING' in CMAKE
assert "if(JETKVM_TF_SD_ACMD23)" in CMAKE
assert 'set(JETKVM_TF_SD_ACMD23 "0" CACHE STRING' in VM_CMAKE
assert "JETKVM_TF_SD_ACMD23=${JETKVM_TF_SD_ACMD23}" in VM_CMAKE
assert "#if JETKVM_TF_SD_ACMD23" in VM_SOURCE
assert "sdmmc_acmd23_set_enabled(true)" in VM_SOURCE

# The command is exactly SD ACMD23 (CMD55 + command 23), not MMC CMD23.
assert "SD_APP_SET_WR_BLK_ERASE_COUNT   23" in PATCH
assert ".opcode = SD_APP_SET_WR_BLK_ERASE_COUNT" in PATCH
assert ".flags = SCF_CMD_AC | SCF_RSP_R1" in PATCH
assert ".arg = (uint32_t)block_count" in PATCH

# Eligibility is deliberately narrow and auditable at the CMD25 call site.
assert "if (block_count > 1 && !card->is_mmc && !host_is_spi(card))" in PATCH

# Only a positive unsupported result may fall back. CRC, timeout and every
# other transport/protocol error are counted and returned to f_write().
assert "if (err == ESP_ERR_NOT_SUPPORTED)" in PATCH
assert "s_acmd23_unsupported_latched" in PATCH
assert "s_acmd23_failures" in PATCH
assert 'ESP_LOGE(TAG, "SD ACMD23 failed' in PATCH
assert "return err;" in PATCH
assert "err == ESP_ERR_TIMEOUT" not in PATCH
assert "err == ESP_ERR_INVALID_CRC" not in PATCH

# Runtime counters must be inspectable from the normal device stats snapshot.
assert "sdmmc_acmd23_get_stats" in PATCH
assert "jetkvm_virtual_media_get_sd_write_hint_stats" in VM_HEADER
for field in (
    '"sdWriteHint"',
    '"enabled"',
    '"unsupportedLatched"',
    '"attempts"',
    '"successes"',
    '"unsupported"',
    '"failures"',
    '"lastBlockCount"',
    '"lastError"',
):
    assert field in HTTP_STATS

# A local locked IDF checkout must recognize the patch in exactly one direction.
idf_path = Path(
    os.environ.get("IDF_PATH", Path.home() / ".espressif/v6.0.2/esp-idf")
)
if idf_path.exists():
    forward = subprocess.run(
        ["git", "apply", "--check", str(PATCH_PATH)],
        cwd=idf_path,
        check=False,
        capture_output=True,
    ).returncode == 0
    reverse = subprocess.run(
        ["git", "apply", "--reverse", "--check", str(PATCH_PATH)],
        cwd=idf_path,
        check=False,
        capture_output=True,
    ).returncode == 0
    assert forward != reverse, (
        "ACMD23 patch must either apply cleanly or already be fully applied"
    )

print("ESP-IDF SD ACMD23 pre-erase patch contract: PASS")

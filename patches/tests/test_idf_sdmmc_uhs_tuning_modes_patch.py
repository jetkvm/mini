#!/usr/bin/env python3
"""Lock SD UHS-I tuning to the modes for which CMD19 is defined."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PATCH_NAME = "esp-idf-6.0.2-sdmmc-uhs-tuning-modes.patch"
PATCH = (ROOT / "patches" / PATCH_NAME).read_text(encoding="utf-8")
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

registration = f"apply_idf_patch(patches/{PATCH_NAME})"
assert registration in CMAKE
assert CMAKE.index(registration) < CMAKE.index(
    "include($ENV{IDF_PATH}/tools/cmake/project.cmake)"
)

assert "card->max_freq_khz != SDMMC_FREQ_SDR50" in PATCH
assert "card->max_freq_khz != SDMMC_FREQ_SDR104" in PATCH
assert "+        return ESP_OK;" in PATCH
assert "+    if (card->max_freq_khz == SDMMC_FREQ_SDR104)" in PATCH
assert "card->host.max_freq_khz == SDMMC_FREQ_SDR104" in PATCH
assert PATCH.count("sdmmc_do_timing_tuning") == 2

print("ESP-IDF SDMMC UHS-I tuning-mode patch contract: PASS")

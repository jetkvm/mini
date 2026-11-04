#!/usr/bin/env python3
"""Static contract for the ESP-IDF SPI private-RX OOM crash fix."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / "patches" / "esp-idf-6.0.2-spi-private-rx-oom.patch"
CMAKE = ROOT / "CMakeLists.txt"


patch_text = PATCH.read_text(encoding="utf-8")
cmake_text = CMAKE.read_text(encoding="utf-8")

assert "components/esp_driver_spi/src/gpspi/spi_master.c" in patch_text
assert "trans_buf->buffer_to_rcv != NULL" in patch_text
assert "memcpy(orig_rx_buffer, trans_buf->buffer_to_rcv" in patch_text
assert "apply_idf_patch(patches/esp-idf-6.0.2-spi-private-rx-oom.patch)" in cmake_text

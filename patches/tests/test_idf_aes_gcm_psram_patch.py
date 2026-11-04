#!/usr/bin/env python3
"""Static contract for hardware AES-GCM on TLS and DTLS records in PSRAM.

The gate accepts DMA-capable PSRAM buffers except with flash encryption or
PSRAM ECC, whose alignment rule the GCM DMA path does not apply. The second
patch frees what the GCM DMA path allocated when a later allocation fails.
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GATE = "esp-idf-6.0.2-aes-gcm-psram-dma.patch"
CLEANUP = "esp-idf-6.0.2-aes-dma-alloc-failure-cleanup.patch"
GCM_C = "components/mbedtls/port/aes/esp_aes_gcm.c"
DMA_C = "components/mbedtls/port/aes/dma/esp_aes_dma_core.c"
cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")

assert "CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y" in defaults
for name in (GATE, CLEANUP):
    assert cmake.index(f"apply_idf_patch(patches/{name})") < cmake.index(
        "include($ENV{IDF_PATH}/tools/cmake/project.cmake)")


def function(source, signature, end):
    start = source.index(signature)
    return source[start:source.index(end, start)]


def check_gate(gcm):
    helper = function(gcm, "static bool esp_aes_gcm_dma_capable(", "\n}\n")
    assert ("#if SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE && CONFIG_SPIRAM && SOC_PSRAM_DMA_CAPABLE"
            " && !CONFIG_SPIRAM_ECC_ENABLE\n") in helper
    assert "esp_ptr_dma_ext_capable(p) && !efuse_hal_flash_encryption_enabled()" in helper
    assert helper.rstrip().endswith("return esp_ptr_dma_capable(p);")
    gate = function(gcm, "static bool esp_aes_gcm_input_support_hw_accel(", "\n}\n")
    assert "esp_ptr_dma_capable(" not in gate
    for operand in ("aad) && aad_len > 0", "input) && length > 0", "output) && length > 0"):
        assert f"!esp_aes_gcm_dma_capable({operand}" in gate, operand


def check_cleanup(dma):
    # The cache-synced variant (ESP32-P4) is the first definition of each.
    build = function(dma, "static esp_err_t generate_descriptor_list(", "\nint esp_aes_process_dma(")
    after_alloc = build[build.index("size_t populated_dma_descs"):]  # dma_descriptors is allocated
    body, fail = after_alloc.split("\nfail:\n")
    assert "return ESP_FAIL;" not in body
    assert body.count("goto fail;") == 3
    for buffer in ("start_alignment_stream_buffer", "end_alignment_stream_buffer", "dma_descriptors"):
        assert f"free({buffer});" in fail, buffer
    gcm = function(dma, "int esp_aes_process_dma_gcm(", "\n#endif //CONFIG_MBEDTLS_HARDWARE_GCM")
    allocating = gcm[gcm.index("generate_descriptor_list(aad"):gcm.index("\ncleanup:\n")]
    assert "return" not in allocating
    cleanup = gcm[gcm.index("\ncleanup:\n"):]
    for buffer in ("aad_start_stream_buffer", "aad_end_stream_buffer", "aad_desc", "input_start_stream_buffer",
                   "input_end_stream_buffer", "input_desc", "output_start_stream_buffer",
                   "output_end_stream_buffer", "output_desc", "len_buf", "len_desc"):
        assert f"free({buffer});" in cleanup, buffer
    # Declared before the first goto, so cleanup never frees an uninitialised pointer.
    first_goto = gcm.index("goto cleanup;")
    for buffer in ("*aad_start_stream_buffer = NULL", "*output_end_stream_buffer = NULL", "*len_buf = NULL"):
        assert 0 <= gcm.index(buffer) < first_goto, buffer


def patched_sources(idf_path):
    """The two IDF files with both patches applied, from a pristine or patched tree."""
    with tempfile.TemporaryDirectory() as tmp:
        for rel in (GCM_C, DMA_C):
            (Path(tmp) / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(idf_path / rel, Path(tmp) / rel)
        for name in (GATE, CLEANUP):
            patch = str(ROOT / "patches" / name)
            run = lambda *extra: subprocess.run(["git", "apply", *extra, "--check", patch], cwd=tmp,
                                                capture_output=True).returncode == 0
            forward, applied = run(), run("--reverse")
            assert forward != applied, f"{name} matches neither the pristine nor the patched IDF 6.0.2"
            if forward:
                subprocess.run(["git", "apply", patch], cwd=tmp, check=True)
        return [(Path(tmp) / rel).read_text(encoding="utf-8") for rel in (GCM_C, DMA_C)]


idf_path = Path(os.environ.get("IDF_PATH") or Path.home() / ".espressif/v6.0.2/esp-idf")
if not (idf_path / GCM_C).is_file():
    print(f"ESP-IDF AES-GCM PSRAM DMA contract: SKIP (no ESP-IDF 6.0.2 tree at {idf_path})")
    sys.exit(0)
gcm_source, dma_source = patched_sources(idf_path)
check_gate(gcm_source)
check_cleanup(dma_source)
print("ESP-IDF AES-GCM PSRAM DMA contract: PASS")

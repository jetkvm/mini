#!/usr/bin/env python3
"""The CSI DMA interrupt must be able to run while the cache is disabled.

A flash write or erase disables the cache and every interrupt that is not
cache-safe. If the CSI DMA interrupt waits for the flash operation, the
circular descriptor ring can reach a descriptor that was not re-armed and
capture stops. The firmware therefore enables
CONFIG_CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE, and every ESP-IDF DW-GDMA function the
completion callback calls must then be placed in IRAM by a linker mapping that
this option enables. ESP-IDF's mapping does not cover every function this
driver calls, so the component adds its own; this test fails when a call is
added without a mapping.
"""

from pathlib import Path
import os
import re


COMPONENT = Path(__file__).resolve().parents[1]
ROOT = COMPONENT.parents[1]
IDF = Path(os.environ.get("IDF_PATH", Path.home() / ".espressif/v6.0.2/esp-idf"))
source = (COMPONENT / "p4_cam_ctlr_csi.c").read_text()


def extract_function(signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def noflash_conditions(linker_text):
    """Map each `dw_gdma: <fn> (noflash)` entry to the Kconfig conditions guarding it."""
    mapping = {}
    condition = None
    for line in linker_text.splitlines():
        guard = re.match(r"\s*if\s+(\w+)\s*=\s*y\s*:", line)
        if guard:
            condition = guard.group(1)
            continue
        entry = re.match(r"\s*dw_gdma\s*:\s*(\w+)\s*\(noflash\)", line)
        if entry:
            mapping.setdefault(entry.group(1), set()).add(condition)
    return mapping


# Everything the completion interrupt runs in this file.
isr_code = "".join(
    extract_function(signature)
    for signature in (
        "IRAM_ATTR static bool csi_dma_trans_done_callback(",
        "static esp_err_t IRAM_ATTR csi_dma_publish_lli(",
        "static bool IRAM_ATTR csi_dma_callback_enter(",
        "static void IRAM_ATTR csi_dma_callback_exit(",
    )
)
called = sorted(set(re.findall(r"\b(dw_gdma_\w+)\s*\(", isr_code)))
assert called, "the completion callback no longer calls any DW-GDMA function"

# Conditions that CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE turns on.
cam_kconfig = (IDF / "components/esp_driver_cam/Kconfig").read_text()
block = re.search(
    r"config CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE\n(.*?)(?:\n\s*config |\Z)", cam_kconfig, re.S
).group(1)
enabled = {"CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE", "SOC_DW_GDMA_SUPPORTED"}
enabled |= set(re.findall(r"select\s+(\w+)", block))

mapped = noflash_conditions((IDF / "components/esp_driver_dma/linker.lf").read_text())
for fn, conditions in noflash_conditions((COMPONENT / "linker.lf").read_text()).items():
    mapped.setdefault(fn, set()).update(conditions)

missing = [fn for fn in called if not (mapped.get(fn, set()) & enabled)]
assert not missing, (
    "DW-GDMA functions called from the CSI DMA interrupt are not in IRAM when "
    f"CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE is enabled: {missing}"
)

defaults = (ROOT / "sdkconfig.defaults").read_text()
assert re.search(r"^CONFIG_CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE=y$", defaults, re.M), (
    "sdkconfig.defaults must enable CONFIG_CAM_CTLR_MIPI_CSI_ISR_CACHE_SAFE"
)

print(f"test_isr_cache_safe: {len(called)} DW-GDMA calls from the CSI DMA interrupt are in IRAM")

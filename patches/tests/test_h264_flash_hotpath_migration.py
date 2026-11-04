#!/usr/bin/env python3
"""Fresh and already-patched managed dependencies must end with the same flash layout."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
PATCH = Path("patches/esp_h264-1.4.0-flash-hotpath.patch")
VENDOR = Path("managed_components/espressif__esp_h264/linker.lf")
# Original locked esp_h264 1.4.0 decoder mapping; retain its configurable IRAM use.
BASELINE = """[mapping:tinyh264]
archive:libtinyh264.a
entries:
    if ESP_H264_DECODER_IRAM = y:
        h264bsd_reconstruct (noflash)
        h264bsd_cavlc (noflash)
        h264bsd_transform (noflash)
        h264bsd_vlc (noflash)
        h264bsd_stream (noflash)
        h264bsd_intra_prediction (noflash)
    else:
        * (default)
"""
legacy = (ROOT / "patches/tests/fixtures/esp_h264-1.4.0-legacy-linker.lf").read_text()
for name, initial in (("fresh", BASELINE), ("previous IRAM patch", legacy)):
    with tempfile.TemporaryDirectory(prefix="h264-flash-migration-") as directory:
        work = Path(directory)
        subprocess.run(["git", "init", "-q", str(work)], check=True)
        target = work / VENDOR
        target.parent.mkdir(parents=True)
        target.write_text(initial)
        (work / "patches").mkdir()
        shutil.copy2(ROOT / PATCH, work / PATCH)
        script = work / "configure.cmake"
        script.write_text(
            f'set(CMAKE_SOURCE_DIR "{work}")\n'
            f'include("{ROOT}/cmake/apply_managed_component_patch.cmake")\n'
            f'apply_managed_component_patch({PATCH.as_posix()})\n'
        )
        for attempt in range(2):
            result = subprocess.run(
                ["cmake", "-P", str(script)], capture_output=True, text=True
            )
            assert result.returncode == 0, result.stdout + result.stderr
            assert target.read_text() == BASELINE, (name, attempt)
print("H264 Flash placement: fresh/legacy dependency migration and repeated configure: PASS")

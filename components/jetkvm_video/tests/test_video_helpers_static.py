#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
core = (root / "jetkvm_video.c").read_text()
helpers = (root / "jetkvm_video_helpers.c").read_text()
header = (root / "jetkvm_video_helpers.h").read_text()
for name in (
    "h264_raw_frame_size", "h264_raw_luma_range",
    "h264_hw_database_bytes", "video_rate_x100",
    "video_timing_percentile", "video_timing_average",
    "video_timing_maximum",
):
    assert name in helpers
    assert name in header
    assert f"static size_t {name}" not in core
assert "IRAM_ATTR" not in helpers
assert "portENTER_CRITICAL" not in helpers
for lock in ("s_csi_buffer_mux", "s_direct_h264_pipeline_mux", "s_video_metrics_mux"):
    assert lock in core
    assert lock not in helpers
print("video pure helper boundary and ISR lock colocation: PASS")

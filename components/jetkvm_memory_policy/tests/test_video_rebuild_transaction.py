#!/usr/bin/env python3
"""Static acceptance checks for the exclusive video rebuild transaction."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[3]
VIDEO = (ROOT / "components/jetkvm_video/jetkvm_video.c").read_text()
HELPERS = (ROOT / "components/jetkvm_video/jetkvm_video_helpers.c").read_text()
POLICY = (ROOT / "components/jetkvm_memory_policy/jetkvm_memory_policy.c").read_text()
HEADER = (ROOT / "components/jetkvm_memory_policy/include/jetkvm_memory_policy.h").read_text()
HTTP = (ROOT / "components/webrtc_transport/webrtc_stats_json.c").read_text()
TRANSPORT = (ROOT / "components/webrtc_transport/webrtc_transport.c").read_text()


def macro_int(source: str, name: str) -> int:
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(.+)$", source, re.M)
    assert match, f"missing macro {name}"
    expression = match.group(1).split("/*", 1)[0].strip()
    assert re.fullmatch(r"[0-9UuLl\s*()+]+", expression), expression
    expression = re.sub(r"(?<=\d)[UuLl]+", "", expression)
    return int(eval(expression, {"__builtins__": {}}, {}))


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.S)
    assert match, f"missing function {name}"
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:index]
    raise AssertionError(f"unterminated function {name}")


macro = macro_int(HELPERS, "H264_HW_DB_MACRO_SIZE")
row_12 = macro_int(HELPERS, "H264_HW_DB_ROW_12_BYTES")
row_4 = macro_int(HELPERS, "H264_HW_DB_ROW_4_BYTES")
alignment = macro_int(HELPERS, "H264_HW_DB_ALIGNMENT_BYTES")
width_max = macro_int(VIDEO, "FRAME_WIDTH_MAX")
height_max = macro_int(VIDEO, "FRAME_HEIGHT_MAX")
pool_slots = macro_int(VIDEO, "CSI_FRAME_BUFFER_COUNT_DIRECT_HIGH_RATE")


def database_bytes(width: int, height: int) -> int:
    mb_width = (width + macro - 1) // macro
    mb_height = (height + macro - 1) // macro
    return (
        mb_width * (mb_height - 1) * row_12
        + mb_width * (row_12 + row_4)
        + mb_width * (mb_height - 1) * row_4
        + alignment
    )


capture_capacity = width_max * height_max * 3 // 2
# The video pool is allocated once at boot; a rebuild requests none of it.
video_pool = pool_slots * capture_capacity + database_bytes(1920, 1080)
assert video_pool == 21_795_856

# A rebuild's working set is what it still allocates after csi_destroy():
# nothing, or the BGR888 diagnostic's one output. It is a total-free check
# above the admission floor plus a largest-block check for that allocation.
begin = function_body(POLICY, "jetkvm_memory_policy_video_rebuild_begin")
assert "has_projected_floor(" in begin
assert "psram_free, 0U, new_working_set_bytes" in begin
assert "JETKVM_MEMORY_PSRAM_ADMISSION_FLOOR_BYTES" in begin
assert "psram_largest >= new_working_set_bytes" in begin
assert "old_working_set" not in begin

# Begin observes pending PSRAM tickets and publishes the exclusive state while
# holding the same mutex used by reserve/shrink.
assert begin.index("xSemaphoreTake(s_reservation_mutex") < begin.index(
    "pending_psram_materialization"
)
assert "s_reservations[i].request.psram_bytes != 0U" in begin
assert begin.index("s_video_rebuild =") < begin.index(
    "xSemaphoreGive(s_reservation_mutex)"
)

reserve = function_body(POLICY, "reserve_with_domain")
blocks = function_body(POLICY, "video_rebuild_blocks")
assert "s_video_rebuild.active && request->psram_bytes != 0U" in blocks
assert "video_rebuild_blocks(request, psram_only)" in reserve
assert "accepted = !blocked_by_video_rebuild" in reserve
# A TLS-only operation or a logical ingress lease has psram_bytes == 0 and is
# deliberately not rejected by the video transaction predicate.
assert "request->internal_bytes" not in blocks
assert "request->storage_ingress_bytes" not in blocks

# Every owner is judged by the floors and its own largest allocation;
# test_fragmented_psram_admission.py runs the real functions.
fits = function_body(POLICY, "request_fits")
assert "psram_fits(memory, request" in fits
assert "largest <= memory->psram_largest" in function_body(POLICY, "psram_fits")

assert TRANSPORT.count("jetkvm_memory_policy_try_reserve_peer(") == 1
assert "open_ret != ESP_OK && open_ret != ESP_ERR_NOT_FINISHED" in TRANSPORT
assert '"webrtc-peer", &peer_memory' in TRANSPORT

# Initial lock and every later A/B/A rebuild share this one candidate path.
candidates = function_body(VIDEO, "tc_try_candidates_held")
destroy_index = candidates.index("csi_destroy(ctx)")
begin_index = candidates.index("jetkvm_memory_policy_video_rebuild_begin")
start_index = candidates.index("tc_start_streaming")
assert destroy_index < begin_index < start_index
assert candidates.count("jetkvm_memory_policy_video_rebuild_finish") == 3
assert candidates.count("&video_transaction, false") == 2
assert candidates.count("&video_transaction, true") == 1
for failed_stage in ("failed-start cleanup", "failed-capture cleanup"):
    cleanup_index = candidates.index(failed_stage)
    finish_index = candidates.index(
        "jetkvm_memory_policy_video_rebuild_finish", cleanup_index
    )
    assert cleanup_index < finish_index
# The whole candidate loop runs with peer opens held.
wrapper = function_body(VIDEO, "tc_try_candidates")
assert wrapper.index("webrtc_transport_hold_peer_open(true)") < wrapper.index(
    "tc_try_candidates_held(ctx, timings)"
) < wrapper.index("webrtc_transport_hold_peer_open(false)")

helper = function_body(VIDEO, "video_mode_prepare")
for contract in (
    "TC_H264_RAW_FORMAT == ESP_H264_RAW_FMT_BGR888",
    "CSI_FRAME_BUFFER_COUNT_DIRECT_1080P30",
    "CSI_FRAME_BUFFER_COUNT_COMPACT_1080P30",
    "csi_use_compact_ring(ctx, timings, direct_h264_capture)",
    "CSI_FRAME_BUFFER_COUNT_DIRECT_HIGH_RATE",
):
    assert contract in helper

for field in (
    "video_rebuild_active",
    "video_rebuild_begin_denials",
    "video_rebuild_failures",
):
    assert field in HEADER
for field in (
    "videoRebuildActive",
    "videoRebuildBeginDenials",
    "videoRebuildFailures",
):
    assert field in HTTP

print(
    "video rebuild transaction static test passed: "
    f"video pool={video_pool:,}"
)

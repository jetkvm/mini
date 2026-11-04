#!/usr/bin/env python3
"""Static cross-domain memory-budget acceptance model.

This is deliberately an arithmetic/source-contract test, not a replacement
for the runtime heap watermarks.  It makes the large allocations which must
coexist visible in one place and fails when the independently maintained
Kconfig/C constants drift apart.
"""

from __future__ import annotations

import ast
import os
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
KIB = 1024
MIB = 1024 * KIB
BOARD_PSRAM_BYTES = 32 * MIB


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def macros_from(*sources: str) -> dict[str, str]:
    """Collect object-like C macros, including backslash continuations."""

    result: dict[str, str] = {}
    for source in sources:
        logical_lines: list[str] = []
        pending = ""
        for line in read(source).splitlines():
            stripped = line.rstrip()
            if stripped.endswith("\\"):
                pending += stripped[:-1] + " "
                continue
            logical_lines.append(pending + stripped)
            pending = ""
        assert not pending, f"unterminated macro continuation in {source}"
        for line in logical_lines:
            match = re.match(r"\s*#define\s+([A-Za-z_]\w*)\s+(.+?)\s*$", line)
            if match is None:
                continue
            name, expression = match.groups()
            expression = re.sub(r"/\*.*?\*/", "", expression).strip()
            expression = expression.split("//", 1)[0].strip()
            if expression:
                result[name] = expression
    return result


def evaluate(name: str, macros: dict[str, str], active: set[str] | None = None) -> int:
    """Evaluate the integer subset used by the budget macros."""

    if active is None:
        active = set()
    assert name not in active, f"recursive macro: {name}"
    assert name in macros, f"missing macro: {name}"
    active = active | {name}
    expression = re.sub(r"(?<=\d)[uUlL]+\b", "", macros[name])
    tree = ast.parse(expression, mode="eval")

    def visit(node: ast.AST) -> int:
        if isinstance(node, ast.Expression):
            return visit(node.body)
        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            return node.value
        if isinstance(node, ast.Name):
            return evaluate(node.id, macros, active)
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.UAdd, ast.USub)):
            value = visit(node.operand)
            return value if isinstance(node.op, ast.UAdd) else -value
        if isinstance(node, ast.BinOp):
            left = visit(node.left)
            right = visit(node.right)
            if isinstance(node.op, ast.Add):
                return left + right
            if isinstance(node.op, ast.Sub):
                return left - right
            if isinstance(node.op, ast.Mult):
                return left * right
            if isinstance(node.op, (ast.Div, ast.FloorDiv)):
                assert right != 0
                return left // right
            if isinstance(node.op, ast.LShift):
                return left << right
            if isinstance(node.op, ast.RShift):
                return left >> right
        raise AssertionError(f"unsupported integer macro expression: {expression!r}")

    return visit(tree)


def sdkconfig_values(source: str) -> dict[str, int]:
    values: dict[str, int] = {}
    for line in read(source).splitlines():
        match = re.fullmatch(r"(CONFIG_[A-Za-z0-9_]+)=([0-9]+)", line.strip())
        if match:
            values[match.group(1)] = int(match.group(2))
    return values


def kconfig_default(source: str, name: str) -> int:
    text = read(source)
    match = re.search(
        rf"(?ms)^config\s+{re.escape(name)}\s*$.*?^\s*default\s+([0-9]+)\s*$",
        text,
    )
    assert match is not None, f"missing Kconfig default for {name}"
    return int(match.group(1))


def require_compact(source: str, expression: str) -> None:
    source_text = re.sub(r"/\*.*?\*/", "", read(source), flags=re.DOTALL)
    source_text = re.sub(r"//.*", "", source_text)
    compact_source = re.sub(r"\s+", "", source_text)
    compact_expression = re.sub(r"\s+", "", expression)
    assert compact_expression in compact_source, (
        f"source contract missing in {source}: {expression}"
    )


policy = macros_from(
    "components/jetkvm_memory_policy/include/jetkvm_memory_policy.h"
)
main = macros_from(
    "components/jetkvm_video/jetkvm_video.c",
    "components/jetkvm_board_network/jetkvm_board_network.c",
)
screen = macros_from(
    "components/jetkvm_screen/jetkvm_screen.c",
    "components/jetkvm_screen/jetkvm_screen_fit.h",
)
transport = macros_from("components/webrtc_transport/webrtc_transport.c")
cloud = macros_from(
    "components/jetkvm_cloud_client/jetkvm_cloud_client.c",
    "components/jetkvm_cloud_service/jetkvm_cloud_service.c",
)
virtual_media = macros_from("components/jetkvm_virtual_media/jetkvm_virtual_media.c")
virtual_media_http = macros_from(
    "components/jetkvm_virtual_media_http/jetkvm_virtual_media_http.c"
)
virtual_media_http_cmake = read(
    "components/jetkvm_virtual_media_http/CMakeLists.txt"
)
h264 = macros_from(
    "managed_components/espressif__esp_h264/hw/src/esp_h264_enc_hw_param.h",
    "managed_components/espressif__esp_h264/hw/hal/esp32p4/h264_dma_ll.h",
    "managed_components/espressif__esp_h264/hw/src/esp_h264_enc_hw_param.c",
)
peer_arena_macros = macros_from(
    "components/webrtc_transport/peer_memory_arena.h"
)
defaults = sdkconfig_values("sdkconfig.defaults")
configured = sdkconfig_values(os.environ.get("SDKCONFIG", "sdkconfig"))
# The production build resolves this through sdkconfig.h. Seed the same value
# into the static arithmetic model, then exercise alternate A/B values below.
policy["CONFIG_LWIP_TCP_WND_DEFAULT"] = str(
    defaults["CONFIG_LWIP_TCP_WND_DEFAULT"]
)


# lwIP mailbox costs are fixed pointer slots, not preallocated packet
# payloads. The global tcpip queue and each TCP netconn's own receive mailbox
# are 256 entries so browser keep-alive connections do not crowd peer setup
# out of internal RAM.
lwip_mbox_pointer = evaluate("JETKVM_MEMORY_LWIP_MBOX_POINTER_BYTES", policy)
tcpip_mbox_baseline = evaluate(
    "JETKVM_MEMORY_LWIP_TCPIP_MBOX_BASELINE_ENTRIES", policy
)
tcp_mbox_entries = evaluate("JETKVM_MEMORY_LWIP_TCP_MBOX_ENTRIES", policy)
lwip_mbox_ab = evaluate("JETKVM_MEMORY_LWIP_MBOX_AB_ENTRIES", policy)
lwip_max_tcp_sockets = evaluate(
    "JETKVM_MEMORY_LWIP_MAX_TCP_SOCKETS", policy
)
assert lwip_mbox_pointer == 4
assert tcpip_mbox_baseline == 64
assert defaults["CONFIG_LWIP_TCPIP_RECVMBOX_SIZE"] == lwip_mbox_ab
assert defaults["CONFIG_LWIP_TCP_RECVMBOX_SIZE"] == tcp_mbox_entries
assert lwip_max_tcp_sockets == defaults["CONFIG_LWIP_MAX_SOCKETS"]
assert lwip_mbox_ab == 256
assert tcp_mbox_entries == 256
assert evaluate(
    "JETKVM_MEMORY_LWIP_TCPIP_MBOX_AB_GROWTH_BYTES", policy
) == 768
assert evaluate(
    "JETKVM_MEMORY_LWIP_TCP_MBOX_PER_SOCKET_BYTES", policy
) == 1024
assert evaluate(
    "JETKVM_MEMORY_LWIP_TCP_MBOX_ALL_SOCKETS_BYTES", policy
) == 16384
assert evaluate(
    "JETKVM_MEMORY_LWIP_TCPIP_MBOX_AB_PAYLOAD_MAX_BYTES", policy
) == 389_632


# Internal/DMA is one overlapping physical domain, not two additive heaps.
internal_hard = evaluate("JETKVM_MEMORY_INTERNAL_HARD_FLOOR_BYTES", policy)
internal_admit = evaluate("JETKVM_MEMORY_INTERNAL_ADMISSION_FLOOR_BYTES", policy)
dma_hard = evaluate("JETKVM_MEMORY_DMA_HARD_FLOOR_BYTES", policy)
dma_admit = evaluate("JETKVM_MEMORY_DMA_ADMISSION_FLOOR_BYTES", policy)
# A TLS reservation has the same bytes in internal RAM and in its DMA-capable
# part: every measured internal allocation of a connection was DMA-capable.
tls_connection = evaluate("JETKVM_MEMORY_TLS_CONNECTION_RESERVE_BYTES", policy)
assert internal_hard < internal_admit
assert dma_hard < dma_admit
assert defaults["CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL"] == internal_hard

# Named always-live Internal/DMA blocks. Descriptor and allocator overhead is
# intentionally left to the runtime watermark rather than guessed here.
reference_pool = evaluate("ESP_H264_REFERENCE_POOL_SIZE", h264)
tf_dma_scratch = evaluate("TF_UPLOAD_DMA_BOUNCE_BYTES", virtual_media)
tf_upload_staging = evaluate("TF_UPLOAD_OWNER_STAGING_BYTES", virtual_media)
http_upload_control = evaluate(
    "UPLOAD_CONTROL_MEMORY_BYTES", virtual_media_http
)
http_pipeline_half_kib = evaluate(
    "JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE_KIB", virtual_media_http
)
http_pipeline_default_match = re.search(
    r'set\(JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE_KIB "([0-9]+)" CACHE STRING',
    virtual_media_http_cmake,
)
assert http_pipeline_default_match is not None
assert int(http_pipeline_default_match.group(1)) == http_pipeline_half_kib
http_upload_pipeline_payload = 2 * http_pipeline_half_kib * KIB
screen_viewport = (
    evaluate("JETKVM_SCREEN_FLUSH_BUFFER_PIXELS", screen) * 2
)
tinyusb_msc = defaults["CONFIG_TINYUSB_MSC_BUFSIZE"]
ethernet_dma_payload = (
    configured["CONFIG_ETH_DMA_BUFFER_SIZE"]
    * (
        configured["CONFIG_ETH_DMA_RX_BUFFER_NUM"]
        + configured["CONFIG_ETH_DMA_TX_BUFFER_NUM"]
    )
)
named_internal_dma = (
    reference_pool + tf_dma_scratch + screen_viewport + tinyusb_msc
    + ethernet_dma_payload
)
assert reference_pool == 138_247
assert tf_upload_staging == 32 * KIB
assert http_upload_control == 3 * KIB
assert http_pipeline_half_kib == 24
assert http_upload_pipeline_payload == 48 * KIB
assert screen_viewport == 9_600
assert ethernet_dma_payload == 36_864


# The control heap is physically pre-carved, and its sub-budgets must fit it
# exactly. Nothing in this equation is additional general-PSRAM headroom.
control_arena = evaluate("JETKVM_MEMORY_CONTROL_ARENA_BYTES", policy)
control_cloud = evaluate("JETKVM_MEMORY_CONTROL_CLOUD_QUEUE_MAX_BYTES", policy)
control_signaling = evaluate(
    "JETKVM_MEMORY_CONTROL_SIGNALING_MAX_BYTES", policy
)
control_runtime = evaluate(
    "JETKVM_MEMORY_CONTROL_RUNTIME_HEADROOM_BYTES", policy
)
assert control_cloud + control_signaling + control_runtime == control_arena
ui_arena = evaluate("JETKVM_MEMORY_LVGL_ARENA_BYTES", policy) \
    if "JETKVM_MEMORY_LVGL_ARENA_BYTES" in policy else 0
policy_arenas_total = evaluate("JETKVM_MEMORY_POLICY_ARENAS_TOTAL_BYTES", policy) \
    if "JETKVM_MEMORY_POLICY_ARENAS_TOTAL_BYTES" in policy else control_arena
assert policy_arenas_total == control_arena + ui_arena
if ui_arena:
    sdk_defaults_text = read("sdkconfig.defaults")
    assert "CONFIG_LV_USE_CUSTOM_MALLOC=y" in sdk_defaults_text
    assert "# CONFIG_LV_USE_CLIB_MALLOC is not set" in sdk_defaults_text


# Video geometry and the vendor H.264 DB formula are derived from source, not
# copied from the prose table.
width = evaluate("FRAME_WIDTH_MAX", main)
height = evaluate("FRAME_HEIGHT_MAX", main)
capture_capacity = width * height * 3 // 2
capture_720_count = evaluate("CSI_FRAME_BUFFER_COUNT_DIRECT_HIGH_RATE", main)
capture_1080_count = evaluate("CSI_FRAME_BUFFER_COUNT_COMPACT_1080P30", main)
capture_1080_slow_count = evaluate("CSI_FRAME_BUFFER_COUNT_DIRECT_1080P30", main)
assert capture_1080_slow_count == 5
pool_slots = evaluate("VIDEO_POOL_CAPTURE_SLOTS", main)
pool_slot_bytes = evaluate("VIDEO_POOL_CAPTURE_SLOT_BYTES", main)
h264_output = evaluate("H264_OUTPUT_MIN_BYTES", main)

macro = evaluate("H264_DMA_MACRO_SIZE", h264)
row_12 = evaluate("H264_DMA_DB_12_LINES_ROW_LENGTH", h264)
row_4 = evaluate("H264_DMA_DB_4_LINES_ROW_LENGTH", h264)
align_mask = evaluate("HW_DMA_BUF_ALIGN_SIZE", h264)


def h264_db_bytes(frame_width: int, frame_height: int) -> int:
    mb_width = (frame_width + macro - 1) // macro
    mb_height = (frame_height + macro - 1) // macro
    return (
        mb_width * (mb_height - 1) * row_12
        + mb_width * (row_12 + row_4)
        + mb_width * (mb_height - 1) * row_4
        + ((align_mask + 1) << 1)
    )


db_720 = h264_db_bytes(1280, 720)
db_1080 = h264_db_bytes(1920, 1080)
# Streaming borrows its output from the AU pool counted separately below.
video_720 = capture_720_count * capture_capacity + db_720
video_1080 = capture_1080_count * capture_capacity + db_1080
assert capture_capacity == 3_110_400
assert db_720 == 1_382_416
assert db_1080 == 3_133_456
assert video_720 == 20_044_816
assert video_1080 == 15_575_056
assert video_720 - video_1080 == 4_469_760

# Without the fixed pool, each mode would allocate its own slot count and
# the DB of its own resolution. The safe EDID offers these modes; slot counts
# follow video_mode_prepare(): compact ring at direct 1080p30, five slots at
# 1080p24/25, six for every other direct mode.
per_mode_video = {
    "1920x1080p30": capture_1080_count * capture_capacity + db_1080,
    "1920x1080p25": capture_1080_slow_count * capture_capacity + db_1080,
    "1280x720p60": capture_720_count * capture_capacity + db_720,
    "1280x720p30": capture_720_count * capture_capacity + db_720,
    "1024x768p30": capture_720_count * capture_capacity
    + h264_db_bytes(1024, 768),
    "1280x800p30": capture_720_count * capture_capacity
    + h264_db_bytes(1280, 800),
    "1280x1024p30": capture_720_count * capture_capacity
    + h264_db_bytes(1280, 1024),
}
per_mode_worst_name = max(per_mode_video, key=per_mode_video.get)
per_mode_worst = per_mode_video[per_mode_worst_name]
assert per_mode_worst_name == "1280x1024p30"
assert per_mode_worst == 20_628_496

# The fixed pool: six slots of the largest direct-capture frame and the
# 1920x1080 DB, allocated at boot and never freed. Every mode above fits in
# it, so it is the video core in every mode.
assert pool_slots == capture_720_count == 6
assert pool_slot_bytes == capture_capacity
video_pool = pool_slots * pool_slot_bytes + db_1080
assert video_pool == 21_795_856
for mode_bytes in per_mode_video.values():
    assert mode_bytes <= video_pool
assert video_pool - video_720 == db_1080 - db_720 == 1_751_040


# Transport fixed data: the media_lib arena plus the separately allocated
# peer RTP ringbuffer backing.
au_slots = kconfig_default(
    "components/webrtc_transport/Kconfig", "WEBRTC_TRANSPORT_AU_SLOTS"
)
au_bytes = defaults["CONFIG_WEBRTC_TRANSPORT_MAX_AU_BYTES"]
rtp_pool = defaults["CONFIG_WEBRTC_TRANSPORT_RTP_POOL_BYTES"]
sctp_send_cache = defaults[
    "CONFIG_WEBRTC_TRANSPORT_DATA_CHANNEL_SEND_CACHE_BYTES"
]
sctp_receive_cache = defaults[
    "CONFIG_WEBRTC_TRANSPORT_DATA_CHANNEL_RECEIVE_CACHE_BYTES"
]
rpc_scratch = evaluate("JETKVM_MEMORY_RPC_SCRATCH_BYTES", policy)
measured_rpc_scratch = 32 * KIB
rpc_scratch_growth = rpc_scratch - measured_rpc_scratch
assert au_bytes == h264_output
assert au_slots == 2
assert rpc_scratch == 128 * KIB
assert rpc_scratch_growth == 96 * KIB
assert sctp_send_cache == 256 * KIB
assert sctp_receive_cache == evaluate(
    "JETKVM_MEMORY_SCTP_RECEIVE_MAX_BYTES", policy
)
assert sctp_receive_cache == kconfig_default(
    "components/webrtc_transport/Kconfig",
    "WEBRTC_TRANSPORT_DATA_CHANNEL_RECEIVE_CACHE_BYTES",
)
assert configured[
    "CONFIG_WEBRTC_TRANSPORT_DATA_CHANNEL_RECEIVE_CACHE_BYTES"
] == sctp_receive_cache
peer_arena_overhead = evaluate(
    "WEBRTC_PEER_MEMORY_ARENA_OVERHEAD_BYTES", peer_arena_macros
)
peer_arena_bytes = (
    sctp_send_cache + sctp_receive_cache + peer_arena_overhead
)
peer_materialization_bytes = peer_arena_bytes + rtp_pool
assert peer_arena_overhead == 256 * KIB
assert sctp_receive_cache == 256 * KIB
assert peer_arena_bytes == 786_432
assert peer_materialization_bytes == 1_835_008
assert max(peer_arena_bytes, rtp_pool) == 1_048_576
require_compact(
    "components/webrtc_transport/webrtc_transport.c",
    "const size_t peer_arena_bytes = "
    "CONFIG_WEBRTC_TRANSPORT_DATA_CHANNEL_SEND_CACHE_BYTES + "
    "CONFIG_WEBRTC_TRANSPORT_DATA_CHANNEL_RECEIVE_CACHE_BYTES + "
    "WEBRTC_PEER_MEMORY_ARENA_OVERHEAD_BYTES",
)
require_compact(
    "components/webrtc_transport/webrtc_transport.c",
    "const size_t peer_materialization_bytes = peer_arena_bytes + "
    "CONFIG_WEBRTC_TRANSPORT_RTP_POOL_BYTES",
)
require_compact(
    "components/webrtc_transport/webrtc_transport.c",
    ".psram_bytes = peer_materialization_bytes, "
    ".psram_largest_block = peer_largest_block",
)
require_compact(
    "components/webrtc_transport/webrtc_transport.c",
    "const jetkvm_memory_policy_request_t peer_growth_memory = { "
    ".storage_ingress_bytes = JETKVM_MEMORY_SCTP_RECEIVE_MAX_BYTES",
)

# The local source component retains the peer arena allocation boundary.
root_cmake = read("CMakeLists.txt")
peer_component_root = ROOT / "third_party/esp_peer"
peer_cmake = read("third_party/esp_peer/CMakeLists.txt")
assert "third_party/esp_peer" in root_cmake
assert "idf_component_register(" in peer_cmake
assert "src/core" in peer_cmake
assert "add_prebuilt_library(" not in peer_cmake
assert (peer_component_root / "src/core/agent.c").is_file()
assert (peer_component_root / "src/core/sctp.c").is_file()
arena_source = read("components/webrtc_transport/peer_memory_arena.c")
assert "multi_heap_register(base, capacity_bytes)" in arena_source
assert "media_lib_strdup" not in arena_source
assert "peer_ringbuffer_create" not in arena_source
assert "peer_ringbuffer_delete" not in arena_source
assert "info.allocated_blocks != 0U" in arena_source
assert "preserving backing memory" in arena_source
arena_malloc = arena_source[
    arena_source.index("void *media_lib_malloc"):
    arena_source.index("void *media_lib_calloc")
]
# Outside an arena, media_lib blocks go to PSRAM: libc malloc() would put the
# data channel caches' small blocks in internal RAM, which TLS needs.
fallback_caps = set(re.search(
    r"#define PEER_FALLBACK_CAPS \(([^)]*)\)", arena_source
).group(1).replace(" ", "").split("|"))
assert "MALLOC_CAP_SPIRAM" in fallback_caps
assert not fallback_caps & {"MALLOC_CAP_INTERNAL", "MALLOC_CAP_DMA"}
assert arena_malloc.index("s_arena_base") < \
    arena_malloc.index("return heap_caps_malloc(size, PEER_FALLBACK_CAPS)")
assert not re.search(r"\bmalloc\(", arena_malloc)
assert "return heap_caps_realloc(pointer, size, PEER_FALLBACK_CAPS)" in \
    arena_source
assert not re.search(r"\brealloc\(", arena_source)
assert "Never let a late vendor allocation escape" in arena_malloc

# Source-built peer code follows the same arena ABI. The weak fallback is
# the only source file allowed to mention libc allocation directly; it serves
# certificate pre-generation before an arena exists and non-KVM consumers.
source_direct_pattern = re.compile(
    r"\b(?:malloc|calloc|realloc|free|strdup|strndup|aligned_alloc|"
    r"memalign|posix_memalign|heap_caps_[A-Za-z0-9_]+)\s*\("
)
peer_source_root = peer_component_root / "src"
source_bypasses: list[str] = []
for source_path in peer_source_root.rglob("*"):
    if source_path.suffix not in {".c", ".h"} or \
            source_path.name == "media_lib_weak.c":
        continue
    for line_number, line in enumerate(
        source_path.read_text(encoding="utf-8").splitlines(), 1
    ):
        if source_direct_pattern.search(line):
            source_bypasses.append(
                f"{source_path.relative_to(ROOT)}:{line_number}:{line.strip()}"
            )
assert not source_bypasses, source_bypasses


# Storage ingress is one logical ledger. The physical RTC writer and future
# reorder allocation are parts of this total, not independent 4 MiB caches.
ingress_total = evaluate(
    "JETKVM_MEMORY_STORAGE_INGRESS_TOTAL_MAX_BYTES", policy
)
rtc_shared = evaluate("JETKVM_MEMORY_RTC_SHARED_POOL_BYTES", policy)
http_writer = evaluate("JETKVM_MEMORY_HTTP_WRITER_POOL_BYTES", policy)
http_tcp_receive = evaluate(
    "JETKVM_MEMORY_HTTP_TCP_RECEIVE_MAX_BYTES", policy
)
http_ingress = evaluate("JETKVM_MEMORY_HTTP_INGRESS_MAX_BYTES", policy)
sctp_receive = evaluate("JETKVM_MEMORY_SCTP_RECEIVE_MAX_BYTES", policy)
protocol_metadata = evaluate(
    "JETKVM_MEMORY_RTC_PROTOCOL_METADATA_MAX_BYTES", policy
)
ingress_headroom = evaluate(
    "JETKVM_MEMORY_RTC_INGRESS_HEADROOM_BYTES", policy
)
assert rtc_shared + sctp_receive + protocol_metadata + ingress_headroom == ingress_total
assert rtc_shared == 2 * MIB
assert sctp_receive == 256 * KIB
assert protocol_metadata == 4 * KIB
assert ingress_headroom == 1788 * KIB
assert rtc_shared + sctp_receive + protocol_metadata == 2_363_392
assert ingress_total - (
    rtc_shared + sctp_receive + protocol_metadata
) == 1_830_912
assert http_writer <= rtc_shared
assert policy["JETKVM_MEMORY_HTTP_TCP_RECEIVE_MAX_BYTES"] == (
    "CONFIG_LWIP_TCP_WND_DEFAULT"
)
assert http_tcp_receive == defaults["CONFIG_LWIP_TCP_WND_DEFAULT"] == 1280 * KIB
assert http_ingress == http_writer + http_tcp_receive
assert sctp_receive + http_ingress == 2_097_152
assert ingress_total - (sctp_receive + http_ingress) == 2_097_152

# An alternate A/B window must change physical PSRAM admission and the logical
# storage-ingress lease by the same signed amount. This models the exact macros
# used by every HTTP upload/discard reservation.
for ab_window in (1 * MIB, 2 * MIB):
    ab_policy = dict(policy)
    ab_policy["CONFIG_LWIP_TCP_WND_DEFAULT"] = str(ab_window)
    ab_tcp_receive = evaluate(
        "JETKVM_MEMORY_HTTP_TCP_RECEIVE_MAX_BYTES", ab_policy
    )
    ab_http_ingress = evaluate(
        "JETKVM_MEMORY_HTTP_INGRESS_MAX_BYTES", ab_policy
    )
    assert ab_tcp_receive == ab_window
    assert ab_http_ingress == http_writer + ab_window
    assert http_ingress - ab_http_ingress == 1280 * KIB - ab_window
# A live video/control peer and one HTTP file upload must fit together.
assert http_ingress + sctp_receive <= ingress_total
require_compact(
    "components/jetkvm_virtual_media_http/jetkvm_virtual_media_http.c",
    "UPLOAD_BUFFER_COUNT * UPLOAD_BUFFER_SIZE == "
    "JETKVM_MEMORY_HTTP_WRITER_POOL_BYTES",
)
require_compact(
    "components/jetkvm_virtual_media/jetkvm_virtual_media.c",
    ".internal_bytes = TF_UPLOAD_OWNER_STAGING_BYTES, "
    ".internal_largest_block = TF_UPLOAD_OWNER_STAGING_BYTES, "
    ".dma_bytes = TF_UPLOAD_OWNER_STAGING_BYTES, "
    ".dma_largest_block = TF_UPLOAD_OWNER_STAGING_BYTES",
)
require_compact(
    "components/jetkvm_virtual_media_http/jetkvm_virtual_media_http.c",
    ".internal_bytes = UPLOAD_CONTROL_MEMORY_BYTES + "
    "UPLOAD_BUFFER_COUNT * UPLOAD_BUFFER_SIZE, "
    ".internal_largest_block = UPLOAD_ALLOCATION_SIZE, "
    ".dma_bytes = UPLOAD_CONTROL_MEMORY_BYTES + "
    "UPLOAD_BUFFER_COUNT * UPLOAD_BUFFER_SIZE, "
    ".dma_largest_block = UPLOAD_ALLOCATION_SIZE",
)


# A measured hardware baseline: 720p60 cold, and the same run with a
# DataChannel connected. Deriving peer occupancy by subtraction keeps the
# formula tied to that one run.
cold_internal, cold_dma = 215_523, 176_407
cold_general, cold_largest = 10_263_044, 10_223_616
peer_internal, peer_dma, peer_low = 197_411, 158_295, 8_559_104
peer_materialized_peak = cold_general - peer_low
pre_large_window_peer_arena_bytes = 1_703_936
assert cold_general == 10_263_044
assert cold_largest == 10_223_616
assert peer_internal == 197_411
assert peer_dma == 158_295
assert peer_materialized_peak >= pre_large_window_peer_arena_bytes
assert peer_materialized_peak - pre_large_window_peer_arena_bytes <= 256

# RTC materializes one shared 32 KiB staging block. Production HTTP bypasses
# that VM staging and instead owns two 24 KiB Internal/DMA halves plus bounded
# FreeRTOS control objects. Keep the two geometries separate so a smaller HTTP
# half cannot accidentally be modelled as only one half.
rtc_tls_internal = peer_internal - tf_upload_staging - tls_connection
rtc_tls_dma = peer_dma - tf_upload_staging - tls_connection
assert rtc_tls_internal >= internal_admit
assert rtc_tls_dma >= dma_admit

# A measured connected baseline (peer and its ingress lease) for HTTP setup.
# The 24 KiB geometry must admit the upload itself, and a TLS connection
# beside it stays above both admission floors.
http_preflight_internal, http_preflight_dma = 170_463, 131_347
http_upload_internal = (
    http_preflight_internal
    - http_upload_pipeline_payload
    - http_upload_control
)
http_upload_dma = (
    http_preflight_dma
    - http_upload_pipeline_payload
    - http_upload_control
)
http_tls_internal = http_upload_internal - tls_connection
http_tls_dma = http_upload_dma - tls_connection
assert http_upload_internal >= internal_admit
assert http_upload_dma >= dma_admit
assert http_tls_internal >= internal_admit
assert http_tls_dma >= dma_admit
assert http_upload_internal == 118_239
assert http_upload_dma == 79_123
assert http_tls_internal == 110_047
assert http_tls_dma == 70_931
# A second connection beside the upload does not fit in DMA-capable RAM.
assert http_tls_dma - tls_connection < dma_admit

# The measured baseline already paid for the original 1.5 MiB early carve-out
# and the then-current 32 KiB RPC scratch. Splitting the same arena bytes
# between control and UI must not be charged a second time. The current build's
# larger RPC scratch has not yet been remeasured, so project its exact growth
# out of the measured free value instead of presenting a synthetic sample.
# That baseline also owned a separate 512 KiB output, now borrowed from AU.
baseline_policy_arenas = 1536 * KIB
baseline_private_output = 512 * KIB
additional_policy_arena = max(0, policy_arenas_total - baseline_policy_arenas)
effective_cold_general = (
    cold_general - additional_policy_arena - rpc_scratch_growth + baseline_private_output
)
projected_worst_general = (
    effective_cold_general
    - peer_materialization_bytes
    - rtc_shared
    - protocol_metadata
)
psram_admission_floor = evaluate(
    "JETKVM_MEMORY_PSRAM_ADMISSION_FLOOR_BYTES", policy
)
assert projected_worst_general >= psram_admission_floor
worst_margin = projected_worst_general - psram_admission_floor


# Reconcile most of the 32 MiB device PSRAM. Queue control blocks, allocator
# metadata and conditional Cloud/WSS stacks live in the small residual; they
# are intentionally observed at runtime instead of estimated here.
screen_render = screen_viewport
mandatory_psram_stacks = (
    evaluate("SCREEN_TASK_STACK_BYTES", screen)
    + evaluate("SCREEN_STATUS_TASK_STACK", main)
    + evaluate("CLOUD_SERVICE_TASK_STACK", cloud)
    + evaluate("TF_PREFETCH_TASK_STACK_BYTES", virtual_media)
)
known_steady_general = video_720 + au_slots * au_bytes + rpc_scratch
reconciled = (
    effective_cold_general
    + known_steady_general
    + policy_arenas_total
    + screen_render
    + mandatory_psram_stacks
)
unclassified_psram = BOARD_PSRAM_BYTES - reconciled
assert 0 <= unclassified_psram < 128 * KIB

# The cold baseline was measured at 720p60, whose video core is smaller than
# the pool by the DB difference, and smaller than the per-mode worst case
# (1280x1024p30) before the pool. Project both from the same measurement.
pool_cold_general = effective_cold_general - (video_pool - video_720)
pool_worst_general = (
    pool_cold_general
    - peer_materialization_bytes
    - rtc_shared
    - protocol_metadata
)
per_mode_worst_general = projected_worst_general - (per_mode_worst - video_720)
assert pool_worst_general >= psram_admission_floor
pool_margin = pool_worst_general - psram_admission_floor
per_mode_margin = per_mode_worst_general - psram_admission_floor
assert pool_margin == 807_428
assert per_mode_margin == 1_974_788
# Production attaches no RTC shared pool; without it the pool keeps 2.9 MB.
pool_margin_production = pool_margin + rtc_shared
assert pool_margin_production == 2_904_580


# The current policy has enough ticket slots for the documented legal owners;
# this is a slot-count check, not a claim that all TLS operations fit at once.
assert evaluate("JETKVM_MEMORY_RESERVATION_SLOTS", policy) >= 8

print("memory coexistence budget: PASS")
print(
    f"  video core 720p60={video_720:,} 1080p30={video_1080:,} "
    f"delta={video_720 - video_1080:,}"
)
print(
    f"  fixed video pool={video_pool:,} ({pool_slots} x {pool_slot_bytes:,} "
    f"+ DB {db_1080:,}); per-mode worst before the pool="
    f"{per_mode_worst:,} ({per_mode_worst_name})"
)
print(
    f"  control={control_arena:,} ingress={ingress_total:,} "
    f"UI={ui_arena:,} peer media_lib arena={peer_arena_bytes:,}"
)
print(
    f"  peer RTP backing={rtp_pool:,} total materialization="
    f"{peer_materialization_bytes:,}"
)
print(
    f"  worst general at 720p60={projected_worst_general:,} "
    f"4 MiB margin={worst_margin:,} extra arena charge="
    f"{additional_policy_arena:,} RPC scratch growth="
    f"{rpc_scratch_growth:,}"
)
print(
    f"  worst general with the pool={pool_worst_general:,} "
    f"4 MiB margin={pool_margin:,} "
    f"(without the unattached RTC pool {pool_margin_production:,}); "
    f"per-mode before the pool at {per_mode_worst_name}="
    f"{per_mode_worst_general:,} margin={per_mode_margin:,}"
)
print(
    f"  named Internal/DMA fixed={named_internal_dma:,} "
    f"(DMA is included in Internal)"
)
print(
    f"  connected HTTP 2x{http_pipeline_half_kib} KiB upload leaves "
    f"Internal/DMA={http_upload_internal:,}/{http_upload_dma:,}; "
    f"one TLS connection beside it projects "
    f"{http_tls_internal:,}/{http_tls_dma:,} (admitted)"
)
print(
    f"  historical peer materialized delta={peer_materialized_peak:,}; "
    "current admission uses explicit media_lib-arena plus RTP totals"
)
print(
    f"  32 MiB reconciliation residual={unclassified_psram:,} "
    "(metadata/conditional tasks)"
)

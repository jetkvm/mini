#!/usr/bin/env python3
"""Static contracts for line-rate HTTP-to-TF upload buffering."""

from pathlib import Path


COMPONENT = Path(__file__).resolve().parents[1]
SOURCE = (COMPONENT / "jetkvm_virtual_media_http.c").read_text(encoding="utf-8")
CMAKE = (COMPONENT / "CMakeLists.txt").read_text(encoding="utf-8")
KCONFIG = (COMPONENT / "Kconfig").read_text(encoding="utf-8")
WEBRTC_HTTP = (
    COMPONENT.parent / "webrtc_transport" / "webrtc_http.c"
).read_text(encoding="utf-8")
WEBRTC_STATS = (
    COMPONENT.parent / "webrtc_transport" / "webrtc_stats_json.c"
).read_text(encoding="utf-8")
MEMORY_POLICY = (
    COMPONENT.parent
    / "jetkvm_memory_policy"
    / "include"
    / "jetkvm_memory_policy.h"
).read_text(encoding="utf-8")

for contract in (
    "JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT",
    "JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE",
    "JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE_KIB",
    "((size_t)JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE_KIB * 1024U)",
    "#define UPLOAD_ALLOCATION_SIZE (UPLOAD_BUFFER_COUNT * UPLOAD_BUFFER_SIZE)",
    "#define UPLOAD_ALLOCATION_COUNT 1U",
    "#define UPLOAD_BUFFER_SIZE (64U * 1024U)",
    "#define UPLOAD_BUFFER_COUNT 1U",
    "MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT",
    "UPLOAD_FALLBACK_BUFFER_CAPS",
    '"http-storage-upload-psram"',
    "fallback_memory_request",
    "fallback_to_psram ? UPLOAD_FALLBACK_BUFFER_CAPS",
    "#define UPLOAD_BUFFER_SIZE (256U * 1024U)",
    "#define UPLOAD_DMA_ALIGNMENT 128U",
    "#define UPLOAD_BUFFER_COUNT 2U",
    "#define UPLOAD_CONTROL_MEMORY_BYTES (3U * 1024U)",
    "heap_caps_aligned_alloc(",
    "xTaskCreateWithCaps(",
    "upload_writer_task",
    "pipeline.free_buffers",
    "pipeline.ready_chunks",
    "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT",
    "JETKVM_MEMORY_HTTP_WRITER_POOL_BYTES",
    "CONFIG_LWIP_TCP_WND_DEFAULT ==",
    "JETKVM_MEMORY_HTTP_TCP_RECEIVE_MAX_BYTES",
    "JETKVM_MEMORY_HTTP_INGRESS_MAX_BYTES",
    ".storage_ingress_bytes =",
    "jetkvm_memory_policy_shrink(",
    "while (buffered < target)",
    "target - buffered",
    "http_upload_receive_idle_timed_out(",
    'failed_stage = "receive-timeout"',
    '"408 Request Timeout"',
    "last_receive_progress_us = esp_timer_get_time()",
    "pipeline->upload_id, pipeline->owner_token,",
    "chunk.data, chunk.size, NULL, NULL",
    '"UPLOAD_PASS bytes=%" PRIu64 " total_ms=%lld throughput_mbit_s=%llu.%02llu "',
    '"UPLOAD_FAIL stage=%s error=%s bytes=%" PRIu64 " remaining=%" PRIu64 " "',
    "send_json_and_close(",
    '(void)httpd_resp_set_hdr(request, "Connection", "close")',
    "if (remaining > 0U)",
    "read_and_clear_emac_rx_drop_counters()",
    "pipeline->emac_during_write",
    "pipeline->emac_writer_gaps",
    "emacMissedDuringWriteFrames",
    "maxWriteEmacMissedFrames",
    "maxEmacMissedDuringWriteFrames",
):
    assert contract in SOURCE, f"missing upload throughput contract: {contract}"

for contract in (
    'set(JETKVM_TF_HTTP_PSRAM_DIRECT "1" CACHE STRING',
    'set(JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT "1" CACHE STRING',
    'set(JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE "1" CACHE STRING',
    'set(JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE_KIB "24" CACHE STRING',
    'MATCHES "^(24|32|40)$"',
    "must be 24, 32 or 40",
    "JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT=${JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT}",
    "JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE=${JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE}",
    "JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE_KIB=${JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE_KIB}",
    "JETKVM_HTTP_UPLOAD_INTERNAL_PIPELINE requires",
    "JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT AND NOT JETKVM_TF_HTTP_PSRAM_DIRECT",
):
    assert contract in CMAKE, f"missing internal-direct build contract: {contract}"

assert SOURCE.count("return send_json_and_close(") >= 10, (
    "every upload rejection with an unread request body must close the socket"
)

for field in (
    ".internal_bytes = UPLOAD_CONTROL_MEMORY_BYTES",
    ".internal_largest_block = UPLOAD_CONTROL_MEMORY_BYTES",
    ".dma_bytes = UPLOAD_CONTROL_MEMORY_BYTES",
    ".dma_largest_block = UPLOAD_CONTROL_MEMORY_BYTES",
):
    assert field in SOURCE, f"missing HTTP upload Internal/DMA budget: {field}"

# On the Wi-Fi board the payload goes to PSRAM first and the upload holds
# the link's queue until it ends, as a download does.
for contract in (
    "#define UPLOAD_LINK_RESERVE_BYTES JETKVM_MEMORY_DOWNLOAD_LINK_RESERVE_BYTES",
    "#if JETKVM_HTTP_UPLOAD_INTERNAL_DIRECT && CONFIG_ESP_HOSTED_ENABLED\n"
    "#define UPLOAD_PAYLOAD_PSRAM_FIRST 1",
    "bool memory_reserved = !UPLOAD_PAYLOAD_PSRAM_FIRST &&",
    ".dma_bytes = UPLOAD_CONTROL_MEMORY_BYTES + UPLOAD_LINK_RESERVE_BYTES,",
):
    assert contract in SOURCE, f"missing Wi-Fi upload link contract: {contract}"
assert SOURCE.count(
    ".dma_bytes = UPLOAD_CONTROL_MEMORY_BYTES + UPLOAD_LINK_RESERVE_BYTES,"
) == 2, "both PSRAM upload requests reserve the link's queue"
lease = SOURCE[SOURCE.index(
    "const jetkvm_memory_policy_request_t ingress_lease",
    SOURCE.index("static esp_err_t upload_handler_sync("),
):]
lease = lease[:lease.index("};")]
assert ".dma_bytes = UPLOAD_LINK_RESERVE_BYTES" in lease, (
    "the upload holds the link's queue until it ends"
)

assert SOURCE.index("while (buffered < target)") < SOURCE.index(
    "upload_chunk_t chunk = {.data = buffer, .size = buffered}"
), "TCP fragments must be aggregated before queuing one FatFs write"
assert "esp_timer" in CMAKE and "heap" in CMAKE

for contract in (
    '"/storage/upload/start"',
    'cJSON_GetObjectItemCaseSensitive(root, "sha256")',
    "bool sha256_present = sha256 != NULL",
    "sha256_hex_valid(sha256->valuestring)",
    "object_has_exactly_three_members(root, filename, size, sha256)",
    "object_has_exact_members(root, filename, size)",
    "JETKVM_VIRTUAL_MEDIA_UPLOAD_SHA256_UNAVAILABLE",
    "requested_sha256",
    "sha256 must be 64 hex characters when present",
    '"/storage/files"',
    '"/storage/space"',
    '"/storage/mount"',
    '"/storage/unmount"',
    "local_upload_lease_arm(upload.upload_id)",
    "local_upload_lease_take(upload_id)",
    "local_upload_lease_abort_current()",
    "httpd_queue_work(server, local_upload_lease_expire_work, NULL)",
    "jetkvm_virtual_media_abort_upload_owned(upload_id, owner_token)",
    "jetkvm_virtual_media_mount(requested_filename, requested_mode)",
    "CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_LOCAL_CONTROL",
):
    assert contract in SOURCE, f"missing local REST control contract: {contract}"

for contract in (
    '"/storage/diagnostics/http-discard"',
    '"X-JetKVM-Diagnostic"',
    '"http-discard-v1"',
    "CONFIG_JETKVM_VIRTUAL_MEDIA_HTTP_DIAGNOSTIC_DISCARD",
    "http_discard_content_length_valid(request->content_len)",
    '"http-discard-diagnostic"',
    ".storage_ingress_bytes = JETKVM_MEMORY_HTTP_INGRESS_MAX_BYTES",
    "HTTP_DISCARD_BUFFER_SIZE",
    "http_discard_metrics_record(",
    "http_upload_receive_idle_timed_out(",
    '"HTTP_DISCARD_PASS bytes=%" PRIu64',
    '"receiveTimeouts\\\":%" PRIu32',
    '"throughputMbitX100\\\":%llu',
    "httpd_unregister_uri_handler(\n            server, HTTP_DISCARD_URI",
):
    assert contract in SOURCE, f"missing HTTP discard diagnostic contract: {contract}"

discard_start = SOURCE.index(
    "static esp_err_t http_discard_handler(httpd_req_t *request)"
)
discard_end = SOURCE.index("\n}\n#endif", discard_start)
discard_handler = SOURCE[discard_start:discard_end]
for forbidden in (
    "jetkvm_virtual_media_start_upload",
    "jetkvm_virtual_media_claim_upload",
    "jetkvm_virtual_media_write_upload",
    "jetkvm_virtual_media_finish_upload",
    "jetkvm_virtual_media_abort_upload",
):
    assert forbidden not in discard_handler, (
        f"discard diagnostic must not touch storage: {forbidden}"
    )

assert "config JETKVM_VIRTUAL_MEDIA_HTTP_DIAGNOSTIC_DISCARD" in KCONFIG
discard_kconfig = KCONFIG.split(
    "config JETKVM_VIRTUAL_MEDIA_HTTP_DIAGNOSTIC_DISCARD", 1
)[1].split("endmenu", 1)[0]
assert "default n" in discard_kconfig
assert "depends on JETKVM_VIRTUAL_MEDIA_HTTP_LOCAL_CONTROL" in discard_kconfig

# Both authenticated listeners reserve space for diagnostics and optional discard.
assert "config->max_uri_handlers = 30;" in WEBRTC_HTTP

# A build-local receive-window A/B must change both lwIP and the physical/
# logical HTTP admission lease, and expose all three effective values in stats.
assert (
    "#define JETKVM_MEMORY_HTTP_TCP_RECEIVE_MAX_BYTES \\\n    CONFIG_LWIP_TCP_WND_DEFAULT"
    in MEMORY_POLICY
)
for telemetry_contract in (
    '"tcpMboxCapacityPerSocket"',
    '"tcpReceiveWindowBytes"',
    '"httpTcpReceiveMaximumBytes"',
    '"httpIngressMaximumBytes"',
):
    assert telemetry_contract in WEBRTC_STATS

print("virtual-media HTTP upload buffering contract: PASS")

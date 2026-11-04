#!/usr/bin/env python3
"""Static boundary/schema checks for the WebRTC stats renderer."""

from pathlib import Path


COMPONENT = Path(__file__).resolve().parents[1]
HTTP = (COMPONENT / "webrtc_http.c").read_text()
RENDERER = (COMPONENT / "webrtc_stats_json.c").read_text()
HEADER = (COMPONENT / "webrtc_stats_json.h").read_text()

assert "webrtc_stats_json_create(cJSON **root_out)" in HEADER
assert "webrtc_stats_json_create(&root)" in HTTP
assert "webrtc_transport_get_stats(&stats)" not in HTTP
assert "jetkvm_memory_policy_get_snapshot" not in HTTP
assert "jetkvm_usb_service_get_snapshot" not in HTTP
assert "cJSON_AddBoolToObject(root, \"started\"" not in HTTP

for required_key in (
    "started",
    "modeValid",
    "jetkvmDataChannels",
    "usbHid",
    "usbService",
    "stageAge",
    "networkTiming",
    "ethernetRx",
    "memoryBudget",
    "taskStacks",
    "firmware",
):
    assert f'\"{required_key}\"' in RENDERER, required_key

assert "*root_out = NULL" in RENDERER
assert "*root_out = root" in RENDERER
assert "cJSON_Delete(root)" not in RENDERER
assert "httpd_req_t" not in RENDERER
assert "httpd_resp_" not in RENDERER

print("WebRTC stats JSON renderer boundary: PASS")

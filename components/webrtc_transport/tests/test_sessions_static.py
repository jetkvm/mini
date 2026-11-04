#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
core = (root / "webrtc_transport.c").read_text()
sessions = (root / "webrtc_sessions.c").read_text()
header = (root / "webrtc_sessions_internal.h").read_text()

public_apis = (
    "webrtc_transport_session_create",
    "webrtc_transport_cloud_session_create",
    "webrtc_transport_session_poll",
    "webrtc_transport_session_submit",
    "webrtc_transport_session_close",
    "webrtc_transport_cloud_session_submit_offer",
    "webrtc_transport_cloud_session_submit_candidate",
    "webrtc_transport_cloud_session_close",
    "webrtc_transport_signal_event_release",
)
for name in public_apis:
    assert name in sessions, f"session owner missing {name}"
    assert name not in core, f"transport core still implements {name}"
for helper in ("outbound_push", "flush_outbound", "flush_inbound", "free_inbound_signal"):
    assert helper in sessions, f"signaling owner missing {helper}"
    assert helper in header, f"internal boundary missing {helper}"
assert "esp_peer_new" not in sessions
assert "esp_peer_send_video" not in sessions
for contract in (
    "esp_err_t webrtc_transport_set_video_paused(bool paused)",
    "ctx->video_paused = paused",
    "flush_ready_slots_locked(ctx)",
    "ctx->sync_idr_pending_generation = ctx->active_peer_generation",
    "!ctx->video_paused",
):
    assert contract in core, f"video pause contract missing {contract}"
print("WebRTC session/signaling boundary: PASS")

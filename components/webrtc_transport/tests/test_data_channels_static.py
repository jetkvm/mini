#!/usr/bin/env python3
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
core = (root / "webrtc_transport.c").read_text()
channels = (root / "webrtc_data_channels.c").read_text()
header = (root / "webrtc_data_channels_internal.h").read_text()
for owner in (
    "peer_on_channel_open", "peer_on_channel_close", "peer_on_data",
    "jetkvm_control_handle_hidrpc_with_context",
    "jetkvm_control_handle_rpc_with_context",
    "send_one_pending_rpc_event", "send_one_pending_hidrpc_state",
    "webrtc_transport_send_rpc_notification",
    "webrtc_transport_video_state_changed",
):
    assert owner in channels, f"DataChannel owner missing {owner}"
    assert owner not in core or owner in (
        "peer_on_channel_open", "peer_on_channel_close", "peer_on_data",
        "send_one_pending_rpc_event", "send_one_pending_hidrpc_state",
    ), f"transport core still implements {owner}"
for callback in ("peer_on_channel_open", "peer_on_channel_close", "peer_on_data"):
    assert callback in header
    assert f".{callback.removeprefix('peer_')} = {callback}" in core
assert "esp_peer_send_video" not in channels
assert "static void peer_task(" not in channels
# A worker reply is sent from its own buffer, then released whether or not
# it was sent; nothing leaves the loop between the two.
replies = channels[channels.index("void send_pending_rpc_worker_replies("):]
replies = replies[:replies.index("\n}\n")]
assert replies.index("rpc_worker_reply_text(&reply)") < replies.index(
    "rpc_worker_release_reply(&reply);")
assert replies.count("rpc_worker_release_reply(") == 1
assert "reply.json" not in replies
for exit_statement in ("return", "break", "continue", "goto"):
    assert exit_statement not in replies, f"worker replies {exit_statement}"


def body(source, signature):
    text = source[source.index(signature):]
    return text[:text.index("\n}\n")]


# Every videoInputState, the terminal one included, is rendered in one place,
# which adds the OS Services fields or counts a render error without them.
video = body(channels, "static bool render_video_input_event(")
assert channels.count("webrtc_rpc_event_render(WEBRTC_RPC_EVENT_VIDEO_INPUT") == 1
assert "webrtc_rpc_event_render(WEBRTC_RPC_EVENT_VIDEO_INPUT" in video
assert "jetkvm_control_render_guest_video_fields" in video
assert "ctx->stats.rpc_event_render_errors++" in video
assert "return render_video_input_event(ctx, snapshot" in body(
    channels, "static bool render_rpc_event(")
assert "render_video_input_event(ctx, &snapshot" in body(
    channels, "void send_terminal_video_event(")
assert "#define JETKVM_RPC_EVENT_OUTPUT_MAX 1280\n" in channels
# The public hook queues under the lock and wakes the peer task after it.
changed = body(channels, "void webrtc_transport_video_state_changed(void)")
assert changed.index("portENTER_CRITICAL(&ctx->lock);") < changed.index(
    "queue_video_input_event_locked(ctx);") < changed.index(
    "portEXIT_CRITICAL(&ctx->lock);") < changed.index("notify_peer_task(ctx);")
public = (root / "include" / "webrtc_transport.h").read_text()
assert "void webrtc_transport_video_state_changed(void);" in public

# OS Services channels (guest_channels.c holds the table). Only the current
# session's channels are offered to the handler; a refused one is closed by
# its stream, since others may share its label.
assert '#include "webrtc_guest_channels.h"' in public
opened = body(channels, "int peer_on_channel_open(")
assert "guest_channels_label(channel->label)" in opened
assert "guest_current = peer_session_current_locked(ctx);" in opened
assert opened.index("portEXIT_CRITICAL(&ctx->lock);") < opened.index(
    "guest_channels_open(")
assert "peer_sctp_stream_label(label, channel->stream_id);" in opened
assert "guest_channels_closed(" in body(channels, "int peer_on_channel_close(")
received = body(channels, "int peer_on_data(")
assert received.index("guest_current &&") < received.index(
    "guest_channels_message(") < received.rindex(
    "ctx->stats.data_channel_protocol_errors++;")
# The handler (OS Services) takes its own lock: never call the table, and so
# the handler, inside the transport's spinlock. Only the label check, which
# reads nothing else, runs there.
at = 0
while True:
    start = channels.find("portENTER_CRITICAL(&ctx->lock);", at)
    if start < 0:
        break
    end = channels.index("portEXIT_CRITICAL(&ctx->lock);", start)
    held = channels[start:end]
    calls = set(re.findall(r"\bguest_channels_\w+\(", held))
    assert calls <= {"guest_channels_label("}, held
    at = end
# Output goes after the RPC replies and events, which a full send buffer
# would otherwise hold back; teardown reports every channel closed after
# the vendor closed the peer.
loop = body(core, "static void peer_task(void *arg)\n{")
assert loop.index("send_pending_rpc_worker_replies(ctx);") < loop.rindex(
    "send_one_pending_rpc_event(ctx, now_us);") < loop.index(
    "send_guest_channel_output(ctx, now_us);")
teardown = body(core, "static void close_peer(transport_ctx_t *ctx)\n{")
assert teardown.index(
    'close_peer_handle_and_flush_outbound(ctx, peer, "peer teardown");') < \
    teardown.index("reset_guest_channels(ctx);")
guest_send = body(channels, "static guest_send_result_t guest_send(")
assert "ESP_PEER_ERR_WOULD_BLOCK" in guest_send and "GUEST_SEND_BLOCKED" in guest_send
print("WebRTC DataChannel/HIDRPC boundary: PASS")

/* Component-private WebRTC DataChannel control-plane boundary. */
#pragma once

#include "webrtc_transport_ctx_internal.h"

void clear_rpc_events_locked(transport_ctx_t *ctx);
void clear_hidrpc_state_notifications_locked(transport_ctx_t *ctx);
void reset_data_channels_locked(transport_ctx_t *ctx);
bool reboot_rpc_peer_current_locked(
    const transport_ctx_t *ctx, uint32_t session_id, uint32_t generation,
    uint16_t stream_id);
void hid_input_invalidate_generation(uint32_t generation,
                                     const char *reason);
void send_terminal_video_event(transport_ctx_t *ctx);
void queue_video_input_event_locked(transport_ctx_t *ctx);
void drain_pending_rpc_notifications(transport_ctx_t *ctx);
void poll_rpc_hid_state(transport_ctx_t *ctx, int64_t now_us);
void send_one_pending_rpc_event(transport_ctx_t *ctx, int64_t now_us);
void send_one_pending_hidrpc_state(transport_ctx_t *ctx, int64_t now_us);
int peer_on_channel_open(esp_peer_data_channel_info_t *channel, void *arg);
int peer_on_channel_close(esp_peer_data_channel_info_t *channel, void *arg);
int peer_on_data(esp_peer_data_frame_t *frame, void *arg);

void poll_rpc_network_state(transport_ctx_t *ctx, int64_t now_us);
void send_pending_rpc_worker_replies(transport_ctx_t *ctx);
/* Peer task: sends the OS Services channels' output when the handler asked,
 * or every GUEST_OUTPUT_POLL_US for a close that came due. */
void send_guest_channel_output(transport_ctx_t *ctx, int64_t now_us);
/* Peer task, after esp_peer_close(): every guest channel closed. */
void reset_guest_channels(transport_ctx_t *ctx);
/* Peer task: sends what waits in the send queue, before anything new. */
void send_queued_data_channel_messages(transport_ctx_t *ctx);
/* Peer task, after esp_peer_close(): the waiting messages are dropped. */
void clear_data_channel_queue(transport_ctx_t *ctx);

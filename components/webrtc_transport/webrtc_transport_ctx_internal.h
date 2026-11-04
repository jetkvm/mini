/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* Component-private transport context shared by webrtc_transport.c and the
 * split-out adapter/event modules.  Everything here is guarded by the single
 * ctx->lock discipline documented in webrtc_transport.c; never take a second
 * lock around it. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_peer.h"
#include "esp_peer_default.h"

#include "jetkvm_memory_policy.h"
#include "controlled_session_policy.h"
#include "adaptive_bitrate.h"
#include "data_channel_queue.h"
#include "guest_channels.h"
#include "h264_profile.h"
#include "network_timing.h"
#include "peer_memory_arena.h"
#include "reboot_flow.h"
#include "rpc_events.h"
#include "signaling_budget.h"
#include "webrtc_transport.h"
#include "webrtc_transport_private.h"

#define SIGNAL_QUEUE_DEPTH WEBRTC_SIGNAL_QUEUE_DEPTH

/* OTA progress events render to roughly 600-900 bytes; keep four slots so a
 * 1%-cadence download burst cannot overrun a slow SCTP drain. */
#define WEBRTC_OTA_NOTIFY_SLOTS 4U
#define WEBRTC_OTA_NOTIFY_BYTES 1280U

typedef enum {
    AU_SLOT_FREE = 0,
    AU_SLOT_FILLING,
    AU_SLOT_READY,
    AU_SLOT_SENDING,
} au_slot_state_t;

typedef struct {
    /* The slot's buffer: its own allocation, or a buffer lent for guest
     * video (webrtc_transport_set_guest_buffers()). */
    uint8_t *data;
    size_t capacity;
    uint8_t *own;
    size_t size;
    /* CPU-copy prefix of data to clean before it is lent to DMA. */
    size_t cpu_dirty_bytes;
    uint32_t pts_90khz;
    uint32_t generation;
    uint32_t video_epoch;
    uint32_t telemetry_epoch;
    uint32_t capture_sequence;
    uint32_t sync_idr_generation;
    uint64_t order;
    int64_t capture_eof_us;
    int64_t ppa_done_us;
    int64_t encode_start_us;
    int64_t encode_done_us;
    int64_t au_copy_ready_us;
    bool keyframe;
    bool has_idr;
    bool has_sps;
    bool has_pps;
    au_slot_state_t state;
} au_slot_t;

typedef struct {
    webrtc_signal_type_t type;
    uint32_t session_id;
    uint32_t generation;
    char *data;
    size_t size;
    size_t budget_bytes;
    uint8_t payload[];
} inbound_signal_t;

typedef struct {
    /* refs is mutated only under transport_ctx_t.lock. */
    uint32_t refs;
    uint8_t count;
    esp_peer_ice_server_cfg_t servers[WEBRTC_TRANSPORT_MAX_ICE_SERVERS];
} owned_ice_server_list_t;

#define WEBRTC_CONTROL_ICE_STRING_BOUND_BYTES                         \
    (WEBRTC_TRANSPORT_MAX_ICE_SERVERS *                               \
     (WEBRTC_TRANSPORT_MAX_ICE_SERVER_URL_SIZE + 1U +                 \
      WEBRTC_TRANSPORT_MAX_ICE_USERNAME_SIZE + 1U +                   \
      WEBRTC_TRANSPORT_MAX_ICE_CREDENTIAL_SIZE + 1U))
#define WEBRTC_CONTROL_SDP_COPY_BOUND_BYTES \
    (WEBRTC_SIGNAL_PAYLOAD_MAX_BYTES + 144U)
#define WEBRTC_CONTROL_AUX_ALLOCATOR_MARGIN_BYTES (4U * 1024U)

/* ICE ownership and one peer-task SDP rewrite together consume less than a
 * quarter of runtime headroom.  The remaining three quarters stay available
 * to cJSON, OIDC/JWKS, HTTP rendering and callback-held messages. */
_Static_assert(sizeof(owned_ice_server_list_t) +
                       WEBRTC_CONTROL_ICE_STRING_BOUND_BYTES +
                       WEBRTC_CONTROL_SDP_COPY_BOUND_BYTES +
                       WEBRTC_CONTROL_AUX_ALLOCATOR_MARGIN_BYTES <=
                   JETKVM_MEMORY_CONTROL_RUNTIME_HEADROOM_BYTES / 4U,
               "bounded ICE/SDP control allocations exceed their headroom");

typedef struct {
    bool active;
    uint32_t telemetry_epoch;
    uint32_t capture_sequence;
    uint32_t expected_rtp_timestamp;
    int64_t capture_eof_us;
    int64_t ppa_done_us;
    int64_t encode_start_us;
    int64_t encode_done_us;
    int64_t au_copy_ready_us;
    int64_t peer_take_us;
    int64_t first_rtp_us;
    int64_t marker_us;
} active_send_trace_t;

typedef struct {
    portMUX_TYPE lock;
    bool initializing;
    bool stopping;
    bool started;
    bool stop_requested;
    bool mode_valid;
    /* The mode, while valid, is service video's
     * (webrtc_transport_set_guest_mode()): its producer waits for a free
     * slot, where the HDMI encoder skips the frame. */
    bool guest_mode;
    /* OS Services moves the display between sources; no_signal is not
     * reported meanwhile. */
    bool video_switching;
    bool session_active;
    bool rebuild_requested;
    /* Bumped by every mode publish or clear. The peer outlives it; an access
     * unit or sync IDR from an earlier epoch is never sent. */
    uint32_t video_epoch;
    uint32_t mode_teardown_waiters;
    bool closing_peer;
    bool peer_connected;
    webrtc_bitrate_controller_t bitrate_controller;
    bool rpc_data_channel_open;
    bool hidrpc_data_channel_open;
    bool hidrpc_unreliable_ordered_data_channel_open;
    bool hidrpc_unreliable_unordered_data_channel_open;
    bool video_paused;             /* video_pause_sources != 0 */
    uint8_t video_pause_sources;   /* webrtc_video_pause_source_t bits */
    bool waiting_for_sync;
    bool have_last_pts;
    bool session_playout_delay_requested;
    bool active_playout_delay_requested;
    bool active_playout_delay_offered;
    bool active_playout_delay_negotiated;
    uint8_t active_playout_delay_extension_id;
    /* The browser offer's H.264 profiles, for the answer to use. */
    webrtc_h264_offer_profiles_t controlled_offer_h264_profiles;
    uint32_t local_access_epoch;
    webrtc_transport_session_role_t session_role;
    webrtc_transport_session_role_t active_peer_role;
    uint32_t session_playout_delay_session_id;
    uint32_t active_playout_delay_generation;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint32_t session_id;
    uint32_t generation;
    uint32_t active_peer_session;
    uint32_t active_peer_generation;
    uint32_t active_hid_generation;
    uint32_t sync_idr_pending_generation;
    uint32_t sync_idr_inflight_generation;
    uint32_t sync_idr_committed_generation;
    uint32_t sync_idr_last_requested_generation;
    int64_t session_last_poll_us;
    int64_t reconnect_not_before_us;
    uint32_t submitters;
    /* A guest producer waits for a FREE slot; woken when one is released. */
    TaskHandle_t slot_waiter;
    uint16_t rpc_data_channel_stream_id;
    uint16_t hidrpc_data_channel_stream_id;
    uint16_t hidrpc_unreliable_ordered_data_channel_stream_id;
    uint16_t hidrpc_unreliable_unordered_data_channel_stream_id;
    uint64_t next_au_order;
    uint64_t last_sent_au_order;
    uint32_t last_sent_au_generation;
    int64_t last_sent_encode_done_us;
    int64_t last_sent_start_us;
    uint32_t video_ssrc;
    uint32_t video_ssrc_generation;
    /* Inside esp_peer_send_video() (peer task only), and the SSRC and peer
     * generation of its packets, whose resends network timing counts. */
    bool sending_video;
    uint32_t retransmit_ssrc;
    uint32_t retransmit_generation;
    uint32_t last_pli_generation;
    int64_t last_pli_request_us;

    bool hidrpc_handshake_complete;
    uint32_t hidrpc_state_pending_mask;
    uint32_t hidrpc_state_revision[WEBRTC_HIDRPC_STATE_COUNT];
    uint8_t hidrpc_state_retry_count[WEBRTC_HIDRPC_STATE_COUNT];
    int64_t hidrpc_state_retry_not_before_us[WEBRTC_HIDRPC_STATE_COUNT];
    uint8_t hidrpc_state_cursor;
    bool hidrpc_macro_observed_valid;
    uint64_t hidrpc_macro_observed_revision;
    bool hidrpc_macro_active;
    bool hidrpc_macro_is_paste;

    /* Server notifications use one coalescing bit per state family.  They
     * deliberately are not a FIFO: the browser only needs the newest USB,
     * keyboard and video state, and an old generation must never be replayed
     * after a reconnect. */
    uint32_t rpc_event_pending_mask;
    uint32_t rpc_event_revision[WEBRTC_RPC_EVENT_COUNT];
    uint8_t rpc_event_retry_count[WEBRTC_RPC_EVENT_COUNT];
    int64_t rpc_event_retry_not_before_us[WEBRTC_RPC_EVENT_COUNT];
    uint8_t rpc_event_cursor;
    int64_t rpc_event_next_hid_poll_us;
    int64_t rpc_network_next_poll_us;
    bool rpc_event_hid_valid;
    /* Set by the HID worker when the keyboard report changed; the peer loop
     * polls the HID snapshot at once instead of on the poll timer. */
    atomic_bool rpc_event_hid_dirty;
    webrtc_rpc_hid_state_t rpc_event_last_hid;
    bool rpc_terminal_video_event_pending;
    uint32_t rpc_terminal_video_session_id;
    uint32_t rpc_terminal_video_generation;
    uint16_t rpc_terminal_video_stream_id;

    /* Pre-rendered JSON-RPC notifications from foreign tasks (OTA progress,
     * OTA willReboot).  esp_peer_send_data() may only run on peer_task, so
     * producers enqueue under the lock and peer_task drains the ring.  This
     * is best-effort: a full ring drops the oldest pending payload because a
     * stalled browser must never block the updater. */
    uint8_t ota_notify_count;
    uint8_t ota_notify_head;
    uint16_t ota_notify_sizes[WEBRTC_OTA_NOTIFY_SLOTS];
    char ota_notify_payloads[WEBRTC_OTA_NOTIFY_SLOTS][WEBRTC_OTA_NOTIFY_BYTES];
    uint64_t ota_notify_dropped;

    /* Reboot is a single request slot.  The peer callback may only reserve
     * it; response/event dispatch stays on the peer task and esp_restart()
     * belongs exclusively to the low-priority CPU0 worker. */
    bool reboot_available;
    bool reboot_task_stop_requested;
    bool reboot_executing;
    webrtc_reboot_slot_t reboot_slot;

    webrtc_transport_cloud_signal_cb_t cloud_signal_cb;
    webrtc_transport_cloud_lifecycle_cb_t cloud_lifecycle_cb;
    void *cloud_signal_ctx;
    webrtc_cloud_callback_gate_t cloud_callback_gate;
    bool cloud_close_pending;
    bool cloud_invalidation_pending;
    uint32_t cloud_invalidation_session_id;
    uint32_t cloud_invalidated_generation;
    uint32_t cloud_replacement_generation;
    owned_ice_server_list_t *session_ice_servers;
    owned_ice_server_list_t *active_peer_ice_servers;

    TaskHandle_t task;
    SemaphoreHandle_t task_done;
    bool peer_task_join_pending;
    TaskHandle_t reboot_task;
    SemaphoreHandle_t reboot_task_done;
    bool reboot_task_join_pending;
    QueueHandle_t inbound_queue;
    esp_peer_handle_t peer;
    jetkvm_memory_policy_reservation_t peer_growth_reservation;
    jetkvm_memory_policy_reservation_t peer_tls_reservation;
    bool peer_tls_release_pending;
    esp_peer_default_cfg_t peer_default_cfg;
    esp_peer_rtp_transform_cb_t rtp_transform;

    /* The browser's OS Services channels. The handler is set under the
     * lock; the table belongs to the peer task, which alone opens, routes,
     * sends and closes them. output_pending asks the next loop to send. */
    webrtc_transport_guest_channels_t guest_handler;
    guest_channels_t guest_channels;
    atomic_bool guest_output_pending;
    int64_t guest_output_next_poll_us;
    int64_t guest_output_not_before_us;

    /* peer_on_data is invoked synchronously by esp_peer_main_loop(), and that
     * loop has exactly one owner: peer_task.  A transport-lifetime scratch is
     * therefore sufficient for both the temporary VM directory snapshot and
     * its serialized RPC response; no lock or per-message allocation is
     * needed.  Stop frees it only after peer_task has exited. */
    char *rpc_scratch;

    /* RPC and HIDRPC messages the SCTP send buffer could not take yet; the
     * peer task's alone. */
    data_channel_queue_t send_queue;

    au_slot_t slots[CONFIG_WEBRTC_TRANSPORT_AU_SLOTS];
    webrtc_signal_event_t *outbound[SIGNAL_QUEUE_DEPTH];
    uint8_t outbound_head;
    uint8_t outbound_count;
    webrtc_signaling_budget_t signaling_budget;

    active_send_trace_t active_send_trace;
    webrtc_transport_stats_t stats;
} transport_ctx_t;

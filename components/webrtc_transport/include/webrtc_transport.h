/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "webrtc_guest_channels.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Coarse state exported for logs and the browser diagnostic endpoint. */
typedef enum {
    WEBRTC_TRANSPORT_PEER_STOPPED = 0,
    WEBRTC_TRANSPORT_PEER_WAITING,
    WEBRTC_TRANSPORT_PEER_GATHERING,
    WEBRTC_TRANSPORT_PEER_PAIRING,
    WEBRTC_TRANSPORT_PEER_CONNECTING,
    WEBRTC_TRANSPORT_PEER_CONNECTED,
    WEBRTC_TRANSPORT_PEER_FAILED,
} webrtc_transport_peer_state_t;

/** Signaling role for the one active transport session. */
typedef enum {
    /** Diagnostic HTTP path: the device creates an offer and Chrome answers. */
    WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING = 0,
    /** Cloud path: Chrome creates an offer and the device answers. */
    WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED,
    /** Local WebSocket path: Chrome creates an offer and the device answers. */
    WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLED,
} webrtc_transport_session_role_t;

/** Outbound signaling produced by a CLOUD_CONTROLLED session. */
typedef enum {
    WEBRTC_TRANSPORT_CLOUD_SIGNAL_ANSWER = 1,
    WEBRTC_TRANSPORT_CLOUD_SIGNAL_CANDIDATE,
} webrtc_transport_cloud_signal_type_t;

#define WEBRTC_TRANSPORT_CLOUD_SESSION_API_VERSION     2U
#define WEBRTC_TRANSPORT_MAX_ICE_SERVERS              16U
#define WEBRTC_TRANSPORT_MAX_ICE_SERVER_URL_SIZE       1024U
#define WEBRTC_TRANSPORT_MAX_ICE_USERNAME_SIZE          256U
#define WEBRTC_TRANSPORT_MAX_ICE_CREDENTIAL_SIZE       1024U

/** One STUN/TURN server copied into a CLOUD_CONTROLLED session. */
typedef struct {
    /** Required, NUL-terminated STUN/TURN URL. */
    const char *url;
    /** Optional, NUL-terminated TURN username. */
    const char *username;
    /** Optional, NUL-terminated TURN credential. */
    const char *credential;
} webrtc_transport_ice_server_t;

typedef enum {
    /** esp_peer is initialized and this generation can accept its offer. */
    WEBRTC_TRANSPORT_CLOUD_LIFECYCLE_PEER_READY = 1,
    /** The old generation can no longer accept signaling; make a new offer. */
    WEBRTC_TRANSPORT_CLOUD_LIFECYCLE_GENERATION_INVALIDATED,
    /** The matching explicit cloud-session close has completed logically. */
    WEBRTC_TRANSPORT_CLOUD_LIFECYCLE_SESSION_CLOSED,
} webrtc_transport_cloud_lifecycle_type_t;

typedef struct {
    webrtc_transport_cloud_lifecycle_type_t type;
    uint32_t session_id;
    /** Ready, invalidated, or closed generation, depending on type. */
    uint32_t generation;
    /** New generation for GENERATION_INVALIDATED; zero for other events. */
    uint32_t replacement_generation;
} webrtc_transport_cloud_lifecycle_event_t;

/**
 * Nonblocking cloud-session lifecycle sink.
 *
 * Lifecycle and signaling callbacks are serialized.  PEER_READY and
 * GENERATION_INVALIDATED run on the peer task.  SESSION_CLOSED runs on the
 * explicit close caller after any older callback has returned, and close does
 * not return until it does.  A lifecycle sink may submit signaling for the
 * advertised current generation, but must not call create/close/start/stop
 * reentrantly.
 */
typedef void (*webrtc_transport_cloud_lifecycle_cb_t)(
    const webrtc_transport_cloud_lifecycle_event_t *event, void *ctx);

/**
 * Nonblocking cloud signaling sink.
 *
 * The callback runs on the peer task.  data is NUL-terminated for convenience
 * but size is authoritative, and both are valid only until the callback
 * returns.  Return true only after the adapter has copied/queued the payload.
 * session_id plus generation let the adapter reject a late callback after a
 * reconnect or HDMI mode rebuild.  Lifecycle and signaling callbacks are
 * serialized.  The sink must not call create/close/start/stop reentrantly;
 * create/close wait for an in-flight callback to return so its ctx can be
 * released safely after close.
 */
typedef bool (*webrtc_transport_cloud_signal_cb_t)(
    uint32_t session_id, uint32_t generation,
    webrtc_transport_cloud_signal_type_t type,
    const char *data, size_t size, void *ctx);

typedef struct {
    webrtc_transport_cloud_signal_cb_t on_signal;
    webrtc_transport_cloud_lifecycle_cb_t on_lifecycle;
    void *ctx;
    /** Stamp min=0/max=0 only if the browser offer advertises the extension. */
    bool sender_playout_delay;
    /**
     * Optional STUN/TURN list.  Create validates and deeply copies the list,
     * including every string, so the caller may release it on return.
     */
    const webrtc_transport_ice_server_t *ice_servers;
    size_t ice_server_count;
} webrtc_transport_cloud_session_cfg_t;

/** One capture-EOF-relative firmware stage-age accumulator. */
typedef struct {
    uint64_t count;
    uint64_t total_us;
    uint32_t max_us;
} webrtc_transport_stage_age_stats_t;

/** One synchronous packet-processing cost accumulator. */
typedef struct {
    uint64_t count;
    uint64_t total_us;
    uint64_t bytes;
    uint64_t errors;
    uint32_t max_us;
} webrtc_transport_packet_cost_stats_t;

/**
 * Mode-scoped stage ages for access units accepted by esp_peer_send_video().
 * total_us is kept for atomic snapshots; the HTTP endpoint exports its integer
 * average so large cumulative totals never pass through cJSON's double.
 */
typedef struct {
    uint32_t epoch;
    webrtc_transport_stage_age_stats_t capture_to_ppa_done;
    webrtc_transport_stage_age_stats_t capture_to_encode_start;
    webrtc_transport_stage_age_stats_t capture_to_encode_done;
    webrtc_transport_stage_age_stats_t capture_to_au_copy_ready;
    webrtc_transport_stage_age_stats_t capture_to_peer_take;
    webrtc_transport_stage_age_stats_t capture_to_send_video_return;
    webrtc_transport_stage_age_stats_t capture_to_first_rtp;
    webrtc_transport_stage_age_stats_t capture_to_rtp_marker;
    webrtc_transport_stage_age_stats_t capture_to_srtp_first;
    webrtc_transport_stage_age_stats_t capture_to_srtp_marker;
    webrtc_transport_stage_age_stats_t capture_to_udp_send_first;
    webrtc_transport_stage_age_stats_t capture_to_udp_send_marker;
    webrtc_transport_stage_age_stats_t capture_to_emac_handoff_first;
    webrtc_transport_stage_age_stats_t capture_to_emac_handoff_marker;
    uint64_t invalid_timestamps;
    uint64_t order_errors;
    uint64_t accumulator_overflows;
    uint64_t epoch_mismatches;
    uint64_t missing_first_rtp;
    uint64_t missing_rtp_marker;
} webrtc_transport_stage_age_summary_t;

/** Packet-level costs and trace health below the RTP transformer. */
typedef struct {
    webrtc_transport_packet_cost_stats_t srtp_protect;
    webrtc_transport_packet_cost_stats_t srtp_cipher;
    webrtc_transport_packet_cost_stats_t srtp_auth_and_framework;
    webrtc_transport_packet_cost_stats_t lwip_sendto;
    webrtc_transport_packet_cost_stats_t emac_handoff;
    uint64_t trace_overwrites;
    uint64_t unmatched_srtp_packets;
    uint64_t unmatched_udp_packets;
    uint64_t unmatched_emac_packets;
    /* srtp_protect() failures are especially important: esp_peer's
     * void DTLS wrapper otherwise hides the status from its RTP sender. */
    uint64_t srtp_error_replay_fail;
    uint64_t srtp_error_replay_old;
    uint64_t srtp_error_buffer_small;
    uint64_t srtp_error_auth_fail;
    uint64_t srtp_error_cipher_fail;
    uint64_t srtp_error_other;
    int32_t srtp_last_error_status;
    uint16_t srtp_last_error_sequence;
    uint32_t srtp_last_error_timestamp;
    uint32_t srtp_last_error_ssrc;
    uint32_t srtp_last_error_input_bytes;
    uint32_t srtp_last_error_output_capacity;
    uint32_t srtp_last_error_output_bytes;
    uint32_t srtp_last_error_fingerprint;
    /* Since boot, unlike the fields above, which restart with the video
     * epoch: video RTP packets that esp_peer sent again after a NACK. It
     * resends the packet it sent before, so lwip_sendto() sees a sequence
     * number of the video SSRC that does not advance. A packet sent through
     * a TURN relay carries the relay's framing and is not seen. */
    uint64_t rtp_retransmits;
} webrtc_transport_network_timing_stats_t;

/* Cumulative duration histogram: how many samples took at least 10, 25 and
 * 50 ms, and the longest one. */
typedef struct {
    uint32_t count;
    uint32_t over_10ms;
    uint32_t over_25ms;
    uint32_t over_50ms;
    uint32_t max_us;
} webrtc_transport_duration_hist_t;

/* Peer-task passes from the wake to the end of the pass, the wait for work
 * not included: how many, their total time, how many took longer than one
 * frame at 60 fps (16.7 ms), and the longest. A pass sends at most one
 * service video frame, so passes that long hold service video below 60 fps. */
typedef struct {
    uint64_t count;
    uint64_t sum_us;
    uint64_t over_16ms;
    uint32_t max_us;
} webrtc_transport_peer_pass_t;

/* Cores whose idle-task runs the stats report. */
#define WEBRTC_TRANSPORT_IDLE_CORES 2

/* esp_peer_send_video() cost for access units in one size range. srtp_us and
 * sendto_us are the per-packet SRTP and lwip_sendto time inside those sends. */
typedef struct {
    uint32_t count;
    uint32_t max_us;
    uint64_t bytes;
    uint64_t packets;
    uint64_t total_us;
    uint64_t srtp_us;
    uint64_t sendto_us;
} webrtc_transport_send_cost_t;

#define WEBRTC_TRANSPORT_SEND_SIZE_BUCKETS 4 /* <16 KB, <48 KB, <96 KB, larger */

/* Phases of one peer-task iteration, in loop order. */
typedef enum {
    WEBRTC_PEER_PHASE_WAIT = 0,    /* ulTaskNotifyTake: idle until a frame, signal or 10 ms tick */
    WEBRTC_PEER_PHASE_CONTROL,     /* session expiry, rebuild, open, inbound signaling, HID state */
    WEBRTC_PEER_PHASE_MEDIA,       /* audio, one video access unit, audio */
    WEBRTC_PEER_PHASE_MAIN_LOOP,   /* esp_peer_main_loop: ICE/DTLS/SCTP receive and timers */
    WEBRTC_PEER_PHASE_RPC,         /* RPC notifications and state events */
    WEBRTC_PEER_PHASE_COUNT,
} webrtc_transport_peer_phase_t;

/** Monotonic transport counters.  Callers may take deltas over any interval. */
typedef struct {
    bool started;
    bool mode_valid;
    /** Set by webrtc_transport_set_video_switching(). */
    bool video_switching;
    bool browser_session_active;
    bool peer_connected;
    /** The webrtc_video_pause_source_t bits that hold video off. */
    uint8_t video_pause_sources;
    uint32_t video_target_bitrate;
    uint32_t video_bitrate_ceiling;
    uint32_t video_remb_bitrate;
    uint32_t video_remb_reports;
    uint32_t video_remb_decreases;
    /* Controller inputs at its last evaluation, for offline replay. */
    int64_t video_bitrate_step_ms;
    uint64_t video_bitrate_step_bytes;
    uint64_t video_bitrate_step_stalls;
    uint64_t video_bitrate_step_slot_drops;
    uint32_t video_bitrate_step_remb;
    uint32_t video_bitrate_step_reports;
    uint32_t video_bitrate_step_decreases;
    int32_t video_bitrate_step_lost;
    uint32_t video_bitrate_step_sequence;
    uint32_t video_bitrate_step_loss_reports;
    uint32_t video_bitrate_loss_permille; /* receiver loss over the last interval */
    /* Latest RTCP receiver report block for the video source. */
    uint32_t video_rtcp_loss_reports;
    int32_t video_rtcp_cumulative_lost;
    uint32_t video_rtcp_highest_sequence;
    uint32_t video_achieved_bitrate;
    uint32_t video_remb_cuts;
    uint32_t video_remb_ignored;
    uint32_t video_bitrate_restores;
    const char *video_bitrate_reason; /* reason for the last evaluation, static string */
    webrtc_transport_duration_hist_t peer_phase[WEBRTC_PEER_PHASE_COUNT];
    webrtc_transport_duration_hist_t au_take_wait; /* AU ready to peer take */
    webrtc_transport_peer_pass_t peer_pass;
    /* Since boot, per core: the runs of the core's idle task, one each time
     * the core wakes from its wait for an interrupt with nothing else to
     * run. A core whose count stops growing had no idle moment. The rate
     * follows the interrupts, not the idle time. idle_counted is false for a
     * core without the idle hook, whose count stays 0. */
    uint64_t idle_runs[WEBRTC_TRANSPORT_IDLE_CORES];
    bool idle_counted[WEBRTC_TRANSPORT_IDLE_CORES];
    /* Between consecutive video frames of one peer, since the last
     * webrtc_transport_reset_video_intervals(): the encoder outputs of the
     * frames sent, and their esp_peer_send_video() calls. */
    webrtc_transport_duration_hist_t video_encode_interval;
    webrtc_transport_duration_hist_t video_send_interval;
    webrtc_transport_send_cost_t video_send_by_size[WEBRTC_TRANSPORT_SEND_SIZE_BUCKETS];
    bool waiting_for_sync_frame;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    uint32_t session_id;
    uint32_t generation;
    uint32_t video_epoch;
    webrtc_transport_peer_state_t peer_state;

    uint64_t access_units_offered;
    uint64_t access_units_enqueued;
    uint64_t access_units_sent;
    uint64_t access_unit_bytes_sent;
    uint64_t access_units_replaced;
    uint64_t access_units_oversize;
    uint64_t access_units_bad_annexb;
    uint64_t access_units_no_session;
    uint64_t access_units_mode_mismatch;
    uint64_t access_units_waiting_for_sync;
    uint64_t access_units_queue_full;
    uint64_t access_units_no_slot;
    uint64_t access_units_send_errors;
    uint64_t access_units_would_block;
    uint64_t pts_regressions;
    uint32_t last_input_pts_90khz;

    uint64_t rtp_packets_sent;
    uint64_t rtp_bytes_sent;
    uint64_t rtp_marker_packets;
    uint32_t last_rtp_timestamp_90khz;
    bool rtp_playout_delay_requested;
    bool rtp_playout_delay_offered;
    bool rtp_playout_delay_negotiated;
    uint8_t rtp_playout_delay_extension_id;
    uint64_t rtp_playout_delay_offer_failures;
    uint64_t rtp_playout_delay_answer_rejections;
    uint64_t rtp_playout_delay_packets_stamped;
    uint64_t rtp_playout_delay_packets_bypassed;
    uint64_t rtp_playout_delay_transform_errors;
    uint64_t rtp_playout_delay_bytes_added;

    uint64_t peer_rebuilds;
    uint64_t peer_connects;
    /* Peers closed because the next video mode did not fit beside them. */
    uint64_t video_peer_releases;
    uint64_t sync_idr_requests;
    uint64_t sync_idr_issued;
    uint64_t sync_idr_committed;
    uint64_t sync_idr_successes;
    uint64_t sync_idr_failures;
    uint32_t sync_idr_pending_generation;
    uint32_t sync_idr_inflight_generation;
    uint32_t sync_idr_committed_generation;
    uint64_t signaling_messages_tx;
    uint64_t signaling_messages_rx;
    uint64_t signaling_messages_dropped;
    uint32_t signaling_resident_bytes;
    uint32_t signaling_resident_high_watermark;

    bool rpc_data_channel_open;
    bool hidrpc_data_channel_open;
    bool hidrpc_unreliable_ordered_data_channel_open;
    bool hidrpc_unreliable_unordered_data_channel_open;
    uint16_t rpc_data_channel_stream_id;
    uint16_t hidrpc_data_channel_stream_id;
    uint16_t hidrpc_unreliable_ordered_data_channel_stream_id;
    uint16_t hidrpc_unreliable_unordered_data_channel_stream_id;
    uint32_t hidrpc_active_generation;
    uint64_t data_channel_opens;
    uint64_t data_channel_closes;
    uint64_t data_channel_messages_rx;
    uint64_t data_channel_messages_tx;
    uint64_t data_channel_bytes_rx;
    uint64_t data_channel_bytes_tx;
    /* Lost messages; one that waited for the send buffer is not lost. */
    uint64_t data_channel_send_errors;
    /* Messages that waited for the send buffer, and the bytes waiting. */
    uint64_t data_channel_send_queued;
    uint32_t data_channel_send_queue_bytes;
    uint64_t data_channel_protocol_errors;
    uint64_t rpc_responses;
    uint64_t reboot_requests;
    uint64_t reboot_accepted;
    uint64_t reboot_busy;
    uint64_t reboot_response_sent;
    uint64_t reboot_will_reboot_sent;
    uint64_t reboot_send_failures;
    uint64_t reboot_executions;
    uint64_t rpc_event_notifications_queued;
    uint64_t rpc_event_notifications_coalesced;
    uint64_t rpc_event_notifications_sent;
    uint64_t rpc_event_notifications_dropped;
    uint64_t rpc_event_render_errors;
    uint64_t rpc_event_poll_errors;
    uint32_t rpc_event_pending_mask;
    uint64_t hidrpc_handshakes;
    bool hidrpc_handshake_complete;
    uint64_t hidrpc_state_notifications_queued;
    uint64_t hidrpc_state_notifications_coalesced;
    uint64_t hidrpc_state_notifications_sent;
    uint64_t hidrpc_state_notifications_dropped;
    uint32_t hidrpc_state_pending_mask;
    uint64_t hidrpc_unsupported_messages;
    uint64_t hidrpc_events_accepted;
    uint64_t hidrpc_events_dropped;
    uint64_t hidrpc_events_backpressured;
    uint64_t hidrpc_events_unsupported;
    uint64_t hidrpc_inactive_messages;

    webrtc_transport_stage_age_summary_t stage_age;
    webrtc_transport_network_timing_stats_t network_timing;

    bool memory_details_cached;
    uint64_t memory_details_sample_timestamp_us;
    uint64_t peer_memory_details_sample_timestamp_us;
    uint32_t internal_heap_free;
    uint32_t internal_heap_minimum;
    uint32_t internal_heap_largest;
    uint32_t internal_dma_heap_free;
    uint32_t internal_dma_heap_minimum;
    uint32_t internal_dma_heap_largest;
    uint32_t psram_free;
    uint32_t psram_minimum;
    uint32_t psram_largest;
    bool peer_memory_arena_active;
    uint32_t peer_memory_arena_capacity;
    uint32_t peer_memory_arena_free;
    uint32_t peer_memory_arena_minimum;
    uint32_t peer_memory_arena_largest;
    uint32_t peer_memory_arena_allocated_blocks;
    uint32_t peer_memory_arena_allocation_failures;
    uint32_t peer_memory_arena_begin_failures;
    uint32_t peer_memory_arena_leak_events;
    uint32_t peer_task_stack_high_watermark;
} webrtc_transport_stats_t;

/**
 * Start the local HTTP signaling server and peer worker.
 *
 * This is idempotent.  The network interface may be configured before or after
 * this call, but the browser cannot connect until it has an address.
 */
esp_err_t webrtc_transport_start(void);

/** Stop the worker/server and release all preallocated access-unit slots. */
esp_err_t webrtc_transport_stop(void);

/**
 * Create the opt-in browser-offerer/cloud signaling session.
 *
 * This does not open a WebSocket and is never selected by the default local
 * HTTP path.  The caller injects raw browser SDP/candidates below and receives
 * the generated answer/candidates through cfg->on_signal.
 */
esp_err_t webrtc_transport_cloud_session_create(
    const webrtc_transport_cloud_session_cfg_t *cfg,
    uint32_t *session_id, uint32_t *generation);

/** Inject a complete browser-created SDP offer for the exact generation. */
esp_err_t webrtc_transport_cloud_session_submit_offer(
    uint32_t session_id, uint32_t generation,
    const char *sdp, size_t size);

/** Inject one browser ICE candidate for the exact generation. */
esp_err_t webrtc_transport_cloud_session_submit_candidate(
    uint32_t session_id, uint32_t generation,
    const char *candidate, size_t size);

/** Close only the matching cloud generation; stale closes are rejected. */
esp_err_t webrtc_transport_cloud_session_close(
    uint32_t session_id, uint32_t generation);

/**
 * Set the only active capture mode.  A changed mode rebuilds the WebRTC peer
 * and therefore creates a new H.264 track/SDP generation.
 */
esp_err_t webrtc_transport_set_mode(uint16_t width, uint16_t height, uint16_t fps);

/** Clear the active mode and close the current peer connection. */
void webrtc_transport_clear_mode(void);

/**
 * Clear the mode and wait until no access unit is being filled or sent. The
 * peer, its DataChannels and the signaling session stay open.
 *
 * Use this before the video owner stops or rebuilds its pipeline.
 */
esp_err_t webrtc_transport_clear_mode_and_wait(uint32_t timeout_ms);

/**
 * While held, no peer opens, and a peer that is opening gives up. The video
 * owner holds this for a whole rebuild, so a browser that reconnects cannot
 * take the memory the next mode is admitted against. Holds nest.
 */
void webrtc_transport_hold_peer_open(bool hold);

/**
 * Close the peer and wait until its ICE/DTLS/SRTP/RTP memory is released.
 * The browser then reconnects. The video owner calls this only when the next
 * mode's memory does not fit beside the peer.
 */
esp_err_t webrtc_transport_release_peer_and_wait(uint32_t timeout_ms);

/**
 * Who asked for video to stop. Each source holds its own pause; delivery
 * resumes when none is held, so a browser closing its virtual media
 * dialog cannot restart video that an install paused.
 */
typedef enum {
    WEBRTC_VIDEO_PAUSE_UI = 1u << 0,  /**< setVideoStreamPaused RPC */
    WEBRTC_VIDEO_PAUSE_OTA = 1u << 1, /**< firmware download and install */
    /** downloadToStorage while it transfers the file to the TF card */
    WEBRTC_VIDEO_PAUSE_STORAGE_DOWNLOAD = 1u << 2,
    /** a browser upload while it holds the TF card's writer lease */
    WEBRTC_VIDEO_PAUSE_STORAGE_UPLOAD = 1u << 3,
} webrtc_video_pause_source_t;

/**
 * The sources whose hold also stops the host's sound. An upload holds video
 * off because it needs the whole link; sound beside it reaches the browser
 * with gaps, so it stops with video and starts again with it. No frame is
 * sent during the hold, the frames that wait are dropped, and the first
 * frame after it is of sound that the host plays then.
 */
#define WEBRTC_AUDIO_PAUSE_SOURCES ((uint8_t)WEBRTC_VIDEO_PAUSE_STORAGE_UPLOAD)

/**
 * Pause or resume encoded video delivery for one source without closing
 * RPC/HID channels. Resuming the last held source requests a fresh IDR so
 * the browser can decode immediately.
 */
esp_err_t webrtc_transport_set_video_paused_by(
    webrtc_video_pause_source_t source, bool paused);

/** webrtc_transport_set_video_paused_by() for WEBRTC_VIDEO_PAUSE_UI. */
esp_err_t webrtc_transport_set_video_paused(bool paused);

/**
 * Submit one complete H.264 Annex-B access unit.
 *
 * pts_90khz is the desired RTP timeline.  The implementation converts it to
 * esp_peer's millisecond input unit; esp_peer multiplies H.264 PTS by 90.
 * The call never waits for RTP, DTLS, SRTP, ICE, or a socket.  It performs at
 * most one bounded copy into a preallocated PSRAM slot.
 */
bool webrtc_transport_send_annexb(const uint8_t *data, size_t size,
                                  uint32_t pts_90khz, bool keyframe);

/** Metadata attached to one complete Annex-B H.264 access unit. */
typedef struct {
    uint32_t sequence;
    uint32_t capture_sequence;
    /** Producer frame timestamp used for media PTS; CSI frame-start when the
     * capture and encoder overlap, otherwise CSI end-of-frame. */
    int64_t capture_timestamp_us;
    /** CSI end-of-frame time used as the firmware stage-age origin. */
    int64_t capture_eof_timestamp_us;
    /** Zero when the active capture path does not use the asynchronous PPA. */
    int64_t ppa_done_timestamp_us;
    int64_t encode_start_timestamp_us;
    int64_t encode_done_timestamp_us;
    uint32_t rtp_timestamp_90khz;
    uint32_t sync_idr_generation;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
    bool keyframe;
    /** encode_start_timestamp_us precedes capture_eof_timestamp_us. */
    bool encode_overlapped_capture;
} app_h264_frame_meta_t;

/** Exclusive writable lease of one existing AU slot; no extra queue or pool.
 * Initialize to zero, do not copy/share an active lease, and finish exactly
 * once with commit/cancel after all encoder DMA has stopped using data.
 * The DMA producer must write back any CPU-written headers before DMA and
 * invalidate its DMA-written output before committing. A cancelled producer
 * must also leave CPU-written bytes clean before releasing its lease.
 * Stop/mode teardown waits for outstanding leases. */
typedef struct {
    uint8_t *data;       /**< 128-byte-aligned DMA output destination. */
    size_t capacity;     /**< Writable bytes, a multiple of 128. */
    uint64_t token;      /**< Opaque ownership token. */
    uint32_t generation;
    uint32_t slot;
} webrtc_transport_au_reservation_t;

/** Set the source mode and reserve a FREE AU slot without waiting. Failure
 * leaves a zero lease; callers may use the existing copying sink fallback.
 * An already-active lease is rejected unchanged so ownership is not lost.
 * Reservation itself does not count an offered encoded frame. */
bool webrtc_transport_reserve_access_unit(
    uint16_t width, uint16_t height, uint16_t fps,
    webrtc_transport_au_reservation_t *reservation);

/** Publish completed, CPU-visible Annex-B output with no payload copy.
 * Consumes/zeros the lease on success and failure. data must not be read or
 * written after this call; send completion returns the slot to the pool.
 * Commit rejects an output whose session/mode generation changed mid-encode. */
bool webrtc_transport_commit_access_unit(
    webrtc_transport_au_reservation_t *reservation, size_t size,
    const app_h264_frame_meta_t *meta);

/** Release an unused/failed output after DMA completion; consumes the lease. */
void webrtc_transport_cancel_access_unit(
    webrtc_transport_au_reservation_t *reservation);

/* OS Services service video: H.264 the computer's sender encodes, read from
 * USB by the CPU into the same slots. */

/** Start a new stream of up to 3840x2160 at up to 60 fps: a new video epoch
 * and a sync IDR request, even when the mode is unchanged. */
esp_err_t webrtc_transport_set_guest_mode(uint16_t width, uint16_t height,
                                          uint16_t fps);

/** Point the slots at buffers (bytes each, one per slot,
 * CONFIG_WEBRTC_TRANSPORT_AU_SLOTS of them), or back at the transport's own
 * with NULL. Clears the mode and waits up to timeout_ms until no slot is
 * filled or sent; then cleans the CPU's writes from the cache of each buffer
 * a slot leaves. On an error the slots keep their buffers. */
esp_err_t webrtc_transport_set_guest_buffers(uint8_t *const *buffers,
                                             size_t bytes, uint32_t timeout_ms);

/** Lease a FREE slot of the active mode without setting it. ESP_OK, or
 * ESP_ERR_INVALID_STATE without a viewer of that mode (drop the frame), or
 * ESP_ERR_NO_MEM while every slot is taken: the calling task is notified
 * (xTaskNotifyGive) when the peer releases one. */
esp_err_t webrtc_transport_reserve_guest_access_unit(
    uint16_t width, uint16_t height, uint16_t fps,
    webrtc_transport_au_reservation_t *reservation);

/** commit/cancel for a lease the CPU wrote (size or written bytes): the next
 * DMA lease of the slot writes those cache lines back first. */
bool webrtc_transport_commit_cpu_access_unit(
    webrtc_transport_au_reservation_t *reservation, size_t size,
    const app_h264_frame_meta_t *meta);
void webrtc_transport_cancel_cpu_access_unit(
    webrtc_transport_au_reservation_t *reservation, size_t written);

/** A frame of the stream was lost before it reached a slot: the viewer waits
 * for a sync IDR (webrtc_transport_take_sync_idr_request). */
void webrtc_transport_note_video_gap(void);

/** While set, the video state reports no error without a mode: the source is
 * changing. The caller reports the change (video_state_changed). */
void webrtc_transport_set_video_switching(bool switching);

/**
 * Optional encoded-frame sink implemented by the transport component.
 *
 * The data pointer is valid only for the duration of the call. The sink must
 * copy or enqueue the access unit without blocking the capture/encode task.
 * Return true when the frame was accepted and false when it was dropped.
 */
bool app_h264_frame_sink_submit(const uint8_t *data, size_t length,
                                const app_h264_frame_meta_t *meta);

/**
 * Consume the connected peer generation's one-shot sync-IDR request.
 *
 * This is intended for the task that owns the hardware encoder.  Each pending
 * attempt is consumed once and only while the peer still needs a decodable
 * IDR; the same logical generation may be reissued after a failed commit/send.
 */
bool webrtc_transport_take_sync_idr_request(uint32_t *peer_generation);

/**
 * Complete the encoder-production phase of an issued sync-IDR request.
 *
 * produced_and_accepted means the sink accepted an IDR access unit.  It does
 * not count as transport success: success is recorded only after that tagged
 * access unit reaches esp_peer_send_video() and the call returns OK.
 */
void webrtc_transport_complete_sync_idr_production(
    uint32_t peer_generation, bool produced_and_accepted);

/** Copy an atomic snapshot of firmware-side transport counters. */
/** Live counters/free/minimum with cached largest-block and block-count details. */
esp_err_t webrtc_transport_get_stats(webrtc_transport_stats_t *stats);
/* Called by the encoder worker between jobs. With adaptive set (quality Auto)
 * the REMB controller returns a target at or below the ceiling; otherwise the
 * ceiling is returned as a fixed rate and the controller state is cleared. */
uint32_t webrtc_transport_video_bitrate(uint32_t ceiling_bps, bool adaptive);
/* The same for service video, which the sender encodes: true when the
 * controller evaluated a new interval, with the target, the access-unit
 * send rate it measured over the interval (bit/s) and whether it cut.
 * Without adaptive the target is the ceiling and never a cut; the
 * intervals and their send rate are measured all the same. */
bool webrtc_transport_guest_bitrate(uint32_t ceiling_bps, bool adaptive,
                                    uint32_t *target_bps, uint32_t *sent_bps,
                                    bool *cut);
/** Explicit diagnostics only: refresh expensive heap walks. */
esp_err_t webrtc_transport_get_stats_detailed(webrtc_transport_stats_t *stats);
/** Start a new measuring window for the video interval histograms. */
void webrtc_transport_reset_video_intervals(void);

/* The cloud WebSocket as /webrtc/stats reports it. connection_epoch changes
 * with every new connection of one client, and client_rebuilds with every new
 * client, so an unchanged pair means the same connection stayed up. */
typedef struct {
    bool connected;
    uint32_t connection_epoch;
    uint64_t client_rebuilds;
} webrtc_transport_cloud_link_t;

/**
 * Report the cloud WebSocket in /webrtc/stats as "cloudConnected",
 * "cloudConnectionEpoch" and "cloudClientRebuilds" (null when the source is
 * missing or returns false). The cloud service depends on this component, so
 * main registers the source, before webrtc_transport_start().
 */
void webrtc_transport_set_cloud_link_source(
    bool (*source)(webrtc_transport_cloud_link_t *link));

/* The hardware H.264 encoder's times for the frames it encoded from HDMI
 * capture, since boot. A frame counts when the encoder made an access unit
 * of it, also when the transport then refused the unit; a failed encode does
 * not count.
 *
 * An encode time is the duration of esp_h264_enc_process() for one frame.
 * The call prepares the frame (rate control, slice header, DMA descriptors),
 * starts the hardware encoder and blocks until its frame-done interrupt;
 * then it invalidates the cache lines of the coded bytes. The whole-frame
 * cache write-back of the input is skipped (dma_cache_fastpath). The time is
 * therefore the hardware's time for the frame plus the wake-up of the
 * encoder task: it follows the video mode and says little about the CPU.
 *
 * A handoff is the time from the end of the frame's capture (the timestamp
 * taken in the CSI transfer-finished interrupt) to the start of that call.
 * The encoder task encodes one frame at a time, so a frame whose capture
 * ends during the encode of the frame before it waits for that encode and
 * its commit to the transport. The rest is the wake-up of the encoder task
 * and what it does before the call: the bitrate preference, the lease of an
 * output slot (no wait, no copy) and the sync IDR request. A frame that has
 * waited longer than one frame period is dropped, not encoded, and a frame
 * without a free output slot is skipped; neither has a handoff here.
 *
 * The counts and sums only grow, so a reader takes the difference between
 * two reports. Each *_max_us is the longest time of the current 2-second
 * window and of the window before it, and 0 without such a frame in them: a
 * reader that polls at least every 2 seconds sees every frame's time. */
typedef struct {
    uint32_t p_frames;
    uint64_t p_encode_us;
    uint32_t p_encode_max_us;
    uint32_t idr_frames;
    uint64_t idr_encode_us;
    uint32_t idr_encode_max_us;
    /* Sum and longest over the P and IDR frames together. */
    uint64_t handoff_us;
    uint32_t handoff_max_us;
} webrtc_transport_video_encode_t;

/**
 * Report the encoder's times in /webrtc/stats (also ?bitrate=1) as
 * "videoEncode", null without a source. The video component depends on this
 * one, so main registers the source, before webrtc_transport_start() starts
 * the HTTP server. The source is called from the HTTP server task.
 */
void webrtc_transport_set_video_encode_source(
    void (*source)(webrtc_transport_video_encode_t *encode));

/* The HDMI receiver chip (TC358743) as the capture task read it last. The
 * capture task reads the chip for its own work; a stats request reads only
 * these copies, never the chip.
 *
 * sys_status is the chip's SYS_STATUS register, which the capture task reads
 * at most every 500 ms in every state of the capture, also during service
 * video, video sleep and a hidden host display, and once a second while it
 * waits for a signal; a read waits while the task is in a longer call, such
 * as the 2 s of an EDID write. The five flags are its
 * bits: the source's +5 V, its TMDS clock, the lock of the receiver's PHY
 * PLL on that clock, the PHY's signal detection, and the video sync.
 *
 * detect_result is the result of the task's last timing detection, as
 * esp_err_to_name() gives it: "ESP_OK" with a stable mode, whose size and
 * frame rate are in detected_*; "ESP_ERR_NOT_FOUND" without TMDS clock or
 * sync, or with HPD low; "ESP_ERR_INVALID_RESPONSE" for timings outside
 * 640x480 to 1920x1080, which detected_* then has. The task detects every
 * 500 ms while it captures and once a second while it waits for a signal; it
 * does not detect during service video, video sleep or a hidden host
 * display, so detect_age_ms grows there. */
typedef struct {
    /* False until the first read of SYS_STATUS; the fields below it to
     * status_age_ms are zero then. */
    bool status_read;
    uint8_t sys_status;
    bool cable_5v;
    bool tmds_clock;
    bool phy_pll;
    bool phy_scdt;
    bool sync;
    uint64_t status_age_ms;
    /* False until the first detection; the fields below are unset then. */
    bool detect_ran;
    const char *detect_result;
    uint16_t detected_width;
    uint16_t detected_height;
    uint16_t detected_fps;
    uint64_t detect_age_ms;
} webrtc_transport_hdmi_receiver_t;

/**
 * Report the HDMI receiver's state in /webrtc/stats (also ?bitrate=1) as
 * "hdmiReceiver", null without a source. main registers the source before
 * webrtc_transport_start() starts the HTTP server. The source is called from
 * the HTTP server task.
 */
void webrtc_transport_set_hdmi_receiver_source(
    void (*source)(webrtc_transport_hdmi_receiver_t *receiver));

struct cJSON;
/**
 * Report OS Services service video in /webrtc/stats (also ?bitrate=1) as
 * "guestVideo": the object source fills, or null without a source. Set by
 * OS Services at init; called from the HTTP server task.
 */
void webrtc_transport_set_guest_video_source(
    bool (*source)(struct cJSON *object));

/* OS Services hook: the OS Services state that videoInputState carries has
 * changed. Queues the event for the current peer and wakes the peer task,
 * which renders it with the state of that moment; it does not wait. Callable
 * from any task (not an ISR), also before webrtc_transport_start() or with
 * no peer, when it does nothing. */
void webrtc_transport_video_state_changed(void);

/**
 * Send one pre-rendered JSON-RPC notification on the RPC data channel.
 *
 * Best-effort, callable from any task (OTA progress pushes use it): when no
 * peer session or RPC channel is currently open the call reports
 * ESP_ERR_INVALID_STATE instead of queueing.  Sending is serialized through
 * the same peer handle the request/response path uses.
 */
/* HID worker hook: a keyboard report was accepted by the host. Wakes the peer
 * task so keysDownState goes out before the next video frame. */
void webrtc_transport_keyboard_changed(void *ctx);

esp_err_t webrtc_transport_send_rpc_notification(const uint8_t *data,
                                                 size_t size);

/**
 * Mount a file of the TF card and give the host its drive. This is the
 * function of the RPC mountWithStorage, for a caller without a session, such
 * as an MQTT command: a mount that the host cannot be given is taken back.
 * It blocks while the host enumerates the drive again, for up to the USB
 * owner's six-second deadline. Returns whether the image is mounted.
 */
bool webrtc_transport_mount_storage_image(const char *filename,
                                          bool cdrom_image);

#ifdef __cplusplus
}
#endif

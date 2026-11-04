#include "jetkvm_audio.h"
#include "jetkvm_access.h"
#include "rpc_worker.h"
/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "webrtc_transport.h"

#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "esp_app_desc.h"
#include "esp_cache.h"
#include "esp_freertos_hooks.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "webrtc_transport_private.h"
#include "controlled_session_policy.h"
#include "device_contract.h"
#include "jetkvm_control.h"
#include "jetkvm_device_state.h"
#include "jetkvm_hid_input.h"
#include "jetkvm_hid_profiles.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_settings.h"
#include "jetkvm_usb_service.h"
#include "jetkvm_video_quality.h"
#include "jetkvm_virtual_media.h"
#include "jetkvm_wol.h"
#include "network_timing.h"
#include "peer_memory_arena.h"
#include "playout_delay.h"
#include "reboot_flow.h"
#include "rpc_events.h"
#include "rtcp_feedback.h"
#include "web_app.h"
#include "signaling_budget.h"
#include "webrtc_transport_ctx_internal.h"
#include "webrtc_rpc_adapters_internal.h"
#include "webrtc_data_channels_internal.h"
#include "webrtc_sessions_internal.h"



_Static_assert(JETKVM_CONTROL_WOL_MAX_DEVICES == JETKVM_WOL_MAX_DEVICES,
               "control and storage WOL list limits must match");
_Static_assert(JETKVM_CONTROL_WOL_NAME_CAPACITY == JETKVM_WOL_NAME_CAPACITY,
               "control and storage WOL name capacities must match");
_Static_assert(JETKVM_CONTROL_WOL_MAC_CAPACITY == JETKVM_WOL_MAC_CAPACITY,
               "control and storage WOL MAC capacities must match");
_Static_assert(JETKVM_CONTROL_WOL_BROADCAST_CAPACITY ==
                   JETKVM_WOL_BROADCAST_CAPACITY,
               "control and storage WOL broadcast capacities must match");









#define AU_SLOT_ALLOCATION_BYTES \
    ((CONFIG_WEBRTC_TRANSPORT_MAX_AU_BYTES + 127U) & ~(size_t)127U)
#define RTP_HEADER_MIN_SIZE 12
#define STAGE_AGE_MAX_US (60ULL * 1000ULL * 1000ULL)

/* With a terminal and a file transfer every pass of the peer task finds
 * work, so its wait returns at once and the task never leaves its core. The
 * idle task of that core then gets no time, and after 5 s the task watchdog
 * reports it from its interrupt handler. The report keeps interrupts off for
 * about 100 ms, HDMI capture misses the end of a frame, and video stops for
 * 2 s. When the idle task of the peer task's core has not run for this long,
 * the peer task leaves the core for one tick, in each pass until it has. */
#define PEER_TASK_IDLE_STARVED_US (1000 * 1000)

#define JETKVM_REBOOT_TASK_STACK 3072U
#define JETKVM_REBOOT_TASK_PRIORITY 1U
#define JETKVM_MEMORY_RETRY_DELAY_MS 30000U
_Static_assert(CONFIG_WEBRTC_TRANSPORT_DATA_CHANNEL_RECEIVE_CACHE_BYTES ==
                   JETKVM_MEMORY_SCTP_RECEIVE_MAX_BYTES,
               "SCTP receive cache must match the ingress ledger");

_Static_assert(STAGE_AGE_MAX_US <= UINT32_MAX,
               "stage ages must fit the exported uint32_t maximum");
_Static_assert(WEBRTC_RPC_EVENT_COUNT < 32,
               "RPC event pending bits must fit uint32_t");
_Static_assert(WEBRTC_HIDRPC_STATE_COUNT < 32,
               "HIDRPC state pending bits must fit uint32_t");
_Static_assert(JETKVM_HIDRPC_TYPE_KEYBOARD_LED_STATE == 0x32U &&
                   JETKVM_HIDRPC_TYPE_KEYS_DOWN_STATE == 0x33U &&
                   JETKVM_HIDRPC_TYPE_KEYBOARD_MACRO_STATE == 0x34U,
               "HIDRPC state renderer and control protocol must agree");
_Static_assert(sizeof(jetkvm_virtual_media_file_list_t) <=
                   JETKVM_MEMORY_RPC_SCRATCH_BYTES,
               "RPC scratch must hold the bounded virtual-media file list");

/* SDP rewriting is control-plane work.  Bind every owned result to the
 * private arena so a 32 KiB offer/answer copy cannot punch holes in the
 * general PSRAM region reserved for video rebuild and storage ingress. */
static const webrtc_playout_delay_allocator_t s_playout_delay_allocator = {
    .alloc = jetkvm_memory_policy_control_alloc,
    .free = jetkvm_memory_policy_control_free,
};


static const char *TAG = "webrtc_transport";
static transport_ctx_t s_ctx = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};
static bool s_dtls_cert_cache_primed;

transport_ctx_t *webrtc_transport_context(void)
{
    return &s_ctx;
}

static void peer_task(void *arg);
static bool receive_rest_after_key(transport_ctx_t *ctx, bool refused,
                                   uint32_t attempts_left,
                                   int64_t *keyboard_us);
static void reboot_task(void *arg);
static esp_err_t webrtc_transport_stop_internal(bool startup_failure);


static esp_err_t prime_dtls_cert_cache(void)
{
    if (s_dtls_cert_cache_primed) {
        return ESP_OK;
    }

    /* In the pinned IDF 6 path the generated PSA key has a randomized-ECDSA
     * policy, while TLS is configured for deterministic ECDSA.  The first
     * peer therefore cannot select that wrapped key, but later peers work
     * because the cached PEM is re-imported with the compatible ANY_HASH
     * policy.  Prime the cache before accepting signaling; this is also the
     * startup order used by the esp_peer peer_demo. */
    int64_t started_us = esp_timer_get_time();
    int ret = esp_peer_pre_generate_cert();
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "failed to pre-generate DTLS certificate: %d", ret);
        return ESP_FAIL;
    }
    s_dtls_cert_cache_primed = true;
    ESP_LOGI(TAG, "DTLS certificate cache primed in %" PRId64 " ms",
             (esp_timer_get_time() - started_us) / 1000);
    return ESP_OK;
}

static bool supported_mode(uint16_t width, uint16_t height, uint16_t fps)
{
    return (width == 1920 && height == 1080 && fps == 30) ||
           (width == 1280 && height == 720 && (fps == 30 || fps == 60)) ||
           (fps == 30 && ((width == 1024 && height == 768) ||
                          (width == 1280 && (height == 800 || height == 1024))));
}

static bool fail_sync_idr_locked(transport_ctx_t *ctx, uint32_t generation,
                                 bool requeue)
{
    bool active = false;
    if (generation != 0 && ctx->sync_idr_inflight_generation == generation) {
        ctx->sync_idr_inflight_generation = 0;
        ctx->stats.sync_idr_inflight_generation = 0;
        active = true;
    }
    if (generation != 0 && ctx->sync_idr_committed_generation == generation) {
        ctx->sync_idr_committed_generation = 0;
        ctx->stats.sync_idr_committed_generation = 0;
        active = true;
    }
    if (!active) {
        return false;
    }
    ctx->stats.sync_idr_failures++;
    if (requeue && ctx->started && ctx->peer_connected &&
        ctx->waiting_for_sync && ctx->peer &&
        generation == ctx->active_peer_generation) {
        ctx->sync_idr_pending_generation = generation;
        ctx->stats.sync_idr_pending_generation = generation;
    }
    return true;
}

uint32_t next_generation_locked(transport_ctx_t *ctx)
{
    /* Stop accepting HID at the same lock-protected point that makes the old
     * signaling generation stale.  The owning caller performs the bridge
     * invalidate/release after leaving this spinlock. */
    ctx->active_hid_generation = 0;
    ctx->stats.hidrpc_active_generation = 0;
    ctx->sync_idr_pending_generation = 0;
    ctx->stats.sync_idr_pending_generation = 0;
    clear_rpc_events_locked(ctx);
    clear_hidrpc_state_notifications_locked(ctx);
    if (webrtc_reboot_slot_cancel_generation(
            &ctx->reboot_slot, ctx->generation)) {
        ctx->reboot_executing = false;
    }
    uint32_t active_sync_idr = ctx->sync_idr_inflight_generation ?
        ctx->sync_idr_inflight_generation :
        ctx->sync_idr_committed_generation;
    fail_sync_idr_locked(ctx, active_sync_idr, false);
    ctx->generation++;
    if (ctx->generation == 0) {
        ctx->generation++;
        ctx->sync_idr_last_requested_generation = 0;
    }
    ctx->stats.generation = ctx->generation;
    return ctx->generation;
}

void clear_cloud_invalidation_locked(transport_ctx_t *ctx)
{
    ctx->cloud_invalidation_pending = false;
    ctx->cloud_invalidation_session_id = 0;
    ctx->cloud_invalidated_generation = 0;
    ctx->cloud_replacement_generation = 0;
}

static void schedule_cloud_invalidation_locked(
    transport_ctx_t *ctx, uint32_t invalidated_generation,
    uint32_t replacement_generation)
{
    if (!ctx->session_active ||
        ctx->session_role != WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED ||
        invalidated_generation == 0 || replacement_generation == 0 ||
        invalidated_generation == replacement_generation) {
        return;
    }
    if (!ctx->cloud_invalidation_pending ||
        ctx->cloud_invalidation_session_id != ctx->session_id) {
        ctx->cloud_invalidated_generation = invalidated_generation;
    }
    ctx->cloud_invalidation_pending = true;
    ctx->cloud_invalidation_session_id = ctx->session_id;
    ctx->cloud_replacement_generation = replacement_generation;
}

static void dispatch_pending_cloud_invalidation(transport_ctx_t *ctx)
{
    webrtc_transport_cloud_lifecycle_cb_t callback = NULL;
    void *callback_ctx = NULL;
    webrtc_transport_cloud_lifecycle_event_t event = { 0 };
    bool dispatch = false;

    portENTER_CRITICAL(&ctx->lock);
    if (ctx->cloud_invalidation_pending) {
        bool current = ctx->started && ctx->session_active &&
            ctx->session_role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED &&
            ctx->session_id == ctx->cloud_invalidation_session_id &&
            ctx->generation == ctx->cloud_replacement_generation;
        if (!current || !ctx->cloud_lifecycle_cb) {
            clear_cloud_invalidation_locked(ctx);
        } else if (webrtc_cloud_callback_gate_try_begin(
                       &ctx->cloud_callback_gate, ctx->session_id,
                       ctx->generation)) {
            callback = ctx->cloud_lifecycle_cb;
            callback_ctx = ctx->cloud_signal_ctx;
            event = (webrtc_transport_cloud_lifecycle_event_t) {
                .type = WEBRTC_TRANSPORT_CLOUD_LIFECYCLE_GENERATION_INVALIDATED,
                .session_id = ctx->session_id,
                .generation = ctx->cloud_invalidated_generation,
                .replacement_generation = ctx->generation,
            };
            clear_cloud_invalidation_locked(ctx);
            dispatch = true;
        }
    }
    portEXIT_CRITICAL(&ctx->lock);

    if (!dispatch) {
        return;
    }
    callback(&event, callback_ctx);
    portENTER_CRITICAL(&ctx->lock);
    webrtc_cloud_callback_gate_end(
        &ctx->cloud_callback_gate, event.session_id,
        event.replacement_generation);
    portEXIT_CRITICAL(&ctx->lock);
}

static void dispatch_cloud_peer_ready(transport_ctx_t *ctx,
                                      uint32_t session_id,
                                      uint32_t generation)
{
    webrtc_transport_cloud_lifecycle_cb_t callback = NULL;
    void *callback_ctx = NULL;
    webrtc_transport_cloud_lifecycle_event_t event = {
        .type = WEBRTC_TRANSPORT_CLOUD_LIFECYCLE_PEER_READY,
        .session_id = session_id,
        .generation = generation,
    };
    portENTER_CRITICAL(&ctx->lock);
    bool current = ctx->started && ctx->session_active && ctx->peer &&
        ctx->session_role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED &&
        ctx->active_peer_role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED &&
        ctx->session_id == session_id && ctx->generation == generation &&
        ctx->active_peer_session == session_id &&
        ctx->active_peer_generation == generation;
    if (current && ctx->cloud_lifecycle_cb &&
        webrtc_cloud_callback_gate_try_begin(
            &ctx->cloud_callback_gate, session_id, generation)) {
        callback = ctx->cloud_lifecycle_cb;
        callback_ctx = ctx->cloud_signal_ctx;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (!callback) {
        return;
    }
    callback(&event, callback_ctx);
    portENTER_CRITICAL(&ctx->lock);
    webrtc_cloud_callback_gate_end(
        &ctx->cloud_callback_gate, session_id, generation);
    portEXIT_CRITICAL(&ctx->lock);
}

static void reset_stage_age_locked(transport_ctx_t *ctx)
{
    uint32_t epoch = ctx->stats.stage_age.epoch + 1U;
    if (epoch == 0) {
        epoch = 1;
    }
    memset(&ctx->stats.stage_age, 0, sizeof(ctx->stats.stage_age));
    ctx->stats.stage_age.epoch = epoch;
    webrtc_network_timing_reset(epoch);
}

static void record_stage_age_locked(
    webrtc_transport_stage_age_summary_t *summary,
    webrtc_transport_stage_age_stats_t *accumulator,
    int64_t capture_eof_us, int64_t event_us)
{
    /* A zero event is intentionally unavailable, for example direct CSI 420
     * has no asynchronous PPA completion timestamp. */
    if (event_us == 0) {
        return;
    }
    if (capture_eof_us <= 0 || event_us < capture_eof_us) {
        summary->invalid_timestamps++;
        return;
    }
    uint64_t age_us = (uint64_t)event_us - (uint64_t)capture_eof_us;
    if (age_us > STAGE_AGE_MAX_US) {
        summary->invalid_timestamps++;
        return;
    }
    if (accumulator->count == UINT64_MAX ||
        UINT64_MAX - accumulator->total_us < age_us) {
        summary->accumulator_overflows++;
        return;
    }
    accumulator->count++;
    accumulator->total_us += age_us;
    if (age_us > accumulator->max_us) {
        accumulator->max_us = (uint32_t)age_us;
    }
}

static bool stage_time_reversed(int64_t earlier_us, int64_t later_us)
{
    return earlier_us > 0 && later_us > 0 && later_us < earlier_us;
}

static void aggregate_active_trace_locked(transport_ctx_t *ctx,
                                          int64_t send_video_return_us)
{
    active_send_trace_t *trace = &ctx->active_send_trace;
    webrtc_transport_stage_age_summary_t *summary = &ctx->stats.stage_age;
    if (!trace->active) {
        return;
    }
    if (trace->telemetry_epoch == 0 ||
        trace->telemetry_epoch != summary->epoch) {
        summary->epoch_mismatches++;
        return;
    }

    bool order_error =
        stage_time_reversed(trace->capture_eof_us, trace->ppa_done_us) ||
        stage_time_reversed(trace->capture_eof_us, trace->encode_start_us) ||
        stage_time_reversed(trace->ppa_done_us, trace->encode_start_us) ||
        stage_time_reversed(trace->encode_start_us, trace->encode_done_us) ||
        stage_time_reversed(trace->encode_done_us, trace->au_copy_ready_us) ||
        stage_time_reversed(trace->au_copy_ready_us, trace->peer_take_us) ||
        stage_time_reversed(trace->peer_take_us, trace->first_rtp_us) ||
        stage_time_reversed(trace->first_rtp_us, trace->marker_us) ||
        stage_time_reversed(trace->peer_take_us, trace->marker_us) ||
        stage_time_reversed(trace->first_rtp_us, send_video_return_us) ||
        stage_time_reversed(trace->marker_us, send_video_return_us) ||
        stage_time_reversed(trace->peer_take_us, send_video_return_us);
    if (order_error) {
        summary->order_errors++;
        return;
    }

    record_stage_age_locked(summary, &summary->capture_to_ppa_done,
                            trace->capture_eof_us, trace->ppa_done_us);
    record_stage_age_locked(summary, &summary->capture_to_encode_start,
                            trace->capture_eof_us, trace->encode_start_us);
    record_stage_age_locked(summary, &summary->capture_to_encode_done,
                            trace->capture_eof_us, trace->encode_done_us);
    record_stage_age_locked(summary, &summary->capture_to_au_copy_ready,
                            trace->capture_eof_us, trace->au_copy_ready_us);
    record_stage_age_locked(summary, &summary->capture_to_peer_take,
                            trace->capture_eof_us, trace->peer_take_us);
    record_stage_age_locked(summary, &summary->capture_to_send_video_return,
                            trace->capture_eof_us, send_video_return_us);
    record_stage_age_locked(summary, &summary->capture_to_first_rtp,
                            trace->capture_eof_us, trace->first_rtp_us);
    record_stage_age_locked(summary, &summary->capture_to_rtp_marker,
                            trace->capture_eof_us, trace->marker_us);
    if (trace->first_rtp_us == 0) {
        summary->missing_first_rtp++;
    }
    if (trace->marker_us == 0) {
        summary->missing_rtp_marker++;
    }
}

void webrtc_transport_keyboard_changed(void *unused)
{
    (void)unused;
    transport_ctx_t *ctx = &s_ctx;
    atomic_store(&ctx->rpc_event_hid_dirty, true);
    notify_peer_task(ctx);
}

bool webrtc_transport_keyboard_change_pending(void)
{
    return atomic_load(&s_ctx.rpc_event_hid_dirty);
}

/* Calls of webrtc_transport_datagram_arrived() that may hold the peer
 * task's handle. The TCP/IP task makes them for every UDP datagram and is
 * not stopped with the transport, so the peer task waits for this count
 * before it deletes itself. */
static atomic_uint s_datagram_wakers;

void webrtc_transport_datagram_arrived(void)
{
    atomic_fetch_add(&s_datagram_wakers, 1U);
    notify_peer_task(&s_ctx);
    atomic_fetch_sub(&s_datagram_wakers, 1U);
}

void notify_peer_task(transport_ctx_t *ctx)
{
    TaskHandle_t task = NULL;
    portENTER_CRITICAL(&ctx->lock);
    task = ctx->task;
    portEXIT_CRITICAL(&ctx->lock);
    if (task) {
        xTaskNotifyGive(task);
    }
}

static bool starts_with_annexb(const uint8_t *data, size_t size)
{
    return data && ((size >= 4 && data[0] == 0 && data[1] == 0 &&
                     data[2] == 0 && data[3] == 1) ||
                    (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1));
}

static void inspect_annexb(const uint8_t *data, size_t size,
                           bool *has_idr, bool *has_sps, bool *has_pps)
{
    *has_idr = false;
    *has_sps = false;
    *has_pps = false;
    for (size_t i = 0; i + 4 < size; ++i) {
        size_t nal_offset = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            nal_offset = i + 3;
        } else if (i + 4 < size && data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1) {
            nal_offset = i + 4;
        }
        if (!nal_offset || nal_offset >= size) {
            continue;
        }
        uint8_t nal_type = data[nal_offset] & 0x1f;
        *has_idr |= nal_type == 5;
        *has_sps |= nal_type == 7;
        *has_pps |= nal_type == 8;
        if (*has_idr && *has_sps && *has_pps) {
            return;
        }
        i = nal_offset;
    }
}

static void flush_ready_slots_locked(transport_ctx_t *ctx)
{
    for (size_t i = 0; i < CONFIG_WEBRTC_TRANSPORT_AU_SLOTS; ++i) {
        /* An in-progress esp_peer send still reads SENDING storage. Pause or
         * rebuild can discard queued AUs, but only the sender releases its
         * slot; a rapid resume may otherwise lend the same bytes to DMA. */
        if (ctx->slots[i].state == AU_SLOT_READY) {
            ctx->slots[i].state = AU_SLOT_FREE;
            ctx->slots[i].size = 0;
            ctx->slots[i].sync_idr_generation = 0;
        }
    }
}

static bool slot_is_decodable_sync(const au_slot_t *slot, uint32_t generation,
                                   uint32_t video_epoch)
{
    return slot->generation == generation &&
           slot->video_epoch == video_epoch && slot->keyframe &&
           slot->has_idr && slot->has_sps && slot->has_pps &&
           slot->sync_idr_generation == generation;
}

static bool slot_needs_reference_sync(const transport_ctx_t *ctx,
                                      const au_slot_t *slot)
{
    return slot->generation == ctx->active_peer_generation &&
           !ctx->waiting_for_sync &&
           ctx->last_sent_au_generation == ctx->active_peer_generation &&
           slot->order != ctx->last_sent_au_order + 1 &&
           !(slot->keyframe && slot->has_idr && slot->has_sps && slot->has_pps);
}

static void request_video_recovery_locked(transport_ctx_t *ctx)
{
    if (ctx->waiting_for_sync) {
        return;
    }
    ctx->waiting_for_sync = true;
    ctx->stats.waiting_for_sync_frame = true;
    ctx->sync_idr_pending_generation = ctx->active_peer_generation;
    ctx->stats.sync_idr_pending_generation = ctx->active_peer_generation;
    ctx->stats.sync_idr_requests++;
}

static void handle_video_pli(transport_ctx_t *ctx, uint32_t generation)
{
    int64_t now_us = esp_timer_get_time();
    bool requested = false;
    portENTER_CRITICAL(&ctx->lock);
    /* Coalesce feedback while an IDR is pending; allow at most one feedback
     * request per configured half-second GOP in this peer generation. */
    uint32_t fps = ctx->fps ? ctx->fps : 30;
    int64_t gop_us = (int64_t)(fps / 2 ? fps / 2 : 1) * 1000000 / fps;
    if (ctx->started && ctx->session_active && ctx->peer &&
        ctx->task == xTaskGetCurrentTaskHandle() && generation != 0 &&
        ctx->peer_connected && !ctx->closing_peer && !ctx->rebuild_requested &&
        !ctx->video_paused && generation == ctx->generation &&
        generation == ctx->active_peer_generation && !ctx->waiting_for_sync &&
        (ctx->last_pli_generation != generation ||
         now_us - ctx->last_pli_request_us >= gop_us)) {
        ctx->last_pli_generation = generation;
        ctx->last_pli_request_us = now_us;
        request_video_recovery_locked(ctx);
        requested = true;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (requested) {
        ESP_LOGI(TAG, "PLI requested sync IDR generation=%" PRIu32, generation);
    }
}

void webrtc_transport_on_rtcp(const uint8_t *data, size_t size)
{
    transport_ctx_t *ctx = &s_ctx;
    uint32_t generation = 0;
    uint32_t ssrc = 0;
    portENTER_CRITICAL(&ctx->lock);
    bool current = ctx->started && ctx->session_active && ctx->peer_connected &&
        ctx->peer && !ctx->closing_peer && !ctx->rebuild_requested &&
        !ctx->video_paused && ctx->task == xTaskGetCurrentTaskHandle() &&
        ctx->active_peer_generation == ctx->generation &&
        ctx->video_ssrc_generation == ctx->active_peer_generation;
    if (current) {
        generation = ctx->active_peer_generation;
        ssrc = ctx->video_ssrc;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (current && webrtc_rtcp_has_video_pli(data, size, ssrc)) {
        handle_video_pli(ctx, generation);
    }
    webrtc_rtcp_report_block_t block;
    if (current && webrtc_rtcp_find_report_block(data, size, ssrc, &block)) {
        portENTER_CRITICAL(&ctx->lock);
        if (ctx->active_peer_generation == generation) {
            webrtc_bitrate_loss(&ctx->bitrate_controller, block.cumulative_lost,
                                block.highest_sequence);
            ctx->stats.video_rtcp_loss_reports++;
            ctx->stats.video_rtcp_cumulative_lost = block.cumulative_lost;
            ctx->stats.video_rtcp_highest_sequence = block.highest_sequence;
        }
        portEXIT_CRITICAL(&ctx->lock);
    }
}

/* Encoded P frames depend on earlier encoded pictures. Only raw capture
 * buffers may use latest-only selection; access units must remain in order. */
static int reserve_slot_locked(transport_ctx_t *ctx)
{
    for (size_t i = 0; i < CONFIG_WEBRTC_TRANSPORT_AU_SLOTS; ++i) {
        if (ctx->slots[i].state == AU_SLOT_FREE) {
            return (int)i;
        }
    }
    return -1;
}

/* The video is service video. guest_mode outlives its mode, so a cleared
 * mode does not count. */
static bool service_video_locked(const transport_ctx_t *ctx)
{
    return ctx->mode_valid && ctx->guest_mode;
}

/* between_steps: take the frame only when it cannot wait and its send is
 * short. A keyframe and a service video frame stay READY, and with them
 * every frame behind. */
static int take_next_ready_slot(transport_ctx_t *ctx, bool between_steps)
{
    int selected = -1;
    uint64_t oldest_order = UINT64_MAX;
    portENTER_CRITICAL(&ctx->lock);
    for (size_t i = 0; i < CONFIG_WEBRTC_TRANSPORT_AU_SLOTS; ++i) {
        if ((ctx->slots[i].state == AU_SLOT_READY ||
             ctx->slots[i].state == AU_SLOT_FILLING) &&
            ctx->slots[i].order < oldest_order) {
            selected = (int)i;
            oldest_order = ctx->slots[i].order;
        }
    }
    /* A concurrent producer may still be copying an earlier access unit.
     * Wait for its notification instead of overtaking it. */
    if (selected >= 0 && ctx->slots[selected].state == AU_SLOT_READY &&
        !(between_steps && (ctx->slots[selected].keyframe ||
                            service_video_locked(ctx)))) {
        ctx->slots[selected].state = AU_SLOT_SENDING;
    } else {
        selected = -1;
    }
    portEXIT_CRITICAL(&ctx->lock);
    return selected;
}

/* esp_peer sends a whole access unit in one call, and a 1080p IDR holds this
 * task for about 60 ms. Read the input that arrived while the task slept
 * before a frame starts, so a HID keepalive does not wait behind the frame and
 * miss the 100 ms auto-release deadline of a held key. Returns the time spent
 * in the vendor main loop, in microseconds, and adds the time of the key
 * reports sent between its parts to *keyboard_us. */
static int64_t receive_before_frame(transport_ctx_t *ctx, int64_t *keyboard_us)
{
    bool ready = false;
    portENTER_CRITICAL(&ctx->lock);
    for (size_t i = 0; i < CONFIG_WEBRTC_TRANSPORT_AU_SLOTS; ++i) {
        ready |= ctx->slots[i].state == AU_SLOT_READY;
    }
    ready &= ctx->peer_connected && ctx->peer != NULL;
    portEXIT_CRITICAL(&ctx->lock);
    if (!ready) {
        return 0;
    }
    int64_t start_us = esp_timer_get_time();
    webrtc_peer_udp_rx_slice_begin(true, 8U);
    (void)esp_peer_main_loop(ctx->peer);
    uint32_t attempts_left = 0U;
    const bool refused = webrtc_peer_udp_rx_slice_end(&attempts_left);
    int64_t reports_us = 0;
    (void)receive_rest_after_key(ctx, refused, attempts_left, &reports_us);
    *keyboard_us += reports_us;
    return esp_timer_get_time() - start_us - reports_us;
}

/* Returns the task that waits for a slot, to be woken after the lock. */
static TaskHandle_t free_sending_slot_locked(transport_ctx_t *ctx,
                                             au_slot_t *slot)
{
    if (slot->state != AU_SLOT_SENDING) {
        return NULL;
    }
    slot->state = AU_SLOT_FREE;
    slot->size = 0;
    slot->sync_idr_generation = 0;
    TaskHandle_t waiter = ctx->slot_waiter;
    ctx->slot_waiter = NULL;
    return waiter;
}

static void release_sending_slot(transport_ctx_t *ctx, int index)
{
    TaskHandle_t waiter = NULL;
    portENTER_CRITICAL(&ctx->lock);
    if (index >= 0 && index < CONFIG_WEBRTC_TRANSPORT_AU_SLOTS) {
        waiter = free_sending_slot_locked(ctx, &ctx->slots[index]);
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (waiter) {
        xTaskNotifyGive(waiter);
    }
}

/* FILLING is producer-owned across session rebuilds. Stop waits for the
 * existing submitters count before freeing the pool. The order token prevents
 * an old reservation from completing a later reuse of the same slot. */
static bool reservation_matches_locked(
    const transport_ctx_t *ctx,
    const webrtc_transport_au_reservation_t *reservation)
{
    if (!reservation->data || !reservation->token ||
        reservation->slot >= CONFIG_WEBRTC_TRANSPORT_AU_SLOTS) {
        return false;
    }
    const au_slot_t *slot = &ctx->slots[reservation->slot];
    return slot->state == AU_SLOT_FILLING &&
           slot->data == reservation->data &&
           slot->order == reservation->token &&
           slot->generation == reservation->generation;
}

static bool finish_access_unit(
    const webrtc_transport_au_reservation_t *reservation, size_t size,
    uint32_t pts_90khz, bool keyframe, const app_h264_frame_meta_t *meta,
    bool valid_payload)
{
    transport_ctx_t *ctx = &s_ctx;
    bool has_idr = false, has_sps = false, has_pps = false;
    bool trace_available = meta && meta->capture_eof_timestamp_us > 0;
    if (valid_payload && keyframe) {
        inspect_annexb(reservation->data, size, &has_idr, &has_sps, &has_pps);
    }
    int64_t au_copy_ready_us = trace_available ? esp_timer_get_time() : 0;
    bool accepted = false;
    portENTER_CRITICAL(&ctx->lock);
    if (!reservation_matches_locked(ctx, reservation)) {
        portEXIT_CRITICAL(&ctx->lock);
        return false;
    }
    au_slot_t *slot = &ctx->slots[reservation->slot];
    uint32_t generation = reservation->generation;
    uint32_t telemetry_epoch = slot->telemetry_epoch;
    uint64_t order = reservation->token;
    if (valid_payload && ctx->started && ctx->session_active &&
        ctx->mode_valid && !ctx->video_paused &&
        generation == ctx->generation &&
        slot->video_epoch == ctx->video_epoch &&
        (!meta || (meta->width == ctx->width && meta->height == ctx->height &&
                   meta->fps == ctx->fps))) {
        slot->size = size;
        slot->pts_90khz = pts_90khz;
        slot->generation = generation;
        slot->telemetry_epoch = trace_available ? telemetry_epoch : 0;
        slot->capture_sequence = trace_available ? meta->capture_sequence : 0;
        slot->order = order;
        slot->capture_eof_us = trace_available ?
                               meta->capture_eof_timestamp_us : 0;
        slot->ppa_done_us = trace_available ? meta->ppa_done_timestamp_us : 0;
        /* Stage ages are intentionally EOF-relative.  An overlapped encoder
         * starts before that origin, so the start sample is unavailable while
         * encode-done and every downstream stage remain directly measurable. */
        slot->encode_start_us = trace_available &&
                                !meta->encode_overlapped_capture ?
                                meta->encode_start_timestamp_us : 0;
        slot->encode_done_us = trace_available ? meta->encode_done_timestamp_us : 0;
        slot->au_copy_ready_us = au_copy_ready_us;
        slot->keyframe = keyframe || has_idr;
        slot->has_idr = has_idr;
        slot->has_sps = has_sps;
        slot->has_pps = has_pps;
        uint32_t sync_idr_generation = meta ? meta->sync_idr_generation : 0;
        if (sync_idr_generation != 0 && slot->keyframe && slot->has_idr &&
            slot->has_sps && slot->has_pps && ctx->waiting_for_sync &&
            sync_idr_generation == ctx->active_peer_generation &&
            sync_idr_generation == ctx->sync_idr_inflight_generation &&
            ctx->sync_idr_committed_generation == 0) {
            slot->sync_idr_generation = sync_idr_generation;
            ctx->sync_idr_inflight_generation = 0;
            ctx->stats.sync_idr_inflight_generation = 0;
            ctx->sync_idr_committed_generation = sync_idr_generation;
            ctx->stats.sync_idr_committed_generation = sync_idr_generation;
            ctx->stats.sync_idr_committed++;
        }
        slot->state = AU_SLOT_READY;
        ctx->stats.access_units_enqueued++;
        if (ctx->have_last_pts && (int32_t)(pts_90khz - ctx->stats.last_input_pts_90khz) < 0) {
            ctx->stats.pts_regressions++;
        }
        ctx->have_last_pts = true;
        ctx->stats.last_input_pts_90khz = pts_90khz;
        accepted = true;
    } else {
        slot->state = AU_SLOT_FREE;
        slot->size = 0;
        slot->sync_idr_generation = 0;
        if (valid_payload) {
            ctx->stats.access_units_no_session++;
        }
    }
    ctx->submitters--;
    portEXIT_CRITICAL(&ctx->lock);
    /* A cancelled oldest FILLING slot can also unblock a later READY AU. */
    notify_peer_task(ctx);
    return accepted;
}

/* Leases a FREE slot of the active mode to a zeroed reservation:
 * ESP_ERR_INVALID_STATE without a viewer of that mode, ESP_ERR_NO_MEM while
 * every slot is taken (waiter, if any, is woken when one is released). */
static esp_err_t lease_access_unit(
    uint16_t width, uint16_t height, uint16_t fps,
    webrtc_transport_au_reservation_t *reservation, TaskHandle_t waiter)
{
    transport_ctx_t *ctx = &s_ctx;
    portENTER_CRITICAL(&ctx->lock);
    if (!ctx->started || !ctx->session_active || !ctx->mode_valid ||
        ctx->video_paused || ctx->width != width || ctx->height != height ||
        ctx->fps != fps) {
        portEXIT_CRITICAL(&ctx->lock);
        return ESP_ERR_INVALID_STATE;
    }
    int index = reserve_slot_locked(ctx);
    if (index < 0) {
        ctx->stats.access_units_no_slot++;
        ctx->slot_waiter = waiter;
        portEXIT_CRITICAL(&ctx->lock);
        return ESP_ERR_NO_MEM;
    }
    au_slot_t *slot = &ctx->slots[index];
    slot->state = AU_SLOT_FILLING;
    slot->order = ++ctx->next_au_order;
    slot->generation = ctx->generation;
    slot->video_epoch = ctx->video_epoch;
    slot->telemetry_epoch = ctx->stats.stage_age.epoch;
    slot->sync_idr_generation = 0;
    ctx->submitters++;
    *reservation = (webrtc_transport_au_reservation_t){
        .data = slot->data,
        .capacity = slot->capacity,
        .token = slot->order,
        .generation = slot->generation,
        .slot = (uint32_t)index,
    };
    portEXIT_CRITICAL(&ctx->lock);
    /* A legacy copying submission can leave dirty lines, including a tail
     * beyond the next (smaller) DMA frame. Clean its entire recorded prefix
     * before lending the buffer to DMA, while FILLING pins its lifetime.
     * The pinned esp_peer H264 single/FU-A packetizers only read the AU
     * and memcpy into their own RTP buffers; they never dirty this storage.
     * The hardware encoder writes back its software NAL headers itself, so
     * normal DMA-only reuse needs no cache walk here. */
    if (slot->cpu_dirty_bytes) {
        size_t clean_bytes = (slot->cpu_dirty_bytes + 127U) & ~(size_t)127U;
        esp_err_t ret = esp_cache_msync(slot->data, clean_bytes,
                                       ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        if (ret != ESP_OK) {
            webrtc_transport_cancel_access_unit(reservation);
            return ret;
        }
        slot->cpu_dirty_bytes = 0;
    }
    return ESP_OK;
}

bool webrtc_transport_reserve_access_unit(
    uint16_t width, uint16_t height, uint16_t fps,
    webrtc_transport_au_reservation_t *reservation)
{
    if (!reservation || reservation->data) {
        return false;
    }
    *reservation = (webrtc_transport_au_reservation_t){0};
    return webrtc_transport_set_mode(width, height, fps) == ESP_OK &&
           lease_access_unit(width, height, fps, reservation, NULL) == ESP_OK;
}

esp_err_t webrtc_transport_reserve_guest_access_unit(
    uint16_t width, uint16_t height, uint16_t fps,
    webrtc_transport_au_reservation_t *reservation)
{
    if (!reservation || reservation->data) {
        return ESP_ERR_INVALID_ARG;
    }
    *reservation = (webrtc_transport_au_reservation_t){0};
    return lease_access_unit(width, height, fps, reservation,
                             xTaskGetCurrentTaskHandle());
}

/* The CPU wrote the first written bytes of the leased slot; the next DMA
 * lease writes them back first. FILLING pins the slot. */
static void note_cpu_writes(const webrtc_transport_au_reservation_t *reservation,
                            size_t written)
{
    if (!reservation || !reservation->data ||
        reservation->slot >= CONFIG_WEBRTC_TRANSPORT_AU_SLOTS) {
        return;
    }
    au_slot_t *slot = &s_ctx.slots[reservation->slot];
    size_t bytes = written < reservation->capacity ? written :
                                                     reservation->capacity;
    if (slot->data == reservation->data && bytes > slot->cpu_dirty_bytes) {
        slot->cpu_dirty_bytes = bytes;
    }
}

bool webrtc_transport_commit_cpu_access_unit(
    webrtc_transport_au_reservation_t *reservation, size_t size,
    const app_h264_frame_meta_t *meta)
{
    note_cpu_writes(reservation, size);
    return webrtc_transport_commit_access_unit(reservation, size, meta);
}

void webrtc_transport_cancel_cpu_access_unit(
    webrtc_transport_au_reservation_t *reservation, size_t written)
{
    note_cpu_writes(reservation, written);
    webrtc_transport_cancel_access_unit(reservation);
}

void webrtc_transport_cancel_access_unit(
    webrtc_transport_au_reservation_t *reservation)
{
    if (!reservation) {
        return;
    }
    transport_ctx_t *ctx = &s_ctx;
    bool released = false;
    portENTER_CRITICAL(&ctx->lock);
    if (reservation_matches_locked(ctx, reservation)) {
        au_slot_t *slot = &ctx->slots[reservation->slot];
        slot->state = AU_SLOT_FREE;
        slot->size = 0;
        slot->sync_idr_generation = 0;
        ctx->submitters--;
        released = true;
    }
    portEXIT_CRITICAL(&ctx->lock);
    *reservation = (webrtc_transport_au_reservation_t){0};
    if (released) {
        notify_peer_task(ctx);
    }
}

bool webrtc_transport_commit_access_unit(
    webrtc_transport_au_reservation_t *reservation, size_t size,
    const app_h264_frame_meta_t *meta)
{
    if (!reservation) {
        return false;
    }
    transport_ctx_t *ctx = &s_ctx;
    portENTER_CRITICAL(&ctx->lock);
    bool owned = reservation_matches_locked(ctx, reservation);
    if (owned) {
        ctx->stats.access_units_offered++;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (!owned) {
        *reservation = (webrtc_transport_au_reservation_t){0};
        return false;
    }
    bool oversize = size > reservation->capacity;
    bool valid_payload = meta && !oversize &&
                         starts_with_annexb(reservation->data, size);
    if (!valid_payload) {
        portENTER_CRITICAL(&ctx->lock);
        if (oversize) {
            ctx->stats.access_units_oversize++;
        } else {
            ctx->stats.access_units_bad_annexb++;
        }
        portEXIT_CRITICAL(&ctx->lock);
    }
    webrtc_transport_au_reservation_t owned_reservation = *reservation;
    *reservation = (webrtc_transport_au_reservation_t){0};
    return finish_access_unit(&owned_reservation, size,
        meta ? meta->rtp_timestamp_90khz : 0,
        meta && meta->keyframe, meta, valid_payload);
}

static bool webrtc_transport_send_annexb_traced(
    const uint8_t *data, size_t size, uint32_t pts_90khz, bool keyframe,
    const app_h264_frame_meta_t *meta)
{
    transport_ctx_t *ctx = &s_ctx;
    int slot_index = -1;
    uint32_t generation = 0;
    uint32_t telemetry_epoch = 0;
    uint64_t order = 0;

    portENTER_CRITICAL(&ctx->lock);
    ctx->stats.access_units_offered++;
    if (!ctx->started || !ctx->session_active || !ctx->mode_valid ||
        ctx->video_paused) {
        ctx->stats.access_units_no_session++;
        portEXIT_CRITICAL(&ctx->lock);
        return false;
    }
    /* Count rejected AUs too, so the sender detects a broken reference chain. */
    order = ++ctx->next_au_order;
    if (!starts_with_annexb(data, size)) {
        ctx->stats.access_units_bad_annexb++;
        portEXIT_CRITICAL(&ctx->lock);
        return false;
    }
    if (size > CONFIG_WEBRTC_TRANSPORT_MAX_AU_BYTES) {
        ctx->stats.access_units_oversize++;
        portEXIT_CRITICAL(&ctx->lock);
        return false;
    }
    slot_index = reserve_slot_locked(ctx);
    if (slot_index < 0) {
        ctx->stats.access_units_queue_full++;
        portEXIT_CRITICAL(&ctx->lock);
        return false;
    }
    generation = ctx->generation;
    telemetry_epoch = ctx->stats.stage_age.epoch;
    ctx->slots[slot_index].order = order;
    ctx->slots[slot_index].generation = generation;
    ctx->slots[slot_index].video_epoch = ctx->video_epoch;
    ctx->slots[slot_index].telemetry_epoch = telemetry_epoch;
    ctx->slots[slot_index].state = AU_SLOT_FILLING;
    ctx->slots[slot_index].sync_idr_generation = 0;
    ctx->submitters++;
    portEXIT_CRITICAL(&ctx->lock);

    memcpy(ctx->slots[slot_index].data, data, size);
    if (size > ctx->slots[slot_index].cpu_dirty_bytes) {
        ctx->slots[slot_index].cpu_dirty_bytes = size;
    }
    webrtc_transport_au_reservation_t reservation = {
        .data = ctx->slots[slot_index].data,
        .capacity = CONFIG_WEBRTC_TRANSPORT_MAX_AU_BYTES,
        .token = order,
        .generation = generation,
        .slot = (uint32_t)slot_index,
    };
    return finish_access_unit(&reservation, size, pts_90khz, keyframe, meta, true);
}

bool webrtc_transport_send_annexb(const uint8_t *data, size_t size,
                                  uint32_t pts_90khz, bool keyframe)
{
    return webrtc_transport_send_annexb_traced(data, size, pts_90khz,
                                                keyframe, NULL);
}

static bool peer_publish_controlled_answer(transport_ctx_t *ctx,
                                           esp_peer_msg_t *message)
{
    bool requested = false;
    bool offered = false;
    uint8_t extension_id = 0;
    uint32_t answer_generation = 0;
    portENTER_CRITICAL(&ctx->lock);
    answer_generation = ctx->active_peer_generation;
    requested = !ctx->closing_peer && answer_generation != 0 &&
                ctx->active_peer_role != WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING &&
                ctx->active_playout_delay_requested &&
                ctx->active_playout_delay_generation == answer_generation;
    offered = requested && ctx->active_playout_delay_offered;
    extension_id = ctx->active_playout_delay_extension_id;
    portEXIT_CRITICAL(&ctx->lock);

    if (!offered) {
        return outbound_push(ctx, WEBRTC_SIGNAL_SDP, message->data,
                             (size_t)message->size);
    }

    webrtc_playout_delay_answer_t answer = { 0 };
    int ret = webrtc_playout_delay_prepare_answer(
        message->data, (size_t)message->size, extension_id,
        &s_playout_delay_allocator, &answer);
    if (ret == 0) {
        bool delivered = outbound_push(
            ctx, WEBRTC_SIGNAL_SDP, (const uint8_t *)answer.data,
            answer.size);
        bool applied = false;
        portENTER_CRITICAL(&ctx->lock);
        if (delivered && !ctx->closing_peer && answer_generation != 0 &&
            ctx->active_peer_generation == answer_generation &&
            ctx->active_peer_role != WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING &&
            ctx->active_playout_delay_generation == answer_generation &&
            ctx->active_playout_delay_requested &&
            ctx->active_playout_delay_offered &&
            ctx->active_playout_delay_extension_id == extension_id) {
            ctx->active_playout_delay_negotiated = true;
            ctx->stats.rtp_playout_delay_negotiated = true;
            applied = true;
        }
        portEXIT_CRITICAL(&ctx->lock);
        if (applied) {
            ESP_LOGI(TAG,
                     "reflected RTP playout-delay in controlled answer extmap=%u inserted=%d",
                     extension_id, answer.inserted);
        }
        webrtc_playout_delay_release_answer(&answer);
        return delivered;
    }

    bool failure_recorded = false;
    portENTER_CRITICAL(&ctx->lock);
    if (!ctx->closing_peer && answer_generation != 0 &&
        ctx->active_peer_generation == answer_generation &&
        ctx->active_peer_role != WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING &&
        ctx->active_playout_delay_generation == answer_generation &&
        ctx->active_playout_delay_offered &&
        ctx->active_playout_delay_extension_id == extension_id) {
        ctx->active_playout_delay_negotiated = false;
        ctx->stats.rtp_playout_delay_negotiated = false;
        ctx->stats.rtp_playout_delay_answer_rejections++;
        failure_recorded = true;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (failure_recorded) {
        ESP_LOGW(TAG,
                 "could not reflect RTP playout-delay in controlled answer extmap=%u: %d",
                 extension_id, ret);
    }
    /* A failed optional extension reflection must not suppress the base SDP
     * answer.  RTP stamping remains disabled for this generation. */
    return outbound_push(ctx, WEBRTC_SIGNAL_SDP, message->data,
                         (size_t)message->size);
}

static int peer_on_message(esp_peer_msg_t *message, void *arg)
{
    transport_ctx_t *ctx = arg;
    if (!message || !message->data || message->size <= 0) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    if ((message->type == ESP_PEER_MSG_TYPE_SDP ||
         message->type == ESP_PEER_MSG_TYPE_CANDIDATE) &&
        (size_t)message->size > WEBRTC_SIGNAL_PAYLOAD_MAX_BYTES) {
        record_signaling_drop(ctx);
        ESP_LOGW(TAG, "rejecting oversized peer signaling message: %d bytes",
                 message->size);
        return ESP_PEER_ERR_INVALID_ARG;
    }
    switch (message->type) {
    case ESP_PEER_MSG_TYPE_SDP: {
        bool controlled = false;
        portENTER_CRITICAL(&ctx->lock);
        controlled = !ctx->closing_peer &&
                     ctx->active_peer_role != WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING;
        portEXIT_CRITICAL(&ctx->lock);
        if (controlled) {
            webrtc_h264_offer_profiles_t h264_profiles;
            portENTER_CRITICAL(&ctx->lock);
            h264_profiles = ctx->controlled_offer_h264_profiles;
            portEXIT_CRITICAL(&ctx->lock);
            if (webrtc_h264_answer_use_offer_profiles(
                    (char *)message->data, (size_t)message->size,
                    &h264_profiles)) {
                ESP_LOGI(TAG, "answer uses the H.264 profile of the offer");
            }
            (void)peer_publish_controlled_answer(ctx, message);
            break;
        }
        bool requested = false;
        uint32_t offer_generation = 0;
        portENTER_CRITICAL(&ctx->lock);
        offer_generation = ctx->active_peer_generation;
        requested = !ctx->closing_peer && offer_generation != 0 &&
                    ctx->active_playout_delay_requested &&
                    ctx->active_playout_delay_generation ==
                        offer_generation;
        portEXIT_CRITICAL(&ctx->lock);
        if (requested) {
            webrtc_playout_delay_offer_t offer = { 0 };
            int ret = webrtc_playout_delay_prepare_offer(
                message->data, (size_t)message->size,
                &s_playout_delay_allocator, &offer);
            if (ret == 0) {
                bool committed = false;
                portENTER_CRITICAL(&ctx->lock);
                if (!ctx->closing_peer && offer_generation != 0 &&
                    ctx->active_peer_generation == offer_generation &&
                    ctx->active_playout_delay_generation == offer_generation &&
                    ctx->active_playout_delay_requested) {
                    ctx->active_playout_delay_extension_id = offer.extension_id;
                    ctx->active_playout_delay_offered = true;
                    ctx->active_playout_delay_negotiated = false;
                    ctx->stats.rtp_playout_delay_requested = true;
                    ctx->stats.rtp_playout_delay_offered = true;
                    ctx->stats.rtp_playout_delay_negotiated = false;
                    ctx->stats.rtp_playout_delay_extension_id = offer.extension_id;
                    committed = true;
                }
                portEXIT_CRITICAL(&ctx->lock);
                if (committed) {
                    ESP_LOGI(TAG,
                             "offering RTP playout-delay min=0 max=0 extmap=%u inserted=%d",
                             offer.extension_id, offer.inserted);
                    outbound_push(ctx, WEBRTC_SIGNAL_SDP,
                                  (const uint8_t *)offer.data, offer.size);
                }
                webrtc_playout_delay_release_offer(&offer);
                if (committed) {
                    break;
                }
                outbound_push(ctx, WEBRTC_SIGNAL_SDP, message->data,
                              (size_t)message->size);
                break;
            }
            bool failure_recorded = false;
            portENTER_CRITICAL(&ctx->lock);
            if (!ctx->closing_peer && offer_generation != 0 &&
                ctx->active_peer_generation == offer_generation &&
                ctx->active_playout_delay_generation == offer_generation) {
                ctx->active_playout_delay_extension_id = 0;
                ctx->active_playout_delay_offered = false;
                ctx->active_playout_delay_negotiated = false;
                ctx->stats.rtp_playout_delay_offered = false;
                ctx->stats.rtp_playout_delay_negotiated = false;
                ctx->stats.rtp_playout_delay_extension_id = 0;
                ctx->stats.rtp_playout_delay_offer_failures++;
                failure_recorded = true;
            }
            portEXIT_CRITICAL(&ctx->lock);
            if (failure_recorded) {
                ESP_LOGW(TAG,
                         "could not add RTP playout-delay extmap to offer: %d",
                         ret);
            }
        }
        outbound_push(ctx, WEBRTC_SIGNAL_SDP, message->data, (size_t)message->size);
        break;
    }
    case ESP_PEER_MSG_TYPE_CANDIDATE:
        outbound_push(ctx, WEBRTC_SIGNAL_CANDIDATE, message->data, (size_t)message->size);
        break;
    default:
        break;
    }
    return ESP_PEER_ERR_NONE;
}

static webrtc_transport_peer_state_t map_peer_state(esp_peer_state_t state)
{
    switch (state) {
    case ESP_PEER_STATE_CANDIDATE_GATHERING:
        return WEBRTC_TRANSPORT_PEER_GATHERING;
    case ESP_PEER_STATE_PAIRING:
    case ESP_PEER_STATE_PAIRED:
        return WEBRTC_TRANSPORT_PEER_PAIRING;
    case ESP_PEER_STATE_CONNECTING:
        return WEBRTC_TRANSPORT_PEER_CONNECTING;
    case ESP_PEER_STATE_CONNECTED:
    case ESP_PEER_STATE_DATA_CHANNEL_CONNECTED:
    case ESP_PEER_STATE_DATA_CHANNEL_OPENED:
    case ESP_PEER_STATE_DATA_CHANNEL_CLOSED:
        return WEBRTC_TRANSPORT_PEER_CONNECTED;
    case ESP_PEER_STATE_CONNECT_FAILED:
        return WEBRTC_TRANSPORT_PEER_FAILED;
    case ESP_PEER_STATE_CLOSED:
    case ESP_PEER_STATE_DISCONNECTED:
        return WEBRTC_TRANSPORT_PEER_WAITING;
    default:
        return WEBRTC_TRANSPORT_PEER_GATHERING;
    }
}

static int peer_on_remb(esp_peer_remb_report_t *report, void *arg)
{
    transport_ctx_t *ctx = arg;
    if (!report) return ESP_PEER_ERR_NONE;
    int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&ctx->lock);
    if (ctx->peer_connected && ctx->active_peer_generation == ctx->generation &&
        (!report->ssrc || report->ssrc == ctx->video_ssrc)) {
        webrtc_bitrate_remb(&ctx->bitrate_controller, report->bitrate, now);
    }
    portEXIT_CRITICAL(&ctx->lock);
    return ESP_PEER_ERR_NONE;
}

/* measured: a fixed rate (not adaptive) still gets its send rate taken once
 * per second, for a caller that has its own limit. */
static uint32_t video_bitrate(transport_ctx_t *ctx, uint32_t ceiling,
                              bool adaptive, bool measured)
{
    int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&ctx->lock);
    uint32_t result = ceiling;
    const uint32_t previous = ctx->stats.video_target_bitrate;
    const bool live = ctx->peer_connected &&
                      ctx->active_peer_generation == ctx->generation;
    /* Slot exhaustion is reported apart from socket refusals; the
     * controller counts it only after receiver loss (adaptive_bitrate.h). */
    const uint64_t stalls = ctx->stats.access_units_queue_full +
                            ctx->stats.access_units_would_block +
                            ctx->stats.access_units_send_errors;
    if (!adaptive && measured && live) {
        (void)webrtc_bitrate_fixed(&ctx->bitrate_controller, ceiling,
                                   ctx->stats.access_unit_bytes_sent, stalls,
                                   ctx->stats.access_units_no_slot, now);
    } else if (!adaptive) {
        /* A manual preset is a fixed rate. Forget the feedback history so a
         * later switch to Auto starts from the ceiling, not from a stale cut. */
        memset(&ctx->bitrate_controller, 0, sizeof(ctx->bitrate_controller));
    } else if (live) {
        result = webrtc_bitrate_step(&ctx->bitrate_controller, ceiling,
                                     ctx->stats.access_unit_bytes_sent, stalls,
                                     ctx->stats.access_units_no_slot, now);
    }
    const webrtc_bitrate_controller_t *s = &ctx->bitrate_controller;
    ctx->stats.video_target_bitrate = result;
    ctx->stats.video_bitrate_ceiling = ceiling;
    ctx->stats.video_remb_bitrate = s->remb;
    ctx->stats.video_remb_reports = s->remb_reports;
    ctx->stats.video_remb_decreases = s->remb_decreases;
    ctx->stats.video_bitrate_step_ms = s->step_ms;
    ctx->stats.video_bitrate_step_bytes = s->sent_bytes;
    ctx->stats.video_bitrate_step_stalls = s->stalls;
    ctx->stats.video_bitrate_step_slot_drops = s->slot_drops;
    ctx->stats.video_bitrate_step_remb = s->evaluated_remb;
    ctx->stats.video_bitrate_step_reports = s->consumed_remb;
    ctx->stats.video_bitrate_step_decreases = s->consumed_decreases;
    ctx->stats.video_bitrate_step_lost = s->step_lost;
    ctx->stats.video_bitrate_step_sequence = s->step_sequence;
    ctx->stats.video_bitrate_step_loss_reports = s->step_loss_reports;
    ctx->stats.video_bitrate_loss_permille = s->loss_permille;
    ctx->stats.video_achieved_bitrate = s->achieved;
    ctx->stats.video_remb_cuts = s->decisions[WEBRTC_BITRATE_CUT];
    ctx->stats.video_remb_ignored = s->decisions[WEBRTC_BITRATE_IGNORED];
    ctx->stats.video_bitrate_restores = s->decisions[WEBRTC_BITRATE_RESTORE];
    ctx->stats.video_bitrate_reason = webrtc_bitrate_reason_name(s->reason);
    const webrtc_bitrate_reason_t reason = s->reason;
    const uint32_t remb = s->remb;
    const uint32_t achieved = s->achieved;
    portEXIT_CRITICAL(&ctx->lock);
    if (result != previous) {
        /* The encoder worker logs the applied value at INFO; this adds the
         * controller's reason for debugging. */
        ESP_LOGD(TAG, "video bitrate %s: target=%" PRIu32 " previous=%" PRIu32
                 " ceiling=%" PRIu32 " remb=%" PRIu32 " achieved=%" PRIu32,
                 webrtc_bitrate_reason_name(reason), result, previous, ceiling,
                 remb, achieved);
    }
    return result;
}

uint32_t webrtc_transport_video_bitrate(uint32_t ceiling, bool adaptive)
{
    return video_bitrate(webrtc_transport_context(), ceiling, adaptive, false);
}

bool webrtc_transport_guest_bitrate(uint32_t ceiling_bps, bool adaptive,
                                    uint32_t *target_bps, uint32_t *sent_bps,
                                    bool *cut)
{
    transport_ctx_t *ctx = webrtc_transport_context();
    portENTER_CRITICAL(&ctx->lock);
    const int64_t previous_step_ms = ctx->bitrate_controller.step_ms;
    portEXIT_CRITICAL(&ctx->lock);
    *target_bps = video_bitrate(ctx, ceiling_bps, adaptive, true);
    portENTER_CRITICAL(&ctx->lock);
    /* A first step only starts the interval. */
    const bool evaluated = previous_step_ms != 0 &&
                           ctx->bitrate_controller.step_ms != previous_step_ms;
    *sent_bps = ctx->bitrate_controller.achieved;
    *cut = ctx->bitrate_controller.reason == WEBRTC_BITRATE_CUT;
    portEXIT_CRITICAL(&ctx->lock);
    return evaluated;
}

static int peer_on_state(esp_peer_state_t state, void *arg)
{
    transport_ctx_t *ctx = arg;
    if (state == ESP_PEER_STATE_VIDEO_PLI_RECEIVED) {
        /* esp_peer >= 1.5.4 reports authenticated video feedback through
         * on_state too. It is not a connection transition: clearing media
         * readiness here stops the sender without scheduling a reconnect.
         * Share coalescing/rate limits with the RTCP decrypt observer. */
        portENTER_CRITICAL(&ctx->lock);
        uint32_t generation = ctx->active_peer_generation;
        portEXIT_CRITICAL(&ctx->lock);
        handle_video_pli(ctx, generation);
        return ESP_PEER_ERR_NONE;
    }
    /* esp_peer can emit CONNECTED before its reconnecting DTLS object
     * has published a usable SRTP stream.  Sending the forced sync IDR from
     * that callback window can reach srtp_get_stream(NULL) (observed as a
     * core-0 Load access fault).  This product always enables/negotiates the
     * RPC DataChannel, whose CONNECTED event is emitted only after DTLS/SRTP
     * and SCTP complete.  Use that as the media-ready barrier; OPENED/CLOSED
     * keep video alive across normal channel lifecycle changes. */
    bool connected = state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED ||
                     state == ESP_PEER_STATE_DATA_CHANNEL_OPENED ||
                     state == ESP_PEER_STATE_DATA_CHANNEL_CLOSED;
    bool failed = state == ESP_PEER_STATE_CONNECT_FAILED ||
                  state == ESP_PEER_STATE_CLOSED ||
                  state == ESP_PEER_STATE_DISCONNECTED ||
                  state == ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED;
    uint32_t hid_generation_to_invalidate = 0;
    portENTER_CRITICAL(&ctx->lock);
    ctx->stats.peer_state = map_peer_state(state);
    if (connected && !ctx->peer_connected) {
        ctx->stats.peer_connects++;
        ctx->waiting_for_sync = true;
        ctx->stats.waiting_for_sync_frame = true;
        uint32_t peer_generation = ctx->active_peer_generation;
        if (peer_generation != 0 &&
            peer_generation != ctx->sync_idr_last_requested_generation) {
            ctx->sync_idr_last_requested_generation = peer_generation;
            ctx->sync_idr_pending_generation = peer_generation;
            ctx->stats.sync_idr_pending_generation = peer_generation;
            ctx->stats.sync_idr_requests++;
        }
        ctx->peer_tls_release_pending = true;
    }
    ctx->peer_connected = connected;
    ctx->stats.peer_connected = connected;
    if (failed && !ctx->closing_peer && ctx->session_active &&
        !ctx->rebuild_requested) {
        hid_generation_to_invalidate = ctx->active_peer_generation;
        uint32_t invalidated_generation = ctx->generation;
        next_generation_locked(ctx);
        schedule_cloud_invalidation_locked(
            ctx, invalidated_generation, ctx->generation);
        ctx->rebuild_requested = true;
        ctx->reconnect_not_before_us = esp_timer_get_time() +
            (int64_t)CONFIG_WEBRTC_TRANSPORT_RECONNECT_DELAY_MS * 1000;
    }
    portEXIT_CRITICAL(&ctx->lock);
    hid_input_invalidate_generation(hid_generation_to_invalidate,
                                    "peer disconnected");
    ESP_LOGI(TAG, "peer state=%d (%s)", state,
             webrtc_transport_peer_state_name(map_peer_state(state)));
    return ESP_PEER_ERR_NONE;
}

static void release_completed_peer_tls_reservation(transport_ctx_t *ctx)
{
    jetkvm_memory_policy_reservation_t reservation = {0};
    portENTER_CRITICAL(&ctx->lock);
    if (ctx->peer_tls_release_pending) {
        reservation = ctx->peer_tls_reservation;
        memset(&ctx->peer_tls_reservation, 0,
               sizeof(ctx->peer_tls_reservation));
        ctx->peer_tls_release_pending = false;
    }
    portEXIT_CRITICAL(&ctx->lock);
    jetkvm_memory_policy_release(&reservation);
}

static int rtp_get_encoded_size(esp_peer_rtp_frame_t *frame, bool *in_place, void *arg)
{
    transport_ctx_t *ctx = arg;
    if (!frame || !in_place) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    frame->encoded_size = frame->orig_size;
    *in_place = true;
    bool negotiated = false;
    uint8_t extension_id = 0;
    portENTER_CRITICAL(&ctx->lock);
    negotiated = ctx->active_playout_delay_requested &&
                 ctx->active_playout_delay_offered &&
                 ctx->active_playout_delay_negotiated &&
                 ctx->active_playout_delay_generation ==
                     ctx->active_peer_generation;
    extension_id = ctx->active_playout_delay_extension_id;
    portEXIT_CRITICAL(&ctx->lock);
    if (!negotiated) {
        return ESP_PEER_ERR_NONE;
    }
    size_t output_size = 0;
    webrtc_playout_delay_rtp_result_t plan = webrtc_playout_delay_rtp_plan(
        frame->orig_data, frame->orig_size, extension_id, &output_size);
    if (plan == WEBRTC_PLAYOUT_DELAY_RTP_INVALID || output_size > UINT32_MAX) {
        return ESP_PEER_ERR_BAD_DATA;
    }
    if (plan == WEBRTC_PLAYOUT_DELAY_RTP_STAMPED) {
        frame->encoded_size = (uint32_t)output_size;
        *in_place = false;
    }
    return ESP_PEER_ERR_NONE;
}

static int rtp_transform(esp_peer_rtp_frame_t *frame, void *arg)
{
    transport_ctx_t *ctx = arg;
    if (!frame || !frame->orig_data || frame->orig_size < RTP_HEADER_MIN_SIZE) {
        return ESP_PEER_ERR_BAD_DATA;
    }
    bool negotiated = false;
    uint8_t extension_id = 0;
    const uint32_t ssrc = ((uint32_t)frame->orig_data[8] << 24) |
        ((uint32_t)frame->orig_data[9] << 16) |
        ((uint32_t)frame->orig_data[10] << 8) | frame->orig_data[11];
    portENTER_CRITICAL(&ctx->lock);
    /* esp_peer passes audio packets through this transformer too. */
    const bool new_video_ssrc = ctx->sending_video &&
        (ssrc != ctx->retransmit_ssrc ||
         ctx->retransmit_generation != ctx->active_peer_generation);
    if (new_video_ssrc) {
        ctx->retransmit_ssrc = ssrc;
        ctx->retransmit_generation = ctx->active_peer_generation;
    }
    ctx->video_ssrc = ssrc;
    ctx->video_ssrc_generation = ctx->active_peer_generation;
    negotiated = ctx->active_playout_delay_requested &&
                 ctx->active_playout_delay_offered &&
                 ctx->active_playout_delay_negotiated &&
                 ctx->active_playout_delay_generation ==
                     ctx->active_peer_generation;
    extension_id = ctx->active_playout_delay_extension_id;
    portEXIT_CRITICAL(&ctx->lock);
    if (new_video_ssrc) {
        webrtc_network_timing_set_video_ssrc(ssrc);
    }

    const uint8_t *rtp = frame->orig_data;
    size_t rtp_size = frame->orig_size;
    bool stamped = false;
    bool bypassed = false;
    if (negotiated) {
        size_t planned_size = 0;
        webrtc_playout_delay_rtp_result_t plan = webrtc_playout_delay_rtp_plan(
            frame->orig_data, frame->orig_size, extension_id, &planned_size);
        if (plan == WEBRTC_PLAYOUT_DELAY_RTP_INVALID ||
            planned_size > UINT32_MAX) {
            portENTER_CRITICAL(&ctx->lock);
            ctx->stats.rtp_playout_delay_transform_errors++;
            portEXIT_CRITICAL(&ctx->lock);
            return ESP_PEER_ERR_BAD_DATA;
        }
        if (plan == WEBRTC_PLAYOUT_DELAY_RTP_STAMPED) {
            uint32_t output_capacity = frame->encoded_size;
            size_t written = 0;
            if (!frame->encoded_data || output_capacity < planned_size ||
                webrtc_playout_delay_rtp_write(
                    frame->orig_data, frame->orig_size, extension_id,
                    frame->encoded_data, output_capacity, &written) !=
                    WEBRTC_PLAYOUT_DELAY_RTP_STAMPED ||
                written > UINT32_MAX) {
                portENTER_CRITICAL(&ctx->lock);
                ctx->stats.rtp_playout_delay_transform_errors++;
                portEXIT_CRITICAL(&ctx->lock);
                return ESP_PEER_ERR_BAD_DATA;
            }
            rtp = frame->encoded_data;
            rtp_size = written;
            frame->encoded_size = (uint32_t)written;
            stamped = true;
        } else {
            frame->encoded_size = frame->orig_size;
            bypassed = true;
        }
    } else {
        frame->encoded_size = frame->orig_size;
    }

    uint32_t timestamp = ((uint32_t)rtp[4] << 24) | ((uint32_t)rtp[5] << 16) |
                         ((uint32_t)rtp[6] << 8) | rtp[7];
    bool marker = (rtp[1] & 0x80) != 0;
    portENTER_CRITICAL(&ctx->lock);
    ctx->stats.rtp_packets_sent++;
    ctx->stats.rtp_bytes_sent += rtp_size;
    if (stamped) {
        ctx->stats.rtp_playout_delay_packets_stamped++;
        ctx->stats.rtp_playout_delay_bytes_added +=
            rtp_size - frame->orig_size;
    } else if (bypassed) {
        ctx->stats.rtp_playout_delay_packets_bypassed++;
    }
    if (marker) {
        ctx->stats.rtp_marker_packets++;
    }
    ctx->stats.last_rtp_timestamp_90khz = timestamp;
    active_send_trace_t *trace = &ctx->active_send_trace;
    if (trace->active && timestamp == trace->expected_rtp_timestamp) {
        bool record_first = trace->first_rtp_us == 0;
        bool record_marker = marker && trace->marker_us == 0;
        if (record_first || record_marker) {
            int64_t now_us = esp_timer_get_time();
            if (record_first) {
                trace->first_rtp_us = now_us;
            }
            if (record_marker) {
                trace->marker_us = now_us;
            }
        }
    }
    portEXIT_CRITICAL(&ctx->lock);
    return ESP_PEER_ERR_NONE;
}

static void close_peer_handle_and_flush_outbound(transport_ctx_t *ctx,
                                                  esp_peer_handle_t peer,
                                                  const char *reason)
{
    if (peer) {
        /* esp_peer_close owns disconnect and resource release as one vendor
         * lifecycle operation. Calling esp_peer_disconnect first tears the
         * same ICE/DTLS/SCTP graph down twice; the second connection then
         * observes corrupted agent pairs or allocator metadata. */
        int ret = esp_peer_close(peer);
        if (ret == ESP_PEER_ERR_NONE) {
            ESP_LOGI(TAG, "%s: esp_peer_close returned %d", reason, ret);
        } else {
            /* esp_peer_close releases its wrapper even when the peer-specific
             * close operation reports an error, so the handle cannot be
             * retried.  Preserve the result in the serial log instead. */
            ESP_LOGW(TAG, "%s: esp_peer_close returned %d", reason, ret);
        }
    }

    /* Peer close can synchronously publish final signaling callbacks.  They
     * are no longer useful once this handle is detached, and retaining their
     * heap-backed copies until a browser poll fragments internal SRAM. */
    flush_outbound(ctx);
}

static void close_peer(transport_ctx_t *ctx)
{
    jetkvm_audio_set_peer_connected(false);
    esp_peer_handle_t peer = NULL;
    owned_ice_server_list_t *active_ice_servers = NULL;
    uint32_t hid_generation = 0;
    jetkvm_memory_policy_reservation_t peer_growth_reservation = {0};
    jetkvm_memory_policy_reservation_t peer_tls_reservation = {0};
    portENTER_CRITICAL(&ctx->lock);
    peer = ctx->peer;
    ctx->peer = NULL;
    peer_growth_reservation = ctx->peer_growth_reservation;
    memset(&ctx->peer_growth_reservation, 0,
           sizeof(ctx->peer_growth_reservation));
    peer_tls_reservation = ctx->peer_tls_reservation;
    memset(&ctx->peer_tls_reservation, 0,
           sizeof(ctx->peer_tls_reservation));
    ctx->peer_tls_release_pending = false;
    active_ice_servers = ctx->active_peer_ice_servers;
    ctx->active_peer_ice_servers = NULL;
    ctx->closing_peer = true;
    ctx->peer_connected = false;
    /* The UI lease dies with its session; OTA and download holds outlive
     * peers. */
    ctx->video_pause_sources &= (uint8_t)~WEBRTC_VIDEO_PAUSE_UI;
    ctx->video_paused = ctx->video_pause_sources != 0;
    reset_data_channels_locked(ctx);
    ctx->active_playout_delay_requested = false;
    ctx->active_playout_delay_offered = false;
    ctx->active_playout_delay_negotiated = false;
    ctx->active_playout_delay_extension_id = 0;
    ctx->active_playout_delay_generation = 0;
    ctx->stats.peer_connected = false;
    ctx->stats.rtp_playout_delay_offered = false;
    ctx->stats.rtp_playout_delay_negotiated = false;
    ctx->stats.rtp_playout_delay_extension_id = 0;
    ctx->stats.peer_state = ctx->started ? WEBRTC_TRANSPORT_PEER_WAITING :
                                          WEBRTC_TRANSPORT_PEER_STOPPED;
    ctx->sync_idr_pending_generation = 0;
    ctx->stats.sync_idr_pending_generation = 0;
    uint32_t active_sync_idr = ctx->sync_idr_inflight_generation ?
        ctx->sync_idr_inflight_generation :
        ctx->sync_idr_committed_generation;
    fail_sync_idr_locked(ctx, active_sync_idr, false);
    ctx->active_send_trace.active = false;
    flush_ready_slots_locked(ctx);
    /* Invalidate the callback generation before closing.  A close callback
     * that tries to publish signaling will then free its temporary copy
     * instead of extending the queue while teardown is draining it. */
    hid_generation = ctx->active_peer_generation;
    ctx->active_hid_generation = 0;
    ctx->stats.hidrpc_active_generation = 0;
    ctx->active_peer_session = 0;
    ctx->active_peer_generation = 0;
    ctx->active_peer_role = WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING;
    portEXIT_CRITICAL(&ctx->lock);
    hid_input_invalidate_generation(hid_generation, "peer teardown");
    close_peer_handle_and_flush_outbound(ctx, peer, "peer teardown");
    clear_data_channel_queue(ctx);
    /* The close callbacks reported what the vendor closed; the rest of the
     * guest channels went with the peer. */
    reset_guest_channels(ctx);
    jetkvm_memory_policy_release(&peer_growth_reservation);
    jetkvm_memory_policy_release(&peer_tls_reservation);
    owned_ice_server_list_release(ctx, active_ice_servers);
    portENTER_CRITICAL(&ctx->lock);
    ctx->closing_peer = false;
    portEXIT_CRITICAL(&ctx->lock);
}

static esp_err_t open_peer(transport_ctx_t *ctx, uint32_t session_id,
                           uint32_t generation, uint16_t width,
                           uint16_t height, uint16_t fps,
                           webrtc_transport_session_role_t role)
{
    esp_err_t fail_result = ESP_FAIL;
    bool hid_generation_armed = false;
    owned_ice_server_list_t *active_ice_servers = NULL;
    jetkvm_memory_policy_reservation_t peer_memory_reservation = {0};
    jetkvm_memory_policy_reservation_t peer_tls_reservation = {0};
    jetkvm_memory_policy_reservation_t published_reservation = {0};
    jetkvm_memory_policy_reservation_t published_tls_reservation = {0};
    ctx->peer_default_cfg = (esp_peer_default_cfg_t) {
        /* DTLS flights over DERP can arrive after eleven one-tick polls.
         * Allow relay latency during setup. Gathering and connected peers
         * use bounded non-blocking receives to preserve signaling and video. */
        .agent_recv_timeout = webrtc_peer_udp_handshake_timeout_ms(),
        .data_ch_cfg = {
            .send_cache_size =
                CONFIG_WEBRTC_TRANSPORT_DATA_CHANNEL_SEND_CACHE_BYTES,
            .recv_cache_size =
                CONFIG_WEBRTC_TRANSPORT_DATA_CHANNEL_RECEIVE_CACHE_BYTES,
        },
        .rtp_cfg = {
            .send_pool_size = CONFIG_WEBRTC_TRANSPORT_RTP_POOL_BYTES,
            .send_queue_num = CONFIG_WEBRTC_TRANSPORT_RTP_QUEUE_PACKETS,
            .max_resend_count = 3,
        },
        .alive_binding_retries = 3,
        .keep_role =
            role != WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING,
    };
    esp_peer_cfg_t peer_cfg = {
        .role = role != WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING ?
                    ESP_PEER_ROLE_CONTROLLED : ESP_PEER_ROLE_CONTROLLING,
        .video_info = {
            .codec = ESP_PEER_VIDEO_CODEC_H264,
            .width = width,
            .height = height,
            .fps = fps,
        },
        .audio_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
        /* The rtpmap line of Opus is "opus/48000/2" whatever the stream
         * carries (RFC 7587); the Mini's frames are mono. */
        .audio_info = {.codec = ESP_PEER_AUDIO_CODEC_OPUS, .sample_rate = 48000, .channel = 2},
        .video_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
        .no_auto_reconnect = true,
        .enable_data_channel = true,
        .manual_ch_create = true,
        .extra_cfg = &ctx->peer_default_cfg,
        .extra_size = sizeof(ctx->peer_default_cfg),
        .ctx = ctx,
        .on_state = peer_on_state,
        .on_remb = peer_on_remb,
        .on_msg = peer_on_message,
        .on_channel_open = peer_on_channel_open,
        .on_data = peer_on_data,
        .on_channel_close = peer_on_channel_close,
    };

    portENTER_CRITICAL(&ctx->lock);
    bool current = ctx->started && !ctx->stop_requested && !ctx->peer &&
                   !ctx->peer_growth_reservation.active &&
                   !ctx->peer_tls_reservation.active &&
                   !ctx->closing_peer && !ctx->rebuild_requested &&
                   ctx->session_active && ctx->session_id == session_id &&
                   ctx->generation == generation &&
                   ctx->session_role == role && !ctx->mode_teardown_waiters;
    if (!current) {
        portEXIT_CRITICAL(&ctx->lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* A session through the cloud opens its peer with the offer's ICE
     * servers, which takes a larger reservation. */
    const bool with_ice_servers =
        role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED &&
        ctx->session_ice_servers != NULL &&
        ctx->session_ice_servers->count != 0U;
    portEXIT_CRITICAL(&ctx->lock);

    /* Reserve the complete peer working set while leaving every vendor
     * allocation in the ESP-IDF global heap. esp_peer mixes media_lib,
     * libc, ringbuffer and process-global objects, so routing only the public
     * media_lib ABI into a generation-local heap creates an invalid allocator
     * boundary during reconnect teardown. DTLS continues after
     * esp_peer_open(), so the session's reservation stays until the
     * DataChannel proves DTLS/SRTP/SCTP are fully materialized. */
    const size_t peer_arena_bytes =
        CONFIG_WEBRTC_TRANSPORT_DATA_CHANNEL_SEND_CACHE_BYTES +
        CONFIG_WEBRTC_TRANSPORT_DATA_CHANNEL_RECEIVE_CACHE_BYTES +
        WEBRTC_PEER_MEMORY_ARENA_OVERHEAD_BYTES;
    const size_t peer_materialization_bytes =
        peer_arena_bytes + CONFIG_WEBRTC_TRANSPORT_RTP_POOL_BYTES;
    const size_t peer_largest_block =
        peer_arena_bytes > CONFIG_WEBRTC_TRANSPORT_RTP_POOL_BYTES ?
            peer_arena_bytes : CONFIG_WEBRTC_TRANSPORT_RTP_POOL_BYTES;
    const jetkvm_memory_policy_request_t peer_memory = {
        .psram_bytes = peer_materialization_bytes,
        .psram_largest_block = peer_largest_block,
        .storage_ingress_bytes =
            JETKVM_MEMORY_SCTP_RECEIVE_MAX_BYTES,
    };
    /* The video buffers are allocated once at boot, so the peer needs its
     * real 1 MiB largest block and the PSRAM floor, whether or not a capture
     * mode is valid. */
    esp_err_t reservation_ret = jetkvm_memory_policy_try_reserve_peer(
        "webrtc-peer", &peer_memory, &peer_memory_reservation);
    if (reservation_ret != ESP_OK) {
        return reservation_ret;
    }
    bool tls_reserved = jetkvm_memory_policy_reserve_tls_session(
        "webrtc-dtls", with_ice_servers, &peer_tls_reservation);
    /* Other owners hold a TLS reservation for one handshake, under a
     * second, so a refusal here is not the lasting shortage that
     * ESP_ERR_NO_MEM waits out for 30 s; ESP_FAIL retries after the
     * reconnect delay. */
    if (!tls_reserved) {
        jetkvm_memory_policy_release(&peer_memory_reservation);
        return ESP_FAIL;
    }
    portENTER_CRITICAL(&ctx->lock);
    current = ctx->started && !ctx->stop_requested && !ctx->peer &&
              !ctx->peer_growth_reservation.active &&
              !ctx->peer_tls_reservation.active &&
              !ctx->closing_peer && !ctx->rebuild_requested &&
              ctx->session_active && ctx->session_id == session_id &&
              ctx->generation == generation &&
              ctx->session_role == role && !ctx->mode_teardown_waiters;
    if (!current) {
        portEXIT_CRITICAL(&ctx->lock);
        jetkvm_memory_policy_release(&peer_memory_reservation);
        jetkvm_memory_policy_release(&peer_tls_reservation);
        return ESP_ERR_INVALID_STATE;
    }
    if (role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED) {
        active_ice_servers = ctx->session_ice_servers;
        owned_ice_server_list_retain_locked(active_ice_servers);
    }
    ctx->active_peer_ice_servers = active_ice_servers;
    ctx->active_peer_session = session_id;
    ctx->active_peer_generation = generation;
    memset(&ctx->bitrate_controller, 0, sizeof(ctx->bitrate_controller));
    ctx->active_peer_role = role;
    reset_data_channels_locked(ctx);
    ctx->waiting_for_sync = true;
    ctx->active_playout_delay_requested =
        ctx->session_active && ctx->session_id == session_id &&
        ctx->session_playout_delay_session_id == session_id &&
        ctx->session_playout_delay_requested;
    ctx->active_playout_delay_offered = false;
    ctx->active_playout_delay_negotiated = false;
    ctx->active_playout_delay_extension_id = 0;
    ctx->active_playout_delay_generation = generation;
    ctx->stats.waiting_for_sync_frame = true;
    ctx->stats.rtp_playout_delay_requested =
        ctx->active_playout_delay_requested;
    ctx->stats.rtp_playout_delay_offered = false;
    ctx->stats.rtp_playout_delay_negotiated = false;
    ctx->stats.rtp_playout_delay_extension_id = 0;
    ctx->stats.peer_state = WEBRTC_TRANSPORT_PEER_GATHERING;
    ctx->stats.peer_rebuilds++;
    portEXIT_CRITICAL(&ctx->lock);

    peer_cfg.server_lists = active_ice_servers ?
        active_ice_servers->servers : NULL;
    peer_cfg.server_num = active_ice_servers ? active_ice_servers->count : 0;

    esp_peer_handle_t peer = NULL;
    int ret = esp_peer_open(&peer_cfg, esp_peer_get_default_impl(), &peer);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_open failed: %d", ret);
        goto fail;
    }
    const jetkvm_memory_policy_request_t peer_growth_memory = {
        /* The complete arena is already reflected in physical heap free.
         * Retain only the logical SCTP receive charge in the shared 4 MiB
         * storage-ingress ledger; cache growth cannot escape the arena. */
        .storage_ingress_bytes =
            JETKVM_MEMORY_SCTP_RECEIVE_MAX_BYTES,
    };
    if (!jetkvm_memory_policy_shrink(
            &peer_memory_reservation, &peer_growth_memory)) {
        fail_result = ESP_ERR_INVALID_STATE;
        ESP_LOGE(TAG, "failed to retain peer growth memory reservation");
        goto fail;
    }
    portENTER_CRITICAL(&ctx->lock);
    current = ctx->started && !ctx->stop_requested && !ctx->peer &&
              !ctx->peer_growth_reservation.active &&
              !ctx->peer_tls_reservation.active &&
              !ctx->closing_peer && !ctx->rebuild_requested &&
              ctx->session_active && ctx->session_id == session_id &&
              ctx->generation == generation &&
              ctx->session_role == role && !ctx->mode_teardown_waiters &&
              ctx->active_peer_session == session_id &&
              ctx->active_peer_generation == generation;
    if (current) {
        ctx->peer = peer;
        ctx->peer_growth_reservation = peer_memory_reservation;
        memset(&peer_memory_reservation, 0,
               sizeof(peer_memory_reservation));
        ctx->peer_tls_reservation = peer_tls_reservation;
        memset(&peer_tls_reservation, 0,
               sizeof(peer_tls_reservation));
        ctx->peer_tls_release_pending = false;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (!current) {
        fail_result = ESP_ERR_INVALID_STATE;
        goto fail;
    }

    ctx->rtp_transform = (esp_peer_rtp_transform_cb_t) {
        .get_encoded_size = rtp_get_encoded_size,
        .transform = rtp_transform,
    };
    ret = esp_peer_set_rtp_transformer(peer, ESP_PEER_RTP_TRANSFORM_ROLE_SENDER,
                                       &ctx->rtp_transform, ctx);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "failed to install RTP accounting transformer: %d", ret);
        goto fail;
    }
    esp_err_t hid_err =
        jetkvm_hid_input_set_active_generation(generation);
    if (hid_err == ESP_OK) {
        hid_generation_armed = true;
        portENTER_CRITICAL(&ctx->lock);
        current = ctx->started && !ctx->stop_requested &&
                  !ctx->closing_peer && !ctx->rebuild_requested &&
                  ctx->session_active && ctx->session_id == session_id &&
                  ctx->generation == generation &&
                  ctx->session_role == role && !ctx->mode_teardown_waiters &&
                  ctx->peer == peer &&
                  ctx->active_peer_session == session_id &&
                  ctx->active_peer_generation == generation;
        if (current) {
            ctx->active_hid_generation = generation;
            ctx->stats.hidrpc_active_generation = generation;
        }
        portEXIT_CRITICAL(&ctx->lock);
        if (!current) {
            fail_result = ESP_ERR_INVALID_STATE;
            goto fail;
        }
    } else if (hid_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "could not arm HID generation %" PRIu32 ": %s",
                 generation, esp_err_to_name(hid_err));
    }
    ret = esp_peer_new_connection(peer);
    if (ret != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_new_connection failed: %d", ret);
        goto fail;
    }
    portENTER_CRITICAL(&ctx->lock);
    current = ctx->started && !ctx->stop_requested &&
              !ctx->closing_peer && !ctx->rebuild_requested &&
              ctx->session_active && ctx->session_id == session_id &&
              ctx->generation == generation && ctx->session_role == role &&
              !ctx->mode_teardown_waiters &&
              ctx->peer == peer && ctx->active_peer_session == session_id &&
              ctx->active_peer_generation == generation &&
              (!hid_generation_armed ||
               ctx->active_hid_generation == generation);
    portEXIT_CRITICAL(&ctx->lock);
    if (!current) {
        fail_result = ESP_ERR_INVALID_STATE;
        goto fail;
    }
    ESP_LOGI(TAG, "new H264 peer generation=%" PRIu32 " mode=%ux%u@%u",
             generation, width, height, fps);
    if (role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED) {
        dispatch_cloud_peer_ready(ctx, session_id, generation);
    }
    return ESP_OK;

fail:
    portENTER_CRITICAL(&ctx->lock);
    if (ctx->peer == peer) {
        ctx->peer = NULL;
        published_reservation = ctx->peer_growth_reservation;
        memset(&ctx->peer_growth_reservation, 0,
               sizeof(ctx->peer_growth_reservation));
        published_tls_reservation = ctx->peer_tls_reservation;
        memset(&ctx->peer_tls_reservation, 0,
               sizeof(ctx->peer_tls_reservation));
        ctx->peer_tls_release_pending = false;
    }
    if (ctx->active_peer_ice_servers == active_ice_servers) {
        ctx->active_peer_ice_servers = NULL;
    }
    ctx->active_peer_session = 0;
    ctx->active_peer_generation = 0;
    if (ctx->active_hid_generation == generation) {
        ctx->active_hid_generation = 0;
        ctx->stats.hidrpc_active_generation = 0;
    }
    ctx->active_peer_role = WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING;
    ctx->active_playout_delay_requested = false;
    ctx->active_playout_delay_offered = false;
    ctx->active_playout_delay_negotiated = false;
    ctx->active_playout_delay_extension_id = 0;
    ctx->active_playout_delay_generation = 0;
    ctx->stats.rtp_playout_delay_offered = false;
    ctx->stats.rtp_playout_delay_negotiated = false;
    ctx->stats.rtp_playout_delay_extension_id = 0;
    ctx->closing_peer = peer != NULL;
    portEXIT_CRITICAL(&ctx->lock);
    if (hid_generation_armed) {
        hid_input_invalidate_generation(generation, "peer open rollback");
    }
    close_peer_handle_and_flush_outbound(ctx, peer, "peer open rollback");
    jetkvm_memory_policy_release(&peer_memory_reservation);
    jetkvm_memory_policy_release(&peer_tls_reservation);
    jetkvm_memory_policy_release(&published_reservation);
    jetkvm_memory_policy_release(&published_tls_reservation);
    owned_ice_server_list_release(ctx, active_ice_servers);
    portENTER_CRITICAL(&ctx->lock);
    ctx->closing_peer = false;
    ctx->stats.peer_state = WEBRTC_TRANSPORT_PEER_FAILED;
    portEXIT_CRITICAL(&ctx->lock);
    return fail_result;
}

static void apply_controlled_remote_offer(
    transport_ctx_t *ctx, esp_peer_handle_t peer,
    const inbound_signal_t *signal, bool playout_delay_requested)
{
    uint8_t extension_id = 0;
    bool offered = playout_delay_requested &&
        webrtc_playout_delay_offer_accepts_sender(
            signal->data, signal->size, &extension_id);
    webrtc_h264_offer_profiles_t h264_profiles;
    webrtc_h264_offer_profiles_parse(signal->data, signal->size,
                                     &h264_profiles);
    bool applied = false;
    portENTER_CRITICAL(&ctx->lock);
    if (ctx->peer == peer) {
        ctx->controlled_offer_h264_profiles = h264_profiles;
    }
    if (ctx->peer == peer && ctx->session_active &&
        ctx->session_role != WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING &&
        ctx->active_peer_role != WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING &&
        signal->session_id == ctx->session_id &&
        signal->generation == ctx->generation &&
        signal->session_id == ctx->active_peer_session &&
        signal->generation == ctx->active_peer_generation &&
        ctx->active_playout_delay_generation == signal->generation) {
        ctx->active_playout_delay_offered = offered;
        ctx->active_playout_delay_negotiated = false;
        ctx->active_playout_delay_extension_id = offered ? extension_id : 0;
        ctx->stats.rtp_playout_delay_offered = offered;
        ctx->stats.rtp_playout_delay_negotiated = false;
        ctx->stats.rtp_playout_delay_extension_id =
            offered ? extension_id : 0;
        applied = true;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (applied && offered) {
        ESP_LOGI(TAG,
                 "browser offer advertised RTP playout-delay extmap=%u",
                 extension_id);
    }
}

static void clear_controlled_offer_after_send_failure(
    transport_ctx_t *ctx, esp_peer_handle_t peer,
    const inbound_signal_t *signal)
{
    portENTER_CRITICAL(&ctx->lock);
    if (ctx->peer == peer &&
        ctx->active_peer_role != WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING &&
        signal->session_id == ctx->active_peer_session &&
        signal->generation == ctx->active_peer_generation) {
        ctx->active_playout_delay_offered = false;
        ctx->active_playout_delay_negotiated = false;
        ctx->active_playout_delay_extension_id = 0;
        ctx->stats.rtp_playout_delay_offered = false;
        ctx->stats.rtp_playout_delay_negotiated = false;
        ctx->stats.rtp_playout_delay_extension_id = 0;
    }
    portEXIT_CRITICAL(&ctx->lock);
}

static void process_inbound(transport_ctx_t *ctx)
{
    for (;;) {
        inbound_signal_t *signal = NULL;
        if (xQueuePeek(ctx->inbound_queue, &signal, 0) != pdTRUE) {
            return;
        }
        if (!signal) {
            (void)xQueueReceive(ctx->inbound_queue, &signal, 0);
            continue;
        }
        esp_peer_handle_t peer = NULL;
        bool playout_delay_offered = false;
        bool playout_delay_requested = false;
        uint8_t playout_delay_extension_id = 0;
        webrtc_transport_session_role_t role =
            WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING;
        webrtc_inbound_action_t action = WEBRTC_INBOUND_DROP_STALE;
        portENTER_CRITICAL(&ctx->lock);
        webrtc_inbound_owner_snapshot_t snapshot = {
            .session_active = ctx->session_active,
            .session_id = ctx->session_id,
            .generation = ctx->generation,
            .peer_ready = ctx->peer && !ctx->closing_peer &&
                          !ctx->rebuild_requested,
            .peer_role_matches = ctx->active_peer_role == ctx->session_role,
            .active_peer_session = ctx->active_peer_session,
            .active_peer_generation = ctx->active_peer_generation,
        };
        action = webrtc_inbound_classify(
            &snapshot, signal->session_id, signal->generation);
        if (action == WEBRTC_INBOUND_DELIVER_CURRENT) {
            peer = ctx->peer;
            role = ctx->active_peer_role;
        }
        if (action == WEBRTC_INBOUND_DELIVER_CURRENT &&
            signal->type == WEBRTC_SIGNAL_SDP) {
            playout_delay_requested = ctx->active_playout_delay_requested &&
                ctx->active_playout_delay_generation == signal->generation;
            if (role == WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING) {
                playout_delay_offered = playout_delay_requested &&
                    ctx->active_playout_delay_offered;
                playout_delay_extension_id =
                    ctx->active_playout_delay_extension_id;
            }
        }
        if (action == WEBRTC_INBOUND_DROP_STALE) {
            ctx->stats.signaling_messages_dropped++;
        }
        portEXIT_CRITICAL(&ctx->lock);

        /* A current-generation event owns its FIFO slot until the exact peer
         * is ready.  This closes create -> immediate offer/candidate races
         * without ever replaying an older session or generation. */
        if (action == WEBRTC_INBOUND_DEFER_CURRENT) {
            return;
        }
        inbound_signal_t *owned_signal = NULL;
        if (xQueueReceive(ctx->inbound_queue, &owned_signal, 0) != pdTRUE) {
            continue;
        }
        signal = owned_signal;
        if (!signal) {
            continue;
        }
        if (action == WEBRTC_INBOUND_DELIVER_CURRENT) {
            if (signal->type == WEBRTC_SIGNAL_SDP &&
                role != WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING) {
                /* esp_peer may synchronously emit its answer from send_msg(),
                 * so publish the remote offer's extmap policy first. */
                apply_controlled_remote_offer(
                    ctx, peer, signal, playout_delay_requested);
            }
            bool playout_delay_accepted =
                role == WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING &&
                playout_delay_offered &&
                webrtc_playout_delay_answer_accepts(
                    signal->data, signal->size, playout_delay_extension_id);
            esp_peer_msg_t message = {
                .type = signal->type == WEBRTC_SIGNAL_SDP ? ESP_PEER_MSG_TYPE_SDP :
                        ESP_PEER_MSG_TYPE_CANDIDATE,
                .data = (uint8_t *)signal->data,
                .size = (int)signal->size,
            };
            int ret = esp_peer_send_msg(peer, &message);
            if (ret != ESP_PEER_ERR_NONE) {
                ESP_LOGW(TAG, "esp_peer_send_msg type=%d failed: %d", signal->type, ret);
                portENTER_CRITICAL(&ctx->lock);
                ctx->stats.signaling_messages_dropped++;
                portEXIT_CRITICAL(&ctx->lock);
                if (signal->type == WEBRTC_SIGNAL_SDP &&
                    role != WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING) {
                    clear_controlled_offer_after_send_failure(
                        ctx, peer, signal);
                }
            } else if (signal->type == WEBRTC_SIGNAL_SDP &&
                       role == WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING) {
                bool applied = false;
                portENTER_CRITICAL(&ctx->lock);
                if (ctx->peer == peer && ctx->session_active &&
                    signal->session_id == ctx->session_id &&
                    signal->generation == ctx->generation &&
                    signal->session_id == ctx->active_peer_session &&
                    signal->generation == ctx->active_peer_generation &&
                    ctx->active_playout_delay_generation == signal->generation) {
                    ctx->active_playout_delay_negotiated =
                        playout_delay_accepted;
                    ctx->stats.rtp_playout_delay_negotiated =
                        playout_delay_accepted;
                    if (playout_delay_offered && !playout_delay_accepted) {
                        ctx->stats.rtp_playout_delay_answer_rejections++;
                    }
                    applied = true;
                }
                portEXIT_CRITICAL(&ctx->lock);
                if (applied && playout_delay_offered) {
                    ESP_LOGI(TAG,
                             "RTP playout-delay answer extmap=%u accepted=%d",
                             playout_delay_extension_id,
                             playout_delay_accepted);
                }
            }
        }
        free_inbound_signal(ctx, signal);
    }
}

static void record_duration(webrtc_transport_duration_hist_t *h, int64_t us)
{
    if (us < 0) return;
    h->count++;
    h->over_10ms += us >= 10000;
    h->over_25ms += us >= 25000;
    h->over_50ms += us >= 50000;
    if ((uint64_t)us > h->max_us) h->max_us = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

static void record_peer_pass(webrtc_transport_peer_pass_t *p, int64_t us)
{
    if (us < 0) return;
    p->count++;
    p->sum_us += (uint64_t)us;
    p->over_16ms += us > 1000000 / 60;
    if ((uint64_t)us > p->max_us) p->max_us = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

/* Called before last_sent_au_generation moves to this frame's peer. */
static void record_send_intervals_locked(transport_ctx_t *ctx, uint32_t generation,
                                         int64_t encode_done_us, int64_t send_start_us)
{
    if (generation == ctx->last_sent_au_generation && ctx->last_sent_start_us > 0) {
        if (encode_done_us > 0 && ctx->last_sent_encode_done_us > 0) {
            record_duration(&ctx->stats.video_encode_interval,
                            encode_done_us - ctx->last_sent_encode_done_us);
        }
        record_duration(&ctx->stats.video_send_interval,
                        send_start_us - ctx->last_sent_start_us);
    }
    ctx->last_sent_encode_done_us = encode_done_us;
    ctx->last_sent_start_us = send_start_us;
}

static void send_next_access_unit(transport_ctx_t *ctx, bool between_steps)
{
    bool connected = false;
    portENTER_CRITICAL(&ctx->lock);
    connected = ctx->peer_connected && ctx->peer;
    portEXIT_CRITICAL(&ctx->lock);
    if (!connected) {
        return;
    }

    int index = take_next_ready_slot(ctx, between_steps);
    if (index < 0) {
        return;
    }
    int64_t peer_take_us = esp_timer_get_time();
    au_slot_t *slot = &ctx->slots[index];
    portENTER_CRITICAL(&ctx->lock);
    if (slot->au_copy_ready_us > 0) {
        record_duration(&ctx->stats.au_take_wait, peer_take_us - slot->au_copy_ready_us);
    }
    portEXIT_CRITICAL(&ctx->lock);
    bool drop_for_sync = false;
    bool decodable_sync = false;
    uint32_t send_generation = 0;
    uint32_t send_video_epoch = 0;
    uint32_t sync_idr_generation = 0;
    uint32_t network_timing_epoch = 0;
    uint32_t network_rtp_timestamp = 0;
    int64_t network_capture_eof_us = 0;
    portENTER_CRITICAL(&ctx->lock);
    ctx->active_send_trace.active = false;
    send_generation = ctx->active_peer_generation;
    send_video_epoch = slot->video_epoch;
    sync_idr_generation = slot->sync_idr_generation;
    decodable_sync = slot_is_decodable_sync(slot, send_generation,
                                            ctx->video_epoch);
    if (slot_needs_reference_sync(ctx, slot)) {
        /* Queue overflow or send failure lost a reference picture. Never send
         * its dependent P frames; request a new IDR on the encoder owner. */
        request_video_recovery_locked(ctx);
    }
    if (slot->generation != ctx->active_peer_generation ||
        slot->video_epoch != ctx->video_epoch) {
        drop_for_sync = true;
    } else if (ctx->waiting_for_sync) {
        if (!decodable_sync) {
            drop_for_sync = true;
            ctx->stats.access_units_waiting_for_sync++;
        }
    }
    if (!drop_for_sync && slot->capture_eof_us > 0) {
        /* The pinned esp_peer implementation invokes the sender RTP
         * transformer synchronously inside esp_peer_send_video().  Keep this
         * trace valid only across that call; the missing counters make a
         * changed callback contract visible after a dependency update. */
        ctx->active_send_trace = (active_send_trace_t) {
            .active = true,
            .telemetry_epoch = slot->telemetry_epoch,
            .capture_sequence = slot->capture_sequence,
            .expected_rtp_timestamp = (slot->pts_90khz / 90U) * 90U,
            .capture_eof_us = slot->capture_eof_us,
            .ppa_done_us = slot->ppa_done_us,
            .encode_start_us = slot->encode_start_us,
            .encode_done_us = slot->encode_done_us,
            .au_copy_ready_us = slot->au_copy_ready_us,
            .peer_take_us = peer_take_us,
        };
        network_timing_epoch = slot->telemetry_epoch;
        network_rtp_timestamp = (slot->pts_90khz / 90U) * 90U;
        network_capture_eof_us = slot->capture_eof_us;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (drop_for_sync) {
        release_sending_slot(ctx, index);
        return;
    }

    /* esp_peer takes video PTS in milliseconds and its H264 RTP layer
     * multiplies that value by 90.  Keep the public/producer boundary in 90 kHz. */
    esp_peer_video_frame_t frame = {
        .pts = slot->pts_90khz / 90U,
        .data = slot->data,
        .size = (int)slot->size,
    };
    webrtc_network_timing_begin(network_timing_epoch, network_rtp_timestamp,
                                network_capture_eof_us);
    webrtc_transport_network_timing_stats_t cost_before, cost_after;
    webrtc_transport_stage_age_summary_t unused_stage_age;
    webrtc_network_timing_snapshot(&cost_before, &unused_stage_age);
    const int64_t send_video_start_us = esp_timer_get_time();
    ctx->sending_video = true;
    int ret = esp_peer_send_video(ctx->peer, &frame);
    ctx->sending_video = false;
    int64_t send_video_return_us = esp_timer_get_time();
    jetkvm_virtual_media_note_video_frame();
    webrtc_network_timing_snapshot(&cost_after, &unused_stage_age);
    bool sync_idr_sent = false;
    bool sync_idr_failed = false;
    portENTER_CRITICAL(&ctx->lock);
    if (ret == ESP_PEER_ERR_NONE) {
        const size_t bucket = slot->size < 16384 ? 0 : slot->size < 49152 ? 1 :
                              slot->size < 98304 ? 2 : 3;
        webrtc_transport_send_cost_t *cost = &ctx->stats.video_send_by_size[bucket];
        const uint64_t send_us = (uint64_t)(send_video_return_us - send_video_start_us);
        cost->count++;
        cost->bytes += slot->size;
        cost->packets += cost_after.lwip_sendto.count - cost_before.lwip_sendto.count;
        cost->total_us += send_us;
        cost->srtp_us += cost_after.srtp_protect.total_us - cost_before.srtp_protect.total_us;
        cost->sendto_us += cost_after.lwip_sendto.total_us - cost_before.lwip_sendto.total_us;
        if (send_us > cost->max_us) cost->max_us = (uint32_t)send_us;
        ctx->stats.access_units_sent++;
        record_send_intervals_locked(ctx, send_generation, slot->encode_done_us,
                                     send_video_start_us);
        ctx->last_sent_au_order = slot->order;
        ctx->last_sent_au_generation = send_generation;
        ctx->stats.access_unit_bytes_sent += slot->size;
        aggregate_active_trace_locked(ctx, send_video_return_us);
        /* A mode change during the send starts a new epoch that still
         * waits for its own sync IDR. */
        if (decodable_sync && send_generation == ctx->active_peer_generation &&
            send_video_epoch == ctx->video_epoch) {
            ctx->waiting_for_sync = false;
            ctx->stats.waiting_for_sync_frame = false;
            if (ctx->sync_idr_pending_generation == send_generation) {
                ctx->sync_idr_pending_generation = 0;
                ctx->stats.sync_idr_pending_generation = 0;
            }
            if (sync_idr_generation != 0 &&
                ctx->sync_idr_committed_generation == sync_idr_generation) {
                ctx->sync_idr_committed_generation = 0;
                ctx->stats.sync_idr_committed_generation = 0;
                ctx->stats.sync_idr_successes++;
                sync_idr_sent = true;
            }
        }
    } else if (ret == ESP_PEER_ERR_WOULD_BLOCK) {
        ctx->stats.access_units_would_block++;
        sync_idr_failed = send_video_epoch == ctx->video_epoch &&
            fail_sync_idr_locked(ctx, sync_idr_generation, true);
    } else {
        ctx->stats.access_units_send_errors++;
        sync_idr_failed = send_video_epoch == ctx->video_epoch &&
            fail_sync_idr_locked(ctx, sync_idr_generation, true);
    }
    ctx->active_send_trace.active = false;
    TaskHandle_t waiter = free_sending_slot_locked(ctx, slot);
    portEXIT_CRITICAL(&ctx->lock);
    if (waiter) {
        xTaskNotifyGive(waiter);
    }
    if (ret != ESP_PEER_ERR_NONE) {
        webrtc_network_timing_cancel(network_timing_epoch,
                                     network_rtp_timestamp,
                                     network_capture_eof_us);
    }
    if (sync_idr_sent) {
        ESP_LOGI(TAG, "sync IDR sent generation=%" PRIu32,
                 sync_idr_generation);
    } else if (sync_idr_failed) {
        ESP_LOGW(TAG, "sync IDR send failed generation=%" PRIu32 " ret=%d; requeued",
                 sync_idr_generation, ret);
    }
}

static void expire_stale_session(transport_ctx_t *ctx, int64_t now_us)
{
    portENTER_CRITICAL(&ctx->lock);
    bool expired = ctx->session_active &&
        ctx->session_role != WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED &&
        !jetkvm_access_epoch_valid(ctx->local_access_epoch);
    portEXIT_CRITICAL(&ctx->lock);
    if (expired) webrtc_transport_revoke_local();
    portENTER_CRITICAL(&ctx->lock);
    /* Only the diagnostic HTTP transport requires regular event polls.
     * WebSocket liveness belongs to its socket/heartbeat lifecycle: a busy
     * HTTPD task must not make the peer task expire an otherwise live socket. */
    if (ctx->session_active &&
        ctx->session_role == WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING &&
        ctx->session_last_poll_us > 0 &&
        now_us - ctx->session_last_poll_us >
            (int64_t)CONFIG_WEBRTC_TRANSPORT_SESSION_TIMEOUT_MS * 1000) {
        ESP_EARLY_LOGW(TAG, "browser signaling session timed out");
        ctx->session_active = false;
        ctx->stats.browser_session_active = false;
        next_generation_locked(ctx);
        ctx->rebuild_requested = true;
    }
    portEXIT_CRITICAL(&ctx->lock);
}

static TickType_t reboot_wait_ticks(int64_t remaining_us)
{
    uint32_t remaining_ms = remaining_us > 0 ?
        (uint32_t)(((uint64_t)remaining_us + 999ULL) / 1000ULL) : 1U;
    TickType_t ticks = pdMS_TO_TICKS(remaining_ms);
    return ticks == 0 ? 1 : ticks;
}

/* A dispatched reboot happens unless its queued response or willReboot is
 * lost (webrtc_reboot_slot_message_lost()); otherwise the browser was told
 * it would. Only the transport's stop cancels it too; a closing peer does
 * not. */
static void reboot_task(void *arg)
{
    transport_ctx_t *ctx = arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        webrtc_reboot_slot_t request = {0};
        portENTER_CRITICAL(&ctx->lock);
        bool stop = ctx->reboot_task_stop_requested;
        bool ready = !stop && ctx->reboot_slot.dispatch_ready;
        if (ready) {
            request = ctx->reboot_slot;
            ctx->reboot_executing = true;
        }
        portEXIT_CRITICAL(&ctx->lock);
        if (!ready) {
            if (stop) {
                break;
            }
            continue;
        }

        /* The wait is measured from the slot's times and re-checked after
         * every wake, so a short first tick cannot cut it. */
        bool cancelled = false;
        for (;;) {
            portENTER_CRITICAL(&ctx->lock);
            stop = ctx->reboot_task_stop_requested;
            cancelled = !ctx->reboot_slot.dispatch_ready;
            int64_t wait_us = webrtc_reboot_wait_us(&ctx->reboot_slot,
                                                    esp_timer_get_time());
            bool restart = !stop && !cancelled && wait_us == 0;
            if (restart) {
                ctx->stats.reboot_executions++;
            }
            portEXIT_CRITICAL(&ctx->lock);
            if (stop || cancelled || restart) {
                break;
            }
            (void)ulTaskNotifyTake(pdTRUE, reboot_wait_ticks(wait_us));
        }
        if (stop) {
            break;
        }
        if (cancelled) {
            continue;
        }
        ESP_LOGW(TAG,
                 "executing accepted reboot session=%" PRIu32
                 " generation=%" PRIu32 " force=%d",
                 request.session_id, request.generation, request.force);
        esp_restart();
    }

    portENTER_CRITICAL(&ctx->lock);
    ctx->reboot_available = false;
    ctx->reboot_executing = false;
    webrtc_reboot_slot_clear(&ctx->reboot_slot);
    ctx->reboot_task = NULL;
    portEXIT_CRITICAL(&ctx->lock);
    xSemaphoreGive(ctx->reboot_task_done);
    vTaskDelete(NULL);
}

/* The receive and the data-channel output of one pass each take 10 to 20 ms
 * on the Wi-Fi board while a terminal and a file transfer run. A frame that
 * the HDMI encoder finished during one of them goes out right after it.
 * Behind both, it waits longer than a frame time, and the encoder then finds
 * no free slot for the frame after the next, which is lost. Two kinds of
 * frame are left for the top of the pass. A keyframe's send takes about
 * 60 ms, and the top reads the input and publishes the keyboard state first.
 * A service video frame is not lost when it waits, because its producer waits
 * for the slot, and its send between the steps takes the time from the
 * receive: at 2160 lines, 9 of 128 keys were late instead of 1 to 4. Returns
 * the time taken, in microseconds. */
static int64_t send_frame_between_steps(transport_ctx_t *ctx)
{
    const int64_t start_us = esp_timer_get_time();
    send_next_access_unit(ctx, true);
    return esp_timer_get_time() - start_us;
}

/* Sends a keyboard change that the HID worker reported to the browser.
 * Returns the time taken, in microseconds. */
static int64_t publish_keyboard_change(transport_ctx_t *ctx)
{
    if (!atomic_load(&ctx->rpc_event_hid_dirty)) {
        return 0;
    }
    const int64_t start_us = esp_timer_get_time();
    poll_rpc_hid_state(ctx, start_us);
    send_one_pending_rpc_event(ctx, start_us);
    send_one_pending_hidrpc_state(ctx, start_us);
    return esp_timer_get_time() - start_us;
}

/* A receive slice stops at a keyboard change, so that the key's report goes
 * out before the slice's other datagrams. This sends the report and reads
 * the attempts that the slice had left, again when that rest stops at the
 * next change. Without the rest, a pass reads fewer datagrams the more the
 * keys change. A service video pass has a frame of 21 ms and one receive
 * after it: beside a terminal, a file transfer and a key and a mouse move
 * every 100 ms it then reads 192 datagrams per second instead of 212, the
 * socket stays at its limit of 32 datagrams and drops 5 more per second,
 * and a key waits 86 ms in it instead of 40 ms. refused and attempts_left
 * are what the slice's end returned.
 * Returns whether the last slice refused an attempt, so packets may still
 * wait in the socket, and adds the time of the reports to *keyboard_us. */
static bool receive_rest_after_key(transport_ctx_t *ctx, bool refused,
                                   uint32_t attempts_left,
                                   int64_t *keyboard_us)
{
    /* Each further slice reads one datagram at least before it can stop,
     * so attempts_left falls every time. */
    while (attempts_left > 0U) {
        *keyboard_us += publish_keyboard_change(ctx);
        webrtc_peer_udp_rx_slice_begin(true, attempts_left);
        (void)esp_peer_main_loop(ctx->peer);
        refused = webrtc_peer_udp_rx_slice_end(&attempts_left);
    }
    return refused;
}

static void send_pending_audio(transport_ctx_t *ctx)
{
    bool connected;
    bool paused;
    portENTER_CRITICAL(&ctx->lock);
    connected = ctx->peer_connected;
    paused = (ctx->video_pause_sources & WEBRTC_AUDIO_PAUSE_SOURCES) != 0;
    portEXIT_CRITICAL(&ctx->lock);
    /* The audio worker encodes only while a peer takes its frames. A hold
     * that stops the sound is, for the worker, a peer that takes none: it
     * drops the frames that wait, keeps reading USB, and starts a new
     * capture segment when the hold ends. The browser gets no frame during
     * the hold and plays silence, and then gets sound of that time. */
    const bool takes_audio = connected && !paused;
    jetkvm_audio_set_peer_connected(takes_audio);
    if (!ctx->peer || !takes_audio) return;
    uint8_t packet[JETKVM_AUDIO_PACKET_MAX];
    esp_peer_audio_frame_t frame = {.data = packet};
    /* Drain fairly before and after video; the queue also absorbs an IDR send. */
    for (unsigned i = 0; i < 4 && jetkvm_audio_pop(packet, &frame.size, &frame.pts); ++i) {
        (void)esp_peer_send_audio(ctx->peer, &frame);
    }
}

/* Runs of each core's idle task, counted by its hook, and whether the core
 * has the hook. */
static atomic_uint s_idle_runs[portNUM_PROCESSORS];
static bool s_idle_counted[portNUM_PROCESSORS];
_Static_assert(portNUM_PROCESSORS <= WEBRTC_TRANSPORT_IDLE_CORES,
               "the stats report the idle runs of every core");

static bool count_idle_run(void)
{
    atomic_fetch_add_explicit(&s_idle_runs[xPortGetCoreID()], 1U, memory_order_relaxed);
    return true;
}

/* Registers the hooks once. On a core whose hook has no place in ESP-IDF's
 * table the runs are not counted, and the peer task keeps that core. */
static void count_idle_runs(void)
{
    static bool registered;
    if (registered) {
        return;
    }
    registered = true;
    for (UBaseType_t core = 0; core < portNUM_PROCESSORS; ++core) {
        s_idle_counted[core] =
            esp_register_freertos_idle_hook_for_cpu(count_idle_run, core) == ESP_OK;
        if (!s_idle_counted[core]) {
            ESP_LOGW(TAG, "no idle hook on core %u: the peer task does not make room for its idle task",
                     (unsigned)core);
        }
    }
}

static void peer_task(void *arg)
{
    transport_ctx_t *ctx = arg;
    int64_t next_stack_sample_us = 0;
    /* Per core, because a peer task without a fixed core can change it. */
    unsigned idle_runs_seen[portNUM_PROCESSORS] = {0};
    int64_t idle_seen_us[portNUM_PROCESSORS];
    /* The hooks' 32-bit counts, added up into the stats' 64-bit ones: the
     * task reads them at least every 10 ms, long before one wraps. */
    unsigned idle_runs_added[portNUM_PROCESSORS] = {0};
    count_idle_runs();
    for (UBaseType_t core = 0; core < portNUM_PROCESSORS; ++core) {
        idle_seen_us[core] = esp_timer_get_time();
    }
    portENTER_CRITICAL(&ctx->lock);
    for (UBaseType_t core = 0; core < portNUM_PROCESSORS; ++core) {
        ctx->stats.idle_counted[core] = s_idle_counted[core];
    }
    portEXIT_CRITICAL(&ctx->lock);
    for (;;) {
        int64_t phase_us[WEBRTC_PEER_PHASE_COUNT + 1];
        phase_us[WEBRTC_PEER_PHASE_WAIT] = esp_timer_get_time();
        /* CONFIG_FREERTOS_HZ is 100 in the acceptance firmware.  A 5 ms
         * conversion becomes zero ticks and turns the idle peer into a busy
         * loop that starves IDLE1/WDT.  One 10 ms tick is still woken early by
         * every frame or signaling notification. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        /* A pass starts at the wake: the tick that a starved IDLE task gets
         * below holds the frame back as well. */
        const int64_t woke_us = esp_timer_get_time();
        int64_t now_us = woke_us;
        const BaseType_t core = xPortGetCoreID();
        const unsigned idle_runs = atomic_load(&s_idle_runs[core]);
        if (idle_runs != idle_runs_seen[core]) {
            idle_runs_seen[core] = idle_runs;
            idle_seen_us[core] = now_us;
        } else if (s_idle_counted[core] &&
                   now_us - idle_seen_us[core] >= PEER_TASK_IDLE_STARVED_US) {
            vTaskDelay(1);
            now_us = esp_timer_get_time();
        }
        phase_us[WEBRTC_PEER_PHASE_CONTROL] = now_us;
        expire_stale_session(ctx, now_us);

        bool stop = false;
        bool rebuild = false;
        portENTER_CRITICAL(&ctx->lock);
        stop = ctx->stop_requested;
        rebuild = ctx->rebuild_requested;
        if (rebuild) {
            ctx->rebuild_requested = false;
        }
        portEXIT_CRITICAL(&ctx->lock);

        if (now_us >= next_stack_sample_us) {
            /* Scanning unused stack bytes is diagnostic work; keep it out of
             * the media critical section and out of the per-frame fast path. */
            uint32_t watermark = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
            portENTER_CRITICAL(&ctx->lock);
            ctx->stats.peer_task_stack_high_watermark = watermark;
            portEXIT_CRITICAL(&ctx->lock);
            next_stack_sample_us = now_us + 1000000;
        }
        if (stop) {
            break;
        }
        if (rebuild) {
            send_terminal_video_event(ctx);
            close_peer(ctx);
        }

        dispatch_pending_cloud_invalidation(ctx);

        /* close_peer can take long enough for HTTP signaling or HDMI
         * mode state to advance.  Refresh every open input after teardown;
         * never allocate a peer from the pre-close generation snapshot. */
        bool can_open = false;
        uint32_t session_id = 0;
        uint32_t generation = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        uint16_t fps = 0;
        webrtc_transport_session_role_t role =
            WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING;
        now_us = esp_timer_get_time();
        portENTER_CRITICAL(&ctx->lock);
        /* A peer opened while the video owner waits for a mode teardown
         * would compete with the next mode's memory admission. */
        can_open = !ctx->peer && !ctx->rebuild_requested &&
                   ctx->session_active && !ctx->mode_teardown_waiters &&
                   now_us >= ctx->reconnect_not_before_us;
        session_id = ctx->session_id;
        generation = ctx->generation;
        width = ctx->width;
        height = ctx->height;
        fps = ctx->fps;
        if (!ctx->mode_valid) {
            width = 1280;
            height = 720;
            fps = 30;
        }
        role = ctx->session_role;
        portEXIT_CRITICAL(&ctx->lock);
        if (can_open) {
            esp_err_t open_ret = open_peer(
                ctx, session_id, generation, width, height, fps, role);
            /* A video transaction is normal scheduling backpressure. Leave
             * the session intact and retry on the next peer-task iteration. */
            if (open_ret != ESP_OK && open_ret != ESP_ERR_NOT_FINISHED) {
                portENTER_CRITICAL(&ctx->lock);
                if (open_ret != ESP_ERR_INVALID_STATE) {
                    const uint32_t retry_delay_ms =
                        open_ret == ESP_ERR_NO_MEM ?
                            JETKVM_MEMORY_RETRY_DELAY_MS :
                            CONFIG_WEBRTC_TRANSPORT_RECONNECT_DELAY_MS;
                    ctx->reconnect_not_before_us = esp_timer_get_time() +
                        (int64_t)retry_delay_ms * 1000;
                }
                ctx->rebuild_requested = true;
                portEXIT_CRITICAL(&ctx->lock);
            }
        }
        process_inbound(ctx);
        int64_t keyboard_before_frame_us = 0;
        int64_t pre_frame_main_loop_us =
            receive_before_frame(ctx, &keyboard_before_frame_us);
        /* A keyboard change wakes this task; publish it ahead of the frame so
         * the browser's keys-down view does not wait behind a video send. */
        keyboard_before_frame_us += publish_keyboard_change(ctx);
        /* Do not put an agent socket receive wait in front of the encoded
         * frame.  esp_peer_send_video packetizes/queues first; main_loop then
         * services ICE/DTLS/SRTP and drains network work. */
        phase_us[WEBRTC_PEER_PHASE_MEDIA] = esp_timer_get_time();
        send_pending_audio(ctx);
        send_next_access_unit(ctx, false);
        send_pending_audio(ctx);
        phase_us[WEBRTC_PEER_PHASE_MAIN_LOOP] = esp_timer_get_time();
        bool signaling_needs_receive_fairness = false;
        bool media_connected = false;
        bool service_video = false;
        portENTER_CRITICAL(&ctx->lock);
        signaling_needs_receive_fairness =
            ctx->stats.peer_state == WEBRTC_TRANSPORT_PEER_GATHERING;
        media_connected = ctx->peer_connected;
        service_video = service_video_locked(ctx);
        portEXIT_CRITICAL(&ctx->lock);
        bool more_to_receive = false;
        int64_t keyboard_after_receive_us = 0;
        if (ctx->peer) {
            /* esp_peer_main_loop() drains currently readable UDP packets
             * inside one call. Call it once per task slice (once more when
             * that left packets in the socket, and once in
             * receive_before_frame when a frame is due); receive fairness is
             * supplied by the udp_socket_recvfrom_poll_allowed() transport
             * hook rather than by repeatedly entering the peer main loop.
             *
             * During gathering, Chrome can start sending STUN before its
             * trickled candidate reaches this task. An unbounded vendor drain
             * then consumes every periodic STUN request and never returns to
             * process_inbound(), so the candidate can never form a pair. One
             * receive attempt is enough to yield back to signaling. Once the
             * candidate synchronously moves the peer to pairing, the normal
             * blocking DTLS path is restored. */
            /* After DTLS/SCTP complete, a tick-rounded blocking receive can
             * throttle 60 fps video below 50 fps. Poll a bounded number of
             * packets, then return to frame/control work. Handshakes retain
             * their blocking receive behavior. */
            webrtc_peer_udp_rx_slice_begin(
                signaling_needs_receive_fairness || media_connected,
                media_connected ? 8U : 1U);
            int ret = esp_peer_main_loop(ctx->peer);
            uint32_t attempts_left = 0U;
            const bool refused = webrtc_peer_udp_rx_slice_end(&attempts_left);
            more_to_receive = receive_rest_after_key(ctx, refused, attempts_left,
                                                     &keyboard_after_receive_us) &&
                              media_connected && !service_video;
            if (ret != ESP_PEER_ERR_NONE &&
                ret != ESP_PEER_ERR_WOULD_BLOCK) {
                ESP_LOGW(TAG, "esp_peer_main_loop returned %d", ret);
            }
        }
        release_completed_peer_tls_reservation(ctx);
        /* A key that the receive delivered last: the browser has its report
         * before the frame. */
        keyboard_after_receive_us += publish_keyboard_change(ctx);
        int64_t frame_after_receive_us = send_frame_between_steps(ctx);
        if (more_to_receive) {
            /* The receive stopped at its 8 packets with more in the socket:
             * a file transfer fills it ahead of the keys. An HDMI pass
             * seldom starts with a frame, because its frames go out between
             * the steps, so it seldom has the receive before the frame. Read
             * once more instead, so that a key does not wait a whole pass
             * for its turn. A service video pass keeps the receive before
             * its frame; with a second one here its frames waited, and
             * service video of 2160 lines fell from 23 to 17 fps. */
            webrtc_peer_udp_rx_slice_begin(true, 8U);
            (void)esp_peer_main_loop(ctx->peer);
            uint32_t attempts_left = 0U;
            const bool refused = webrtc_peer_udp_rx_slice_end(&attempts_left);
            (void)receive_rest_after_key(ctx, refused, attempts_left,
                                         &keyboard_after_receive_us);
            keyboard_after_receive_us += publish_keyboard_change(ctx);
            frame_after_receive_us += send_frame_between_steps(ctx);
        }
        now_us = esp_timer_get_time();
        phase_us[WEBRTC_PEER_PHASE_RPC] = now_us;
        send_queued_data_channel_messages(ctx);
        drain_pending_rpc_notifications(ctx);
        send_pending_rpc_worker_replies(ctx);
        poll_rpc_hid_state(ctx, now_us);
        poll_rpc_network_state(ctx, now_us);
        send_one_pending_rpc_event(ctx, now_us);
        send_one_pending_hidrpc_state(ctx, now_us);
        /* After the replies and events, which a full send buffer would
         * otherwise hold back. */
        send_guest_channel_output(ctx, now_us);
        const int64_t frame_after_output_us = send_frame_between_steps(ctx);
        phase_us[WEBRTC_PEER_PHASE_COUNT] = esp_timer_get_time();
        unsigned idle_runs_new[portNUM_PROCESSORS];
        for (UBaseType_t c = 0; c < portNUM_PROCESSORS; ++c) {
            const unsigned runs = atomic_load(&s_idle_runs[c]);
            idle_runs_new[c] = runs - idle_runs_added[c];
            idle_runs_added[c] = runs;
        }
        portENTER_CRITICAL(&ctx->lock);
        for (UBaseType_t c = 0; c < portNUM_PROCESSORS; ++c) {
            ctx->stats.idle_runs[c] += idle_runs_new[c];
        }
        record_peer_pass(&ctx->stats.peer_pass,
                         phase_us[WEBRTC_PEER_PHASE_COUNT] - woke_us);
        for (int phase = 0; phase < WEBRTC_PEER_PHASE_COUNT; ++phase) {
            int64_t duration_us = phase_us[phase + 1] - phase_us[phase];
            /* The pre-frame receive runs inside the control phase but is
             * main-loop work, the frames sent between the steps are media
             * work, and the keyboard changes sent before the frame and
             * after a receive are RPC work. */
            if (phase == WEBRTC_PEER_PHASE_CONTROL) {
                duration_us -= pre_frame_main_loop_us + keyboard_before_frame_us;
            } else if (phase == WEBRTC_PEER_PHASE_MEDIA) {
                duration_us += frame_after_receive_us + frame_after_output_us;
            } else if (phase == WEBRTC_PEER_PHASE_MAIN_LOOP) {
                duration_us += pre_frame_main_loop_us - frame_after_receive_us -
                               keyboard_after_receive_us;
            } else if (phase == WEBRTC_PEER_PHASE_RPC) {
                duration_us += keyboard_before_frame_us +
                               keyboard_after_receive_us - frame_after_output_us;
            }
            record_duration(&ctx->stats.peer_phase[phase], duration_us);
        }
        portEXIT_CRITICAL(&ctx->lock);
    }

    close_peer(ctx);
    rpc_worker_cancel_replies();
    flush_inbound(ctx);
    portENTER_CRITICAL(&ctx->lock);
    ctx->task = NULL;
    portEXIT_CRITICAL(&ctx->lock);
    /* A datagram's wake that read the handle before it was cleared ends
     * first; a later one finds no handle. */
    while (atomic_load(&s_datagram_wakers) != 0U) {
        vTaskDelay(1);
    }
    xSemaphoreGive(ctx->task_done);
    vTaskDelete(NULL);
}

esp_err_t webrtc_transport_start(void)
{
    transport_ctx_t *ctx = &s_ctx;
    webrtc_peer_udp_timeout_override_init();
    /* esp_peer's default peer logs every retransmitted RTP packet at INFO
     * ("PEER_DEF: r <seq>"). On a lossy link that is hundreds of lines a
     * second on the USB console, which blocks the logging tasks long enough
     * to trigger the task watchdog. Its warnings and errors stay. */
    esp_log_level_set("PEER_DEF", ESP_LOG_WARN);
    /* From esp_peer's default offer (mbedTLS's own order) Chrome takes
     * AES-256-CBC-SHA. mbedTLS checks a CBC record's MAC in constant time,
     * finishing SHA-1 once per possible padding length: 5 ms a full record,
     * which held a file drop to the guest at 100 kB/s. AES-128-GCM takes
     * 0.5 ms, and every WebRTC endpoint supports it (RFC 8827 6.5). */
    (void)esp_peer_set_dtls_cipher_pref(ESP_PEER_DTLS_CIPHER_AES_GCM);
    portENTER_CRITICAL(&ctx->lock);
    if (ctx->initializing || ctx->stopping) {
        portEXIT_CRITICAL(&ctx->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->started) {
        portEXIT_CRITICAL(&ctx->lock);
        return ESP_OK;
    }
    if (ctx->rpc_scratch != NULL) {
        portEXIT_CRITICAL(&ctx->lock);
        return ESP_ERR_INVALID_STATE;
    }
    ctx->initializing = true;
    portEXIT_CRITICAL(&ctx->lock);
    rpc_worker_cancel_replies();
    p4_device_set_reboot_available(false);

    esp_err_t cert_err = prime_dtls_cert_cache();
    if (cert_err != ESP_OK) {
        portENTER_CRITICAL(&ctx->lock);
        ctx->initializing = false;
        portEXIT_CRITICAL(&ctx->lock);
        return cert_err;
    }

    ctx->rpc_scratch = heap_caps_aligned_alloc(
        64U, JETKVM_MEMORY_RPC_SCRATCH_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx->rpc_scratch == NULL) {
        ESP_LOGE(TAG, "failed to allocate %u-byte RPC scratch",
                 (unsigned)JETKVM_MEMORY_RPC_SCRATCH_BYTES);
        goto no_mem;
    }

    for (size_t i = 0; i < CONFIG_WEBRTC_TRANSPORT_AU_SLOTS; ++i) {
        ctx->slots[i].own = heap_caps_aligned_alloc(
            128, AU_SLOT_ALLOCATION_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ctx->slots[i].data = ctx->slots[i].own;
        ctx->slots[i].capacity =
            CONFIG_WEBRTC_TRANSPORT_MAX_AU_BYTES & ~(size_t)127U;
        if (!ctx->slots[i].data) {
            ESP_LOGE(TAG, "failed to allocate AU slot %u (%u bytes)",
                     (unsigned)i, (unsigned)CONFIG_WEBRTC_TRANSPORT_MAX_AU_BYTES);
            goto no_mem;
        }
        /* Heap reuse may contain dirty CPU cache lines. Establish clean
         * DMA output ownership once, before the pool becomes visible. */
        esp_err_t cache_ret = esp_cache_msync(ctx->slots[i].data,
            AU_SLOT_ALLOCATION_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        if (cache_ret != ESP_OK) {
            ESP_LOGE(TAG, "failed to clean AU slot %u: %s",
                     (unsigned)i, esp_err_to_name(cache_ret));
            goto no_mem;
        }
        ctx->slots[i].cpu_dirty_bytes = 0;
        ctx->slots[i].state = AU_SLOT_FREE;
    }
    ctx->inbound_queue = xQueueCreate(SIGNAL_QUEUE_DEPTH, sizeof(inbound_signal_t *));
    ctx->task_done = xSemaphoreCreateBinary();
    ctx->reboot_task_done = xSemaphoreCreateBinary();
    if (!ctx->inbound_queue || !ctx->task_done || !ctx->reboot_task_done) {
        goto no_mem;
    }
    /* Prime detailed telemetry before workers/capture start. Live polls use
     * this sample until idle or an explicit detailed request refreshes it. */
    jetkvm_memory_policy_snapshot_t initial_memory = {0};
    jetkvm_memory_policy_get_snapshot(&initial_memory);
    portENTER_CRITICAL(&ctx->lock);
    memset(&ctx->stats, 0, sizeof(ctx->stats));
    ctx->sync_idr_pending_generation = 0;
    ctx->sync_idr_inflight_generation = 0;
    ctx->sync_idr_committed_generation = 0;
    ctx->sync_idr_last_requested_generation = 0;
    ctx->video_paused = false;
    ctx->video_pause_sources = 0;
    ctx->last_sent_au_order = 0;
    ctx->last_sent_au_generation = 0;
    ctx->reboot_available = false;
    ctx->reboot_task_stop_requested = false;
    ctx->reboot_executing = false;
    webrtc_reboot_slot_clear(&ctx->reboot_slot);
    ctx->started = true;
    ctx->stop_requested = false;
    ctx->stats.started = true;
    ctx->stats.peer_state = WEBRTC_TRANSPORT_PEER_WAITING;
    portEXIT_CRITICAL(&ctx->lock);

    portENTER_CRITICAL(&ctx->lock);
    ctx->reboot_task_join_pending = true;
    portEXIT_CRITICAL(&ctx->lock);
    if (xTaskCreatePinnedToCore(
            reboot_task, "jetkvm_reboot", JETKVM_REBOOT_TASK_STACK, ctx,
            JETKVM_REBOOT_TASK_PRIORITY, &ctx->reboot_task, 0) != pdPASS) {
        portENTER_CRITICAL(&ctx->lock);
        ctx->reboot_task = NULL;
        ctx->reboot_task_join_pending = false;
        portEXIT_CRITICAL(&ctx->lock);
        ESP_LOGE(TAG, "failed to create reboot task");
        goto start_failed;
    }
    portENTER_CRITICAL(&ctx->lock);
    ctx->peer_task_join_pending = true;
    portEXIT_CRITICAL(&ctx->lock);
    if (xTaskCreatePinnedToCore(peer_task, "webrtc_peer",
                                CONFIG_WEBRTC_TRANSPORT_TASK_STACK, ctx,
                                CONFIG_WEBRTC_TRANSPORT_TASK_PRIORITY, &ctx->task,
                                CONFIG_WEBRTC_TRANSPORT_TASK_CORE < 0 ?
                                    tskNO_AFFINITY : CONFIG_WEBRTC_TRANSPORT_TASK_CORE) != pdPASS) {
        portENTER_CRITICAL(&ctx->lock);
        ctx->task = NULL;
        ctx->peer_task_join_pending = false;
        portEXIT_CRITICAL(&ctx->lock);
        ESP_LOGE(TAG, "failed to create peer task");
        goto start_failed;
    }
    esp_err_t err = webrtc_transport_http_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HTTP signaling server: %s", esp_err_to_name(err));
        goto start_failed;
    }
    portENTER_CRITICAL(&ctx->lock);
    bool reboot_ready = ctx->started && !ctx->stop_requested &&
                        ctx->task && ctx->reboot_task;
    ctx->reboot_available = reboot_ready;
    portEXIT_CRITICAL(&ctx->lock);
    if (!reboot_ready) {
        ESP_LOGE(TAG, "reboot worker unavailable after transport startup");
        goto start_failed;
    }
    p4_device_set_reboot_available(true);
    portENTER_CRITICAL(&ctx->lock);
    ctx->initializing = false;
    portEXIT_CRITICAL(&ctx->lock);
    ESP_LOGI(TAG, "local WebRTC page: http://<device-ip>:%d/webrtc",
             CONFIG_WEBRTC_TRANSPORT_HTTP_PORT);
    return ESP_OK;

start_failed:
    (void)webrtc_transport_stop_internal(true);
    return ESP_FAIL;

no_mem:
    if (ctx->inbound_queue) {
        vQueueDelete(ctx->inbound_queue);
        ctx->inbound_queue = NULL;
    }
    if (ctx->task_done) {
        vSemaphoreDelete(ctx->task_done);
        ctx->task_done = NULL;
    }
    if (ctx->reboot_task_done) {
        vSemaphoreDelete(ctx->reboot_task_done);
        ctx->reboot_task_done = NULL;
    }
    heap_caps_free(ctx->rpc_scratch);
    ctx->rpc_scratch = NULL;
    for (size_t i = 0; i < CONFIG_WEBRTC_TRANSPORT_AU_SLOTS; ++i) {
        heap_caps_free(ctx->slots[i].own);
        memset(&ctx->slots[i], 0, sizeof(ctx->slots[i]));
    }
    portENTER_CRITICAL(&ctx->lock);
    ctx->initializing = false;
    portEXIT_CRITICAL(&ctx->lock);
    return ESP_ERR_NO_MEM;
}

static esp_err_t webrtc_transport_stop_internal(bool startup_failure)
{
    transport_ctx_t *ctx = &s_ctx;
    owned_ice_server_list_t *session_ice_servers = NULL;
    portENTER_CRITICAL(&ctx->lock);
    if (ctx->stopping ||
        (startup_failure ? !ctx->initializing : ctx->initializing)) {
        portEXIT_CRITICAL(&ctx->lock);
        return ESP_ERR_INVALID_STATE;
    }
    const bool teardown_pending =
        ctx->rpc_scratch != NULL || ctx->task != NULL ||
        ctx->peer_task_join_pending || ctx->reboot_task != NULL ||
        ctx->reboot_task_join_pending || ctx->inbound_queue != NULL ||
        ctx->task_done != NULL || ctx->reboot_task_done != NULL;
    if (!ctx->started && !teardown_pending) {
        ctx->reboot_available = false;
        ctx->initializing = false;
        portEXIT_CRITICAL(&ctx->lock);
        rpc_worker_cancel_replies();
        p4_device_set_reboot_available(false);
        return ESP_OK;
    }
    /* A timeout leaves all task-owned resources intact.  Mark this attempt
     * so concurrent callers cannot race queue deletion, but allow a later
     * stop call to resume joining the same workers and finish teardown. */
    ctx->stopping = true;
    ctx->initializing = false;
    ctx->started = false;
    ctx->stop_requested = true;
    ctx->session_active = false;
    session_ice_servers = ctx->session_ice_servers;
    ctx->session_ice_servers = NULL;
    ctx->cloud_signal_cb = NULL;
    ctx->cloud_lifecycle_cb = NULL;
    ctx->cloud_signal_ctx = NULL;
    clear_cloud_invalidation_locked(ctx);
    ctx->stats.started = false;
    ctx->stats.browser_session_active = false;
    ctx->rebuild_requested = true;
    ctx->reboot_available = false;
    ctx->reboot_task_stop_requested = true;
    ctx->reboot_executing = false;
    webrtc_reboot_slot_clear(&ctx->reboot_slot);
    TaskHandle_t task = ctx->task;
    TaskHandle_t reboot_worker = ctx->reboot_task;
    bool peer_join_pending = ctx->peer_task_join_pending;
    bool reboot_join_pending = ctx->reboot_task_join_pending;
    portEXIT_CRITICAL(&ctx->lock);
    rpc_worker_cancel_replies();
    p4_device_set_reboot_available(false);

    owned_ice_server_list_release(ctx, session_ice_servers);

    webrtc_transport_http_stop();
    if (task) {
        xTaskNotifyGive(task);
    }
    if (reboot_worker) {
        xTaskNotifyGive(reboot_worker);
    }
    bool task_timeout = peer_join_pending &&
        xSemaphoreTake(ctx->task_done, pdMS_TO_TICKS(5000)) != pdTRUE;
    if (peer_join_pending && !task_timeout) {
        portENTER_CRITICAL(&ctx->lock);
        ctx->peer_task_join_pending = false;
        portEXIT_CRITICAL(&ctx->lock);
    }
    bool reboot_timeout = reboot_join_pending &&
        xSemaphoreTake(ctx->reboot_task_done, pdMS_TO_TICKS(5000)) != pdTRUE;
    if (reboot_join_pending && !reboot_timeout) {
        portENTER_CRITICAL(&ctx->lock);
        ctx->reboot_task_join_pending = false;
        portEXIT_CRITICAL(&ctx->lock);
    }
    if (task_timeout || reboot_timeout) {
        ESP_LOGE(TAG, "%s%s did not stop within 5 seconds",
                 task_timeout ? "peer task" : "",
                 task_timeout && reboot_timeout ? " and reboot task" :
                 (reboot_timeout ? "reboot task" : ""));
        portENTER_CRITICAL(&ctx->lock);
        ctx->stopping = false;
        portEXIT_CRITICAL(&ctx->lock);
        return ESP_ERR_TIMEOUT;
    }
    const int64_t quiescence_deadline_us =
        esp_timer_get_time() + 5LL * 1000LL * 1000LL;
    bool quiescence_timeout = false;
    uint32_t remaining_submitters = 0U;
    bool remaining_callback_busy = false;
    while (true) {
        portENTER_CRITICAL(&ctx->lock);
        remaining_submitters = ctx->submitters;
        portEXIT_CRITICAL(&ctx->lock);
        if (!remaining_submitters) {
            break;
        }
        if (esp_timer_get_time() >= quiescence_deadline_us) {
            quiescence_timeout = true;
            break;
        }
        vTaskDelay(1);
    }
    while (!quiescence_timeout) {
        portENTER_CRITICAL(&ctx->lock);
        remaining_callback_busy = ctx->cloud_callback_gate.active ||
                                  ctx->cloud_close_pending;
        portEXIT_CRITICAL(&ctx->lock);
        if (!remaining_callback_busy) {
            break;
        }
        if (esp_timer_get_time() >= quiescence_deadline_us) {
            quiescence_timeout = true;
            break;
        }
        vTaskDelay(1);
    }
    if (quiescence_timeout) {
        ESP_LOGE(TAG,
                 "transport submitter/cloud callback did not quiesce within "
                 "5 seconds submitters=%" PRIu32 " callback_busy=%d",
                 remaining_submitters, remaining_callback_busy);
        portENTER_CRITICAL(&ctx->lock);
        ctx->stopping = false;
        portEXIT_CRITICAL(&ctx->lock);
        return ESP_ERR_TIMEOUT;
    }
    flush_outbound(ctx);
    flush_inbound(ctx);
    if (ctx->inbound_queue) {
        vQueueDelete(ctx->inbound_queue);
        ctx->inbound_queue = NULL;
    }
    if (ctx->task_done) {
        vSemaphoreDelete(ctx->task_done);
        ctx->task_done = NULL;
    }
    if (ctx->reboot_task_done) {
        vSemaphoreDelete(ctx->reboot_task_done);
        ctx->reboot_task_done = NULL;
    }
    heap_caps_free(ctx->rpc_scratch);
    ctx->rpc_scratch = NULL;
    for (size_t i = 0; i < CONFIG_WEBRTC_TRANSPORT_AU_SLOTS; ++i) {
        heap_caps_free(ctx->slots[i].own);
        memset(&ctx->slots[i], 0, sizeof(ctx->slots[i]));
    }
    portENTER_CRITICAL(&ctx->lock);
    ctx->stopping = false;
    ctx->initializing = false;
    ctx->stop_requested = false;
    ctx->reboot_task_stop_requested = false;
    ctx->mode_valid = false;
    ctx->session_role = WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING;
    ctx->stats.mode_valid = false;
    ctx->stats.peer_state = WEBRTC_TRANSPORT_PEER_STOPPED;
    portEXIT_CRITICAL(&ctx->lock);
    return ESP_OK;
}

esp_err_t webrtc_transport_stop(void)
{
    return webrtc_transport_stop_internal(false);
}

/* Starts a new video epoch on the live peer. Queued access units and sync
 * IDRs belong to the old epoch; the next mode asks for its own sync IDR.
 * Caller holds ctx->lock. */
static void next_video_epoch_locked(transport_ctx_t *ctx)
{
    ctx->video_epoch++;
    ctx->stats.video_epoch = ctx->video_epoch;
    flush_ready_slots_locked(ctx);
    ctx->sync_idr_pending_generation = 0;
    ctx->stats.sync_idr_pending_generation = 0;
    uint32_t active_sync_idr = ctx->sync_idr_inflight_generation ?
        ctx->sync_idr_inflight_generation :
        ctx->sync_idr_committed_generation;
    (void)fail_sync_idr_locked(ctx, active_sync_idr, false);
    ctx->waiting_for_sync = true;
    ctx->stats.waiting_for_sync_frame = true;
    queue_video_input_event_locked(ctx);
}

/* Feedback about the old mode's bitrate does not apply to a new one. A
 * service video stream that restarts at the same size (a new encoder for a
 * new target or frame rate) still crosses the same network. */
static void reset_bitrate_feedback_locked(transport_ctx_t *ctx, uint16_t width,
                                          uint16_t height, bool new_stream)
{
    if (!new_stream || !ctx->mode_valid || ctx->width != width ||
        ctx->height != height) {
        memset(&ctx->bitrate_controller, 0, sizeof(ctx->bitrate_controller));
    }
}

/* Publishes a checked mode. A new stream starts a new video epoch even in
 * an unchanged mode: what is queued cannot be decoded after it. */
static esp_err_t apply_mode(uint16_t width, uint16_t height, uint16_t fps,
                            bool new_stream)
{
    transport_ctx_t *ctx = &s_ctx;
    bool changed = false;
    bool mode_identity_changed = false;
    portENTER_CRITICAL(&ctx->lock);
    if (!ctx->started) {
        portEXIT_CRITICAL(&ctx->lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* Only service video starts a new stream with each call. */
    ctx->guest_mode = new_stream;
    mode_identity_changed = ctx->stats.stage_age.epoch == 0 ||
                            ctx->width != width || ctx->height != height ||
                            ctx->fps != fps;
    changed = new_stream || !ctx->mode_valid || mode_identity_changed;
    if (changed) {
        if (mode_identity_changed) {
            reset_stage_age_locked(ctx);
        }
        reset_bitrate_feedback_locked(ctx, width, height, new_stream);
        ctx->width = width;
        ctx->height = height;
        ctx->fps = fps;
        ctx->mode_valid = true;
        ctx->stats.mode_valid = true;
        ctx->stats.width = width;
        ctx->stats.height = height;
        ctx->stats.fps = fps;
        next_video_epoch_locked(ctx);
        if (ctx->peer_connected && ctx->peer &&
            ctx->active_peer_generation != 0) {
            ctx->sync_idr_pending_generation = ctx->active_peer_generation;
            ctx->stats.sync_idr_pending_generation =
                ctx->active_peer_generation;
            ctx->stats.sync_idr_requests++;
        }
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (changed) {
        ESP_LOGI(TAG, "mode changed to %ux%u@%u (video epoch %" PRIu32 ")",
                 width, height, fps, ctx->video_epoch);
        notify_peer_task(ctx);
    }
    return ESP_OK;
}

esp_err_t webrtc_transport_set_mode(uint16_t width, uint16_t height, uint16_t fps)
{
    return supported_mode(width, height, fps) ?
               apply_mode(width, height, fps, false) : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t webrtc_transport_set_guest_mode(uint16_t width, uint16_t height,
                                          uint16_t fps)
{
    const bool supported = width > 0 && width <= 3840 && height > 0 &&
                           height <= 2160 && fps > 0 && fps <= 60;
    return supported ? apply_mode(width, height, fps, true) :
                       ESP_ERR_NOT_SUPPORTED;
}

void webrtc_transport_note_video_gap(void)
{
    transport_ctx_t *ctx = &s_ctx;
    portENTER_CRITICAL(&ctx->lock);
    if (ctx->started && ctx->peer_connected && ctx->peer &&
        ctx->active_peer_generation != 0) {
        /* The lost frame's order: the P frame after a sync IDR that is
         * already queued then asks for a new one instead of being sent. */
        ctx->next_au_order++;
        request_video_recovery_locked(ctx);
    }
    portEXIT_CRITICAL(&ctx->lock);
}

void webrtc_transport_set_video_switching(bool switching)
{
    portENTER_CRITICAL(&s_ctx.lock);
    s_ctx.video_switching = switching;
    portEXIT_CRITICAL(&s_ctx.lock);
}

void webrtc_transport_clear_mode(void)
{
    transport_ctx_t *ctx = &s_ctx;
    bool changed = false;
    portENTER_CRITICAL(&ctx->lock);
    if (ctx->started && ctx->mode_valid) {
        ctx->mode_valid = false;
        ctx->stats.mode_valid = false;
        next_video_epoch_locked(ctx);
        memset(&ctx->bitrate_controller, 0, sizeof(ctx->bitrate_controller));
        changed = true;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (changed) {
        ESP_LOGI(TAG, "mode cleared (video epoch %" PRIu32 ")",
                 ctx->video_epoch);
        notify_peer_task(ctx);
    }
}

/* No access unit is being filled by the encoder or packetized by the peer. */
static bool video_quiet_locked(const transport_ctx_t *ctx)
{
    if (ctx->submitters != 0) {
        return false;
    }
    for (size_t i = 0; i < CONFIG_WEBRTC_TRANSPORT_AU_SLOTS; ++i) {
        if (ctx->slots[i].state == AU_SLOT_SENDING) {
            return false;
        }
    }
    return true;
}

/* close_peer() clears ctx->peer before the vendor close and the reservation
 * release; closing_peer covers the rest. */
static bool peer_released_locked(const transport_ctx_t *ctx)
{
    return ctx->outbound_count == 0 && !ctx->rebuild_requested &&
           !ctx->closing_peer && !ctx->peer &&
           ctx->active_peer_generation == 0;
}

/* The caller has counted itself in mode_teardown_waiters, which keeps a new
 * peer from opening until this returns. */
static esp_err_t wait_for_teardown(transport_ctx_t *ctx,
                                   bool (*done)(const transport_ctx_t *),
                                   uint32_t timeout_ms, const char *what)
{
    int64_t started_us = esp_timer_get_time();
    int64_t deadline_us = started_us + (int64_t)timeout_ms * 1000;
    esp_err_t result;
    for (;;) {
        portENTER_CRITICAL(&ctx->lock);
        bool complete = !ctx->started || done(ctx);
        portEXIT_CRITICAL(&ctx->lock);
        if (complete) {
            ESP_LOGI(TAG, "%s complete in %" PRId64 " ms", what,
                     (esp_timer_get_time() - started_us) / 1000);
            result = ESP_OK;
            break;
        }
        if (timeout_ms == 0 || esp_timer_get_time() >= deadline_us) {
            ESP_LOGE(TAG, "%s timed out after %" PRIu32 " ms", what,
                     timeout_ms);
            result = ESP_ERR_TIMEOUT;
            break;
        }
        /* The project runs at 100 Hz; ten milliseconds is one real tick. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    portENTER_CRITICAL(&ctx->lock);
    ctx->mode_teardown_waiters--;
    portEXIT_CRITICAL(&ctx->lock);
    notify_peer_task(ctx);
    return result;
}

void webrtc_transport_hold_peer_open(bool hold)
{
    transport_ctx_t *ctx = &s_ctx;
    portENTER_CRITICAL(&ctx->lock);
    if (hold) {
        ctx->mode_teardown_waiters++;
    } else if (ctx->mode_teardown_waiters > 0) {
        ctx->mode_teardown_waiters--;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (!hold) {
        notify_peer_task(ctx);
    }
}

esp_err_t webrtc_transport_clear_mode_and_wait(uint32_t timeout_ms)
{
    transport_ctx_t *ctx = &s_ctx;
    portENTER_CRITICAL(&ctx->lock);
    ctx->mode_teardown_waiters++;
    portEXIT_CRITICAL(&ctx->lock);
    webrtc_transport_clear_mode();
    return wait_for_teardown(ctx, video_quiet_locked, timeout_ms,
                             "mode teardown");
}

esp_err_t webrtc_transport_set_guest_buffers(uint8_t *const *buffers,
                                             size_t bytes, uint32_t timeout_ms)
{
    transport_ctx_t *ctx = &s_ctx;
    esp_err_t ret = webrtc_transport_clear_mode_and_wait(timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }
    /* No mode: nothing leases a slot until the caller sets one. Each slot
     * is FREE; write back what the CPU wrote to the buffer it leaves, so a
     * lent one reaches capture DMA clean and with no stale line. */
    for (size_t i = 0; i < CONFIG_WEBRTC_TRANSPORT_AU_SLOTS; ++i) {
        au_slot_t *slot = &ctx->slots[i];
        if (slot->cpu_dirty_bytes) {
            ret = esp_cache_msync(slot->data,
                                  (slot->cpu_dirty_bytes + 127U) & ~(size_t)127U,
                                  ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                      ESP_CACHE_MSYNC_FLAG_INVALIDATE);
            if (ret != ESP_OK) {
                return ret;
            }
            slot->cpu_dirty_bytes = 0;
        }
    }
    portENTER_CRITICAL(&ctx->lock);
    for (size_t i = 0; i < CONFIG_WEBRTC_TRANSPORT_AU_SLOTS; ++i) {
        au_slot_t *slot = &ctx->slots[i];
        slot->data = buffers ? buffers[i] : slot->own;
        slot->capacity = buffers ? bytes & ~(size_t)127U :
                                   CONFIG_WEBRTC_TRANSPORT_MAX_AU_BYTES &
                                       ~(size_t)127U;
    }
    portEXIT_CRITICAL(&ctx->lock);
    return ESP_OK;
}

esp_err_t webrtc_transport_release_peer_and_wait(uint32_t timeout_ms)
{
    transport_ctx_t *ctx = &s_ctx;
    bool release = false;
    portENTER_CRITICAL(&ctx->lock);
    if (ctx->started && (ctx->peer || ctx->active_peer_generation != 0)) {
        if (ctx->peer && !ctx->closing_peer &&
            ctx->rpc_data_channel_open &&
            ctx->active_peer_session != 0 &&
            ctx->active_peer_generation != 0) {
            if (ctx->rpc_terminal_video_event_pending) {
                ctx->stats.rpc_event_notifications_coalesced++;
            } else {
                ctx->stats.rpc_event_notifications_queued++;
            }
            ctx->rpc_terminal_video_event_pending = true;
            ctx->rpc_terminal_video_session_id = ctx->active_peer_session;
            ctx->rpc_terminal_video_generation =
                ctx->active_peer_generation;
            ctx->rpc_terminal_video_stream_id =
                ctx->rpc_data_channel_stream_id;
        }
        uint32_t invalidated_generation = ctx->generation;
        next_generation_locked(ctx);
        schedule_cloud_invalidation_locked(
            ctx, invalidated_generation, ctx->generation);
        ctx->rebuild_requested = true;
        ctx->stats.video_peer_releases++;
        ctx->mode_teardown_waiters++;
        release = true;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (!release) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGW(TAG, "next video mode needs the peer's memory; releasing peer "
             "(generation %" PRIu32 ")", ctx->generation);
    notify_peer_task(ctx);
    return wait_for_teardown(ctx, peer_released_locked, timeout_ms,
                             "peer release");
}

esp_err_t webrtc_transport_set_video_paused(bool paused)
{
    return webrtc_transport_set_video_paused_by(WEBRTC_VIDEO_PAUSE_UI, paused);
}

esp_err_t webrtc_transport_set_video_paused_by(
    webrtc_video_pause_source_t source, bool hold)
{
    transport_ctx_t *ctx = &s_ctx;
    bool changed = false;
    portENTER_CRITICAL(&ctx->lock);
    if (!ctx->started) {
        portEXIT_CRITICAL(&ctx->lock);
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t held_before = ctx->video_pause_sources;
    if (hold) {
        ctx->video_pause_sources |= (uint8_t)source;
    } else {
        ctx->video_pause_sources &= (uint8_t)~source;
    }
    /* videoInputState tells the page that an upload holds video off. */
    const bool upload_hold_changed =
        ((held_before ^ ctx->video_pause_sources) &
         WEBRTC_VIDEO_PAUSE_STORAGE_UPLOAD) != 0;
    if (upload_hold_changed) {
        queue_video_input_event_locked(ctx);
    }
    bool paused = ctx->video_pause_sources != 0;
    if (ctx->video_paused != paused) {
        ctx->video_paused = paused;
        changed = true;
        if (paused) {
            ctx->sync_idr_pending_generation = 0;
            ctx->stats.sync_idr_pending_generation = 0;
            uint32_t active_sync_idr = ctx->sync_idr_inflight_generation ?
                ctx->sync_idr_inflight_generation :
                ctx->sync_idr_committed_generation;
            (void)fail_sync_idr_locked(ctx, active_sync_idr, false);
            flush_ready_slots_locked(ctx);
        } else if (ctx->peer_connected && ctx->peer &&
                   ctx->active_peer_generation != 0) {
            ctx->waiting_for_sync = true;
            ctx->stats.waiting_for_sync_frame = true;
            ctx->sync_idr_pending_generation = ctx->active_peer_generation;
            ctx->stats.sync_idr_pending_generation =
                ctx->active_peer_generation;
            ctx->stats.sync_idr_requests++;
        }
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (changed) {
        ESP_LOGI(TAG, "video stream %s", paused ? "paused" : "resumed");
    }
    if (changed || upload_hold_changed) {
        notify_peer_task(ctx);
    }
    return ESP_OK;
}

bool webrtc_transport_take_sync_idr_request(uint32_t *peer_generation)
{
    if (!peer_generation) {
        return false;
    }
    *peer_generation = 0;
    transport_ctx_t *ctx = &s_ctx;
    bool taken = false;
    portENTER_CRITICAL(&ctx->lock);
    uint32_t generation = ctx->sync_idr_pending_generation;
    bool sync_au_ready = false;
    for (size_t i = 0; i < CONFIG_WEBRTC_TRANSPORT_AU_SLOTS; ++i) {
        if ((ctx->slots[i].state == AU_SLOT_READY ||
             ctx->slots[i].state == AU_SLOT_SENDING) &&
            slot_is_decodable_sync(&ctx->slots[i], generation,
                                   ctx->video_epoch)) {
            sync_au_ready = true;
            break;
        }
    }
    bool valid = generation != 0 && ctx->started && !ctx->video_paused &&
                 ctx->peer_connected &&
                 ctx->waiting_for_sync && ctx->peer &&
                 generation == ctx->active_peer_generation &&
                 ctx->sync_idr_inflight_generation == 0 &&
                 ctx->sync_idr_committed_generation == 0 && !sync_au_ready;
    if (valid) {
        ctx->sync_idr_pending_generation = 0;
        ctx->stats.sync_idr_pending_generation = 0;
        ctx->sync_idr_inflight_generation = generation;
        ctx->stats.sync_idr_inflight_generation = generation;
        ctx->stats.sync_idr_issued++;
        *peer_generation = generation;
        taken = true;
    } else if (generation != 0 &&
               (!ctx->started || !ctx->peer_connected ||
                !ctx->waiting_for_sync || !ctx->peer ||
                generation != ctx->active_peer_generation)) {
        /* Teardown or connection loss can invalidate the request before the
         * encoder owner observes it.  Such a token was requested but never
         * issued, so it is simply retired. */
        ctx->sync_idr_pending_generation = 0;
        ctx->stats.sync_idr_pending_generation = 0;
    }
    portEXIT_CRITICAL(&ctx->lock);
    return taken;
}

void webrtc_transport_complete_sync_idr_production(
    uint32_t peer_generation, bool produced_and_accepted)
{
    if (peer_generation == 0) {
        return;
    }
    transport_ctx_t *ctx = &s_ctx;
    portENTER_CRITICAL(&ctx->lock);
    if (!produced_and_accepted) {
        fail_sync_idr_locked(ctx, peer_generation, true);
    } else if (ctx->sync_idr_inflight_generation == peer_generation) {
        /* A producer reported acceptance, but send_annexb did not commit a
         * decodable IDR+SPS+PPS slot for this generation.  Treat that as a
         * failed attempt instead of claiming transport success. */
        fail_sync_idr_locked(ctx, peer_generation, true);
    }
    portEXIT_CRITICAL(&ctx->lock);
}

static esp_err_t transport_get_stats(webrtc_transport_stats_t *stats,
                                     bool detailed)
{
    if (!stats) {
        return ESP_ERR_INVALID_ARG;
    }
    transport_ctx_t *ctx = &s_ctx;
    portENTER_CRITICAL(&ctx->lock);
    *stats = ctx->stats;
    stats->video_pause_sources = ctx->video_pause_sources;
    stats->video_switching = ctx->video_switching;
    stats->signaling_resident_bytes =
        (uint32_t)ctx->signaling_budget.used;
    stats->signaling_resident_high_watermark =
        (uint32_t)ctx->signaling_budget.high_watermark;
    portEXIT_CRITICAL(&ctx->lock);
    webrtc_network_timing_snapshot(&stats->network_timing, &stats->stage_age);
    /* Live polling must not walk TLSF pools with interrupts masked. Exact
     * diagnostics opt in; largest-block/count details carry their sample age. */
    jetkvm_memory_policy_snapshot_t memory = {0};
    if (detailed) {
        jetkvm_memory_policy_get_snapshot(&memory);
    } else {
        jetkvm_memory_policy_get_snapshot_cached(&memory);
    }
    stats->memory_details_cached = !detailed;
    stats->memory_details_sample_timestamp_us = memory.detailed_sample_timestamp_us;
    stats->internal_heap_free = memory.internal_free;
    stats->internal_heap_minimum = memory.internal_minimum;
    stats->internal_heap_largest = memory.internal_largest;
    stats->internal_dma_heap_free = memory.dma_free;
    stats->internal_dma_heap_minimum = memory.dma_minimum;
    stats->internal_dma_heap_largest = memory.dma_largest;
    stats->psram_free = memory.psram_free;
    stats->psram_minimum = memory.psram_minimum;
    stats->psram_largest = memory.psram_largest;
    webrtc_peer_memory_arena_snapshot_t peer_memory = {0};
    if (detailed) {
        webrtc_peer_memory_arena_get_snapshot(&peer_memory);
    } else {
        webrtc_peer_memory_arena_get_snapshot_cached(&peer_memory);
    }
    stats->peer_memory_details_sample_timestamp_us =
        peer_memory.detailed_sample_timestamp_us;
    stats->peer_memory_arena_active = peer_memory.active;
    stats->peer_memory_arena_capacity = peer_memory.capacity_bytes;
    stats->peer_memory_arena_free = peer_memory.free_bytes;
    stats->peer_memory_arena_minimum = peer_memory.minimum_free_bytes;
    stats->peer_memory_arena_largest = peer_memory.largest_free_block;
    stats->peer_memory_arena_allocated_blocks = peer_memory.allocated_blocks;
    stats->peer_memory_arena_allocation_failures =
        peer_memory.allocation_failures;
    stats->peer_memory_arena_begin_failures = peer_memory.begin_failures;
    stats->peer_memory_arena_leak_events = peer_memory.leak_events;
    return ESP_OK;
}

esp_err_t webrtc_transport_get_stats(webrtc_transport_stats_t *stats)
{
    return transport_get_stats(stats, false);
}

esp_err_t webrtc_transport_get_stats_detailed(webrtc_transport_stats_t *stats)
{
    return transport_get_stats(stats, true);
}

void webrtc_transport_reset_video_intervals(void)
{
    portENTER_CRITICAL(&s_ctx.lock);
    s_ctx.stats.video_encode_interval = (webrtc_transport_duration_hist_t){ 0 };
    s_ctx.stats.video_send_interval = (webrtc_transport_duration_hist_t){ 0 };
    /* The next frame sent starts the window, so a gap before it is not counted. */
    s_ctx.last_sent_encode_done_us = 0;
    s_ctx.last_sent_start_us = 0;
    portEXIT_CRITICAL(&s_ctx.lock);
}

const char *webrtc_transport_peer_state_name(int state)
{
    switch ((webrtc_transport_peer_state_t)state) {
    case WEBRTC_TRANSPORT_PEER_STOPPED: return "stopped";
    case WEBRTC_TRANSPORT_PEER_WAITING: return "waiting";
    case WEBRTC_TRANSPORT_PEER_GATHERING: return "gathering";
    case WEBRTC_TRANSPORT_PEER_PAIRING: return "pairing";
    case WEBRTC_TRANSPORT_PEER_CONNECTING: return "connecting";
    case WEBRTC_TRANSPORT_PEER_CONNECTED: return "connected";
    case WEBRTC_TRANSPORT_PEER_FAILED: return "failed";
    default: return "unknown";
    }
}

bool app_h264_frame_sink_submit(const uint8_t *data, size_t length,
                                const app_h264_frame_meta_t *meta)
{
    if (!data || !meta || !supported_mode(meta->width, meta->height, meta->fps)) {
        portENTER_CRITICAL(&s_ctx.lock);
        s_ctx.stats.access_units_offered++;
        s_ctx.stats.access_units_mode_mismatch++;
        portEXIT_CRITICAL(&s_ctx.lock);
        return false;
    }
    if (webrtc_transport_set_mode(meta->width, meta->height, meta->fps) != ESP_OK) {
        portENTER_CRITICAL(&s_ctx.lock);
        s_ctx.stats.access_units_offered++;
        s_ctx.stats.access_units_mode_mismatch++;
        portEXIT_CRITICAL(&s_ctx.lock);
        return false;
    }
    return webrtc_transport_send_annexb_traced(data, length,
                                               meta->rtp_timestamp_90khz,
                                               meta->keyframe, meta);
}

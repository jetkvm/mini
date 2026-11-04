#include "jetkvm_board_config.h"
#include "jetkvm_factory_reset.h"
#include "jetkvm_access.h"
/* WebRTC DataChannel, JSON-RPC, and HIDRPC control plane. */
#include "webrtc_data_channels_internal.h"
#include "rpc_worker.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_peer.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jetkvm_cloud_registration.h"
#include "jetkvm_control.h"
#include "jetkvm_device_state.h"
#include "jetkvm_hid_input.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_ota.h"
#include "jetkvm_settings.h"
#include "jetkvm_network_settings.h"
#include "jetkvm_mac_address.h"
#include "jetkvm_c5_wifi.h"
#include "jetkvm_video_quality.h"
#include "webrtc_rpc_adapters_internal.h"
#include "webrtc_sessions_internal.h"
#include "guest_channels.h"
#include "peer_sctp_streams.h"

/* videoInputState with the OS Services fields is the longest event. The
 * peer task's stack (16 KiB) already holds buffers of this size. */
#define JETKVM_RPC_EVENT_OUTPUT_MAX 1280
#define JETKVM_RPC_EVENT_HID_POLL_US (25LL * 1000LL)
#define JETKVM_RPC_EVENT_RETRY_US (100LL * 1000LL)
#define JETKVM_RPC_EVENT_MAX_RETRIES 10U
#define JETKVM_HIDRPC_STATE_RETRY_US (100LL * 1000LL)
#define JETKVM_HIDRPC_STATE_MAX_RETRIES 10U
/* A file drop's channel closes half a second after its last outcome, which
 * no notification announces: the peer task asks this often while idle. */
#define GUEST_OUTPUT_POLL_US (100LL * 1000LL)
/* After a send phase of T, guest output waits GUEST_OUTPUT_PACE * T: a data
 * channel byte costs about 1 us of CPU 1 in DTLS and SCTP, and unpaced
 * terminal output took all of CPU 1, halving video and holding HID input. */
#define GUEST_OUTPUT_PACE 3

static const char *TAG = "webrtc_data_channels";

static uint32_t rpc_event_valid_mask(void)
{
    return (1UL << (unsigned)WEBRTC_RPC_EVENT_COUNT) - 1UL;
}
void clear_rpc_events_locked(transport_ctx_t *ctx)
{
    ctx->rpc_event_pending_mask = 0;
    memset(ctx->rpc_event_retry_count, 0,
           sizeof(ctx->rpc_event_retry_count));
    memset(ctx->rpc_event_retry_not_before_us, 0,
           sizeof(ctx->rpc_event_retry_not_before_us));
    ctx->rpc_event_cursor = 0;
    ctx->rpc_event_next_hid_poll_us = 0;
    ctx->rpc_network_next_poll_us = 0;
    ctx->rpc_event_hid_valid = false;
    memset(&ctx->rpc_event_last_hid, 0, sizeof(ctx->rpc_event_last_hid));
    ctx->stats.rpc_event_pending_mask = 0;
}

void clear_hidrpc_state_notifications_locked(transport_ctx_t *ctx)
{
    ctx->hidrpc_handshake_complete = false;
    ctx->hidrpc_state_pending_mask = 0;
    memset(ctx->hidrpc_state_retry_count, 0,
           sizeof(ctx->hidrpc_state_retry_count));
    memset(ctx->hidrpc_state_retry_not_before_us, 0,
           sizeof(ctx->hidrpc_state_retry_not_before_us));
    ctx->hidrpc_state_cursor = 0;
    ctx->hidrpc_macro_observed_valid = false;
    ctx->hidrpc_macro_observed_revision = 0;
    ctx->hidrpc_macro_active = false;
    ctx->hidrpc_macro_is_paste = false;
    ctx->stats.hidrpc_handshake_complete = false;
    ctx->stats.hidrpc_state_pending_mask = 0;
}

/* The peer belongs to the current session, which may still use it. */
static bool peer_session_current_locked(const transport_ctx_t *ctx)
{
    return ctx->started && !ctx->stop_requested && !ctx->closing_peer &&
           ctx->session_active && ctx->peer &&
           ctx->active_peer_session == ctx->session_id &&
           ctx->active_peer_generation == ctx->generation &&
           ctx->active_peer_role == ctx->session_role &&
           (ctx->session_role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED ||
            jetkvm_access_epoch_valid(ctx->local_access_epoch));
}

static bool rpc_event_session_current_locked(const transport_ctx_t *ctx)
{
    return ctx->rpc_data_channel_open && peer_session_current_locked(ctx);
}

bool reboot_rpc_peer_current_locked(
    const transport_ctx_t *ctx, uint32_t session_id, uint32_t generation,
    uint16_t stream_id)
{
    return ctx->reboot_available && ctx->reboot_task &&
           !ctx->reboot_task_stop_requested && ctx->started &&
           !ctx->stop_requested && !ctx->closing_peer &&
           ctx->session_active && ctx->peer && ctx->rpc_data_channel_open &&
           ctx->session_id == session_id && ctx->generation == generation &&
           ctx->active_peer_session == session_id &&
           ctx->active_peer_generation == generation &&
           ctx->rpc_data_channel_stream_id == stream_id &&
           ctx->active_peer_role == ctx->session_role &&
           (ctx->session_role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED ||
            jetkvm_access_epoch_valid(ctx->local_access_epoch));
}

static void queue_rpc_events_locked(transport_ctx_t *ctx, uint32_t mask)
{
    mask &= rpc_event_valid_mask();
    if (!mask || !rpc_event_session_current_locked(ctx)) {
        return;
    }
    for (unsigned i = 0; i < (unsigned)WEBRTC_RPC_EVENT_COUNT; ++i) {
        uint32_t bit = WEBRTC_RPC_EVENT_MASK(i);
        if (!(mask & bit)) {
            continue;
        }
        if (ctx->rpc_event_pending_mask & bit) {
            ctx->stats.rpc_event_notifications_coalesced++;
        } else {
            ctx->stats.rpc_event_notifications_queued++;
        }
        ctx->rpc_event_revision[i]++;
        if (ctx->rpc_event_revision[i] == 0) {
            ctx->rpc_event_revision[i]++;
        }
        ctx->rpc_event_retry_count[i] = 0;
        ctx->rpc_event_retry_not_before_us[i] = 0;
    }
    ctx->rpc_event_pending_mask |= mask;
    ctx->stats.rpc_event_pending_mask = ctx->rpc_event_pending_mask;
}

/* The peer outlives mode changes; tell its page when video stops or starts. */
void queue_video_input_event_locked(transport_ctx_t *ctx)
{
    queue_rpc_events_locked(ctx,
                            WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_VIDEO_INPUT));
}

void reset_data_channels_locked(transport_ctx_t *ctx)
{
    if (webrtc_reboot_slot_cancel(&ctx->reboot_slot)) {
        ctx->reboot_executing = false;
    }
    ctx->rpc_data_channel_open = false;
    ctx->hidrpc_data_channel_open = false;
    ctx->hidrpc_unreliable_ordered_data_channel_open = false;
    ctx->hidrpc_unreliable_unordered_data_channel_open = false;
    ctx->rpc_data_channel_stream_id = 0;
    ctx->hidrpc_data_channel_stream_id = 0;
    ctx->hidrpc_unreliable_ordered_data_channel_stream_id = 0;
    ctx->hidrpc_unreliable_unordered_data_channel_stream_id = 0;
    ctx->stats.rpc_data_channel_open = false;
    ctx->stats.hidrpc_data_channel_open = false;
    ctx->stats.hidrpc_unreliable_ordered_data_channel_open = false;
    ctx->stats.hidrpc_unreliable_unordered_data_channel_open = false;
    ctx->stats.rpc_data_channel_stream_id = 0;
    ctx->stats.hidrpc_data_channel_stream_id = 0;
    ctx->stats.hidrpc_unreliable_ordered_data_channel_stream_id = 0;
    ctx->stats.hidrpc_unreliable_unordered_data_channel_stream_id = 0;
    clear_rpc_events_locked(ctx);
    clear_hidrpc_state_notifications_locked(ctx);
    ctx->rpc_terminal_video_event_pending = false;
    ctx->rpc_terminal_video_session_id = 0;
    ctx->rpc_terminal_video_generation = 0;
    ctx->rpc_terminal_video_stream_id = 0;
}

static bool hid_generation_active_locked(const transport_ctx_t *ctx,
                                         uint32_t generation)
{
    return generation != 0U && ctx->started && !ctx->stop_requested &&
           !ctx->closing_peer && ctx->session_active && ctx->peer &&
           ctx->active_peer_session == ctx->session_id &&
           ctx->active_peer_generation == generation &&
           ctx->active_hid_generation == generation &&
           ctx->generation == generation &&
           ctx->active_peer_role == ctx->session_role &&
           (ctx->session_role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED ||
            jetkvm_access_epoch_valid(ctx->local_access_epoch));
}

static bool hidrpc_state_session_current_locked(const transport_ctx_t *ctx)
{
    return ctx->hidrpc_handshake_complete &&
           ctx->hidrpc_data_channel_open &&
           hid_generation_active_locked(ctx, ctx->active_peer_generation);
}

static void queue_hidrpc_state_locked(transport_ctx_t *ctx, uint32_t mask)
{
    mask &= WEBRTC_HIDRPC_STATE_ALL_MASK;
    if (!mask || !hidrpc_state_session_current_locked(ctx)) {
        return;
    }
    for (unsigned i = 0; i < (unsigned)WEBRTC_HIDRPC_STATE_COUNT; ++i) {
        uint32_t bit = WEBRTC_HIDRPC_STATE_MASK(i);
        if (!(mask & bit)) {
            continue;
        }
        if (ctx->hidrpc_state_pending_mask & bit) {
            ctx->stats.hidrpc_state_notifications_coalesced++;
        } else {
            ctx->stats.hidrpc_state_notifications_queued++;
        }
        ctx->hidrpc_state_revision[i]++;
        if (ctx->hidrpc_state_revision[i] == 0) {
            ctx->hidrpc_state_revision[i]++;
        }
        ctx->hidrpc_state_retry_count[i] = 0;
        ctx->hidrpc_state_retry_not_before_us[i] = 0;
    }
    ctx->hidrpc_state_pending_mask |= mask;
    ctx->stats.hidrpc_state_pending_mask = ctx->hidrpc_state_pending_mask;
}

typedef struct {
    transport_ctx_t *ctx;
    esp_peer_handle_t peer;
} peer_sender_t;

static data_channel_send_result_t send_to_peer(
    void *arg, uint16_t stream_id, bool text, const uint8_t *data,
    size_t size)
{
    const peer_sender_t *sender = arg;
    esp_peer_data_frame_t frame = {
        .type = text ? ESP_PEER_DATA_CHANNEL_STRING :
                       ESP_PEER_DATA_CHANNEL_DATA,
        .stream_id = stream_id,
        .data = (uint8_t *)data,
        .size = (int)size,
    };
    int ret = esp_peer_send_data(sender->peer, &frame);
    if (ret == ESP_PEER_ERR_WOULD_BLOCK) {
        return DATA_CHANNEL_BLOCKED;
    }
    if (ret != ESP_PEER_ERR_NONE) {
        return DATA_CHANNEL_FAILED;
    }
    portENTER_CRITICAL(&sender->ctx->lock);
    sender->ctx->stats.data_channel_messages_tx++;
    sender->ctx->stats.data_channel_bytes_tx += size;
    portEXIT_CRITICAL(&sender->ctx->lock);
    return DATA_CHANNEL_SENT;
}

/* After the send queue changed. A dispatched reboot learns here whether its
 * response and willReboot still wait. */
static void publish_send_queue_locked(transport_ctx_t *ctx)
{
    webrtc_reboot_slot_t *slot = &ctx->reboot_slot;
    ctx->stats.data_channel_send_queue_bytes = ctx->send_queue.bytes;
    webrtc_reboot_slot_messages_wait(
        slot, slot->dispatch_ready &&
                  data_channel_queue_holds(&ctx->send_queue,
                                           slot->stream_id,
                                           slot->last_queued),
        esp_timer_get_time());
}

static void queued_message_lost(void *arg, uint16_t stream_id,
                                uint32_t number)
{
    transport_ctx_t *ctx = ((const peer_sender_t *)arg)->ctx;
    portENTER_CRITICAL(&ctx->lock);
    const uint32_t last_queued = ctx->reboot_slot.last_queued;
    bool cancelled = webrtc_reboot_slot_message_lost(
        &ctx->reboot_slot, stream_id, number);
    if (cancelled) {
        ctx->stats.reboot_send_failures++;
        ctx->reboot_executing = false;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (cancelled) {
        /* The browser must not get willReboot for a reboot that does not
         * come. */
        data_channel_queue_drop_through(&ctx->send_queue, stream_id,
                                        last_queued);
        ESP_LOGW(TAG, "reboot cancelled: its response or willReboot was lost");
    }
}

/* Sent, or waiting in the send queue until the send buffer takes it. Only a
 * message that is lost counts as a send error. */
static bool data_channel_send_to_peer(
    transport_ctx_t *ctx, esp_peer_handle_t peer, uint16_t stream_id,
    esp_peer_data_channel_type_t type, const uint8_t *data, size_t size)
{
    if (!peer || !data || size == 0 || size > INT_MAX) {
        return false;
    }
    peer_sender_t sender = {.ctx = ctx, .peer = peer};
    data_channel_send_result_t result = data_channel_queue_send(
        &ctx->send_queue, send_to_peer, &sender, stream_id,
        type == ESP_PEER_DATA_CHANNEL_STRING, data, size);
    portENTER_CRITICAL(&ctx->lock);
    if (result == DATA_CHANNEL_QUEUED) {
        ctx->stats.data_channel_send_queued++;
    } else if (result == DATA_CHANNEL_FAILED) {
        ctx->stats.data_channel_send_errors++;
    }
    publish_send_queue_locked(ctx);
    portEXIT_CRITICAL(&ctx->lock);
    return result != DATA_CHANNEL_FAILED;
}

void send_queued_data_channel_messages(transport_ctx_t *ctx)
{
    if (ctx->send_queue.head == NULL) {
        return;
    }
    peer_sender_t sender = {.ctx = ctx};
    portENTER_CRITICAL(&ctx->lock);
    if (!ctx->closing_peer) {
        sender.peer = ctx->peer;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (sender.peer == NULL) {
        return;
    }
    size_t lost = data_channel_queue_drain(&ctx->send_queue, send_to_peer,
                                           queued_message_lost, &sender);
    portENTER_CRITICAL(&ctx->lock);
    ctx->stats.data_channel_send_errors += lost;
    publish_send_queue_locked(ctx);
    portEXIT_CRITICAL(&ctx->lock);
}

void clear_data_channel_queue(transport_ctx_t *ctx)
{
    data_channel_queue_clear(&ctx->send_queue);
    portENTER_CRITICAL(&ctx->lock);
    publish_send_queue_locked(ctx);
    portEXIT_CRITICAL(&ctx->lock);
}

static bool data_channel_send(transport_ctx_t *ctx, uint16_t stream_id,
                              esp_peer_data_channel_type_t type,
                              const uint8_t *data, size_t size)
{
    esp_peer_handle_t peer = NULL;
    portENTER_CRITICAL(&ctx->lock);
    if (!ctx->closing_peer && ctx->peer) {
        peer = ctx->peer;
    }
    portEXIT_CRITICAL(&ctx->lock);
    return data_channel_send_to_peer(
        ctx, peer, stream_id, type, data, size);
}

static void close_data_channel(transport_ctx_t *ctx, const char *label)
{
    esp_peer_handle_t peer = NULL;
    portENTER_CRITICAL(&ctx->lock);
    if (!ctx->closing_peer && ctx->peer != NULL) {
        peer = ctx->peer;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (peer != NULL && label != NULL) {
        (void)esp_peer_close_data_channel(peer, label);
    }
}

/* Used only from the peer task/callback.  Checking the immutable active-peer
 * identity before taking the handle prevents an accepted request from an old
 * generation ever being sent through a replacement peer. */
static bool data_channel_send_exact_rpc_peer(
    transport_ctx_t *ctx, uint32_t session_id, uint32_t generation,
    uint16_t stream_id, const uint8_t *data, size_t size)
{
    esp_peer_handle_t peer = NULL;
    portENTER_CRITICAL(&ctx->lock);
    if (reboot_rpc_peer_current_locked(
            ctx, session_id, generation, stream_id)) {
        peer = ctx->peer;
    }
    portEXIT_CRITICAL(&ctx->lock);
    return data_channel_send_to_peer(
        ctx, peer, stream_id, ESP_PEER_DATA_CHANNEL_STRING, data, size);
}

static webrtc_rpc_usb_state_t rpc_usb_state_from_hid(
    const jetkvm_hid_input_snapshot_t *snapshot)
{
    return webrtc_rpc_usb_state_from_bus(
        snapshot->usb.initialized, snapshot->usb.connected,
        snapshot->usb.mounted, snapshot->usb.suspended);
}

void poll_rpc_hid_state(transport_ctx_t *ctx, int64_t now_us)
{
    uint32_t generation = 0;
    uint32_t session_id = 0;
    uint16_t stream_id = 0;
    portENTER_CRITICAL(&ctx->lock);
    bool dirty = atomic_exchange(&ctx->rpc_event_hid_dirty, false);
    if (rpc_event_session_current_locked(ctx) &&
        (dirty || now_us >= ctx->rpc_event_next_hid_poll_us)) {
        ctx->rpc_event_next_hid_poll_us =
            now_us + JETKVM_RPC_EVENT_HID_POLL_US;
        generation = ctx->active_peer_generation;
        session_id = ctx->active_peer_session;
        stream_id = ctx->rpc_data_channel_stream_id;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (generation == 0) {
        return;
    }

    jetkvm_hid_input_snapshot_t hid_snapshot = {0};
    esp_err_t err = jetkvm_hid_input_get_snapshot(&hid_snapshot);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&ctx->lock);
        ctx->stats.rpc_event_poll_errors++;
        portEXIT_CRITICAL(&ctx->lock);
        return;
    }

    webrtc_rpc_hid_state_t current = {
        .usb = rpc_usb_state_from_hid(&hid_snapshot),
        .keyboard_leds = hid_snapshot.keyboard_leds,
        .modifier = hid_snapshot.keyboard.modifier,
        .keyboard_revision = hid_snapshot.keyboard_revision,
    };
    memcpy(current.keys, hid_snapshot.keyboard.keycodes,
           sizeof(current.keys));

    portENTER_CRITICAL(&ctx->lock);
    bool current_session = rpc_event_session_current_locked(ctx) &&
                           ctx->active_peer_generation == generation &&
                           ctx->active_peer_session == session_id &&
                           ctx->rpc_data_channel_stream_id == stream_id;
    if (current_session) {
        uint32_t changed = webrtc_rpc_event_hid_diff(
            &ctx->rpc_event_last_hid, &current,
            ctx->rpc_event_hid_valid);
        ctx->rpc_event_last_hid = current;
        ctx->rpc_event_hid_valid = true;
        uint32_t json_changed = changed;
        if (ctx->hidrpc_handshake_complete) {
            uint32_t hidrpc_changed = 0;
            if (changed & WEBRTC_RPC_EVENT_MASK(
                              WEBRTC_RPC_EVENT_KEYBOARD_LEDS)) {
                hidrpc_changed |=
                    WEBRTC_HIDRPC_STATE_MASK(
                        WEBRTC_HIDRPC_STATE_KEYBOARD_LEDS);
            }
            if (changed & WEBRTC_RPC_EVENT_MASK(
                              WEBRTC_RPC_EVENT_KEYS_DOWN)) {
                hidrpc_changed |=
                    WEBRTC_HIDRPC_STATE_MASK(
                        WEBRTC_HIDRPC_STATE_KEYS_DOWN);
            }
            queue_hidrpc_state_locked(ctx, hidrpc_changed);
            json_changed &=
                WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_USB_STATE);

            if (!ctx->hidrpc_macro_observed_valid ||
                ctx->hidrpc_macro_observed_revision !=
                    hid_snapshot.macro_state_revision) {
                bool state_changed =
                    !ctx->hidrpc_macro_observed_valid ||
                    ctx->hidrpc_macro_active !=
                        hid_snapshot.macro_active ||
                    ctx->hidrpc_macro_is_paste !=
                        hid_snapshot.macro_is_paste;
                ctx->hidrpc_macro_observed_valid = true;
                ctx->hidrpc_macro_observed_revision =
                    hid_snapshot.macro_state_revision;
                ctx->hidrpc_macro_active = hid_snapshot.macro_active;
                ctx->hidrpc_macro_is_paste =
                    hid_snapshot.macro_is_paste;
                if (state_changed) {
                    queue_hidrpc_state_locked(
                        ctx,
                        WEBRTC_HIDRPC_STATE_MASK(
                            WEBRTC_HIDRPC_STATE_KEYBOARD_MACRO));
                }
            }
        }
        queue_rpc_events_locked(ctx, json_changed);
    }
    portEXIT_CRITICAL(&ctx->lock);
}

static void complete_rpc_event_render_failure(
    transport_ctx_t *ctx, webrtc_rpc_event_kind_t kind,
    uint32_t generation, uint32_t session_id, uint16_t stream_id,
    uint32_t revision)
{
    uint32_t bit = WEBRTC_RPC_EVENT_MASK(kind);
    portENTER_CRITICAL(&ctx->lock);
    ctx->stats.rpc_event_render_errors++;
    if (rpc_event_session_current_locked(ctx) &&
        ctx->active_peer_generation == generation &&
        ctx->active_peer_session == session_id &&
        ctx->rpc_data_channel_stream_id == stream_id &&
        ctx->rpc_event_revision[kind] == revision &&
        (ctx->rpc_event_pending_mask & bit)) {
        ctx->rpc_event_pending_mask &= ~bit;
        ctx->stats.rpc_event_pending_mask = ctx->rpc_event_pending_mask;
        ctx->stats.rpc_event_notifications_dropped++;
    }
    portEXIT_CRITICAL(&ctx->lock);
}

/* videoInputState ends with the OS Services fields when OS Services has
 * any. Fields that do not fit are left out, and the event still goes. */
static bool render_video_input_event(transport_ctx_t *ctx,
                                     const webrtc_rpc_event_snapshot_t *snapshot,
                                     char *output, size_t capacity, size_t *size)
{
    if (!webrtc_rpc_event_render(WEBRTC_RPC_EVENT_VIDEO_INPUT, snapshot,
                                 output, capacity, size)) {
        return false;
    }
    if (!webrtc_rpc_event_add_params(output, capacity, size,
                                     jetkvm_control_render_guest_video_fields)) {
        portENTER_CRITICAL(&ctx->lock);
        ctx->stats.rpc_event_render_errors++;
        portEXIT_CRITICAL(&ctx->lock);
    }
    return true;
}

static bool render_rpc_event(transport_ctx_t *ctx, webrtc_rpc_event_kind_t kind,
                             const webrtc_rpc_event_snapshot_t *snapshot,
                             char *output, size_t capacity, size_t *size)
{
    if (kind == WEBRTC_RPC_EVENT_VIDEO_INPUT) {
        return render_video_input_event(ctx, snapshot, output, capacity, size);
    }
    if (kind == WEBRTC_RPC_EVENT_LOCAL_VERSION) {
        char app_version[JETKVM_OTA_VERSION_CAPACITY];
        char system_version[JETKVM_OTA_VERSION_CAPACITY];
        jetkvm_ota_local_versions(app_version, sizeof(app_version),
                                  system_version, sizeof(system_version));
        return jetkvm_control_render_local_version_notification(
            app_version, system_version, JETKVM_DEVICE_SKU, output, capacity, size);
    }
    if (kind == WEBRTC_RPC_EVENT_DEVICE_CAPABILITIES) {
        /* The providers getDeviceCapabilities derives the board's features
         * from (see the RPC context below), so both report the same list. */
        static const jetkvm_control_context_t board = {
            .get_wifi_settings = jetkvm_c5_wifi_get_settings,
            .virtual_media = {.get_state = rpc_get_virtual_media_state_adapter},
        };
        return jetkvm_control_render_capabilities_notification(
            &board, output, capacity, size);
    }
    return webrtc_rpc_event_render(kind, snapshot, output, capacity, size);
}

void send_one_pending_rpc_event(transport_ctx_t *ctx, int64_t now_us)
{
    webrtc_rpc_event_kind_t kind = WEBRTC_RPC_EVENT_COUNT;
    webrtc_rpc_event_snapshot_t snapshot = {0};
    uint32_t revision = 0;
    uint32_t generation = 0;
    uint32_t session_id = 0;
    uint16_t stream_id = 0;

    portENTER_CRITICAL(&ctx->lock);
    if (rpc_event_session_current_locked(ctx)) {
        for (unsigned offset = 0;
             offset < (unsigned)WEBRTC_RPC_EVENT_COUNT; ++offset) {
            unsigned candidate =
                (ctx->rpc_event_cursor + offset) %
                (unsigned)WEBRTC_RPC_EVENT_COUNT;
            uint32_t bit = WEBRTC_RPC_EVENT_MASK(candidate);
            if (!(ctx->rpc_event_pending_mask & bit) ||
                now_us < ctx->rpc_event_retry_not_before_us[candidate]) {
                continue;
            }
            kind = (webrtc_rpc_event_kind_t)candidate;
            ctx->rpc_event_cursor = (uint8_t)(
                (candidate + 1U) % (unsigned)WEBRTC_RPC_EVENT_COUNT);
            revision = ctx->rpc_event_revision[candidate];
            generation = ctx->active_peer_generation;
            session_id = ctx->active_peer_session;
            stream_id = ctx->rpc_data_channel_stream_id;
            snapshot.video.ready = ctx->mode_valid;
            snapshot.video.switching = ctx->video_switching;
            snapshot.video.streaming =
                ctx->started && ctx->mode_valid ?
                    WEBRTC_RPC_VIDEO_ACTIVE : WEBRTC_RPC_VIDEO_INACTIVE;
            snapshot.video.upload_pauses =
                (ctx->video_pause_sources &
                 WEBRTC_VIDEO_PAUSE_STORAGE_UPLOAD) != 0;
            snapshot.video.width = ctx->mode_valid ? ctx->width : 0;
            snapshot.video.height = ctx->mode_valid ? ctx->height : 0;
            snapshot.video.fps = ctx->mode_valid ? ctx->fps : 0;
            snapshot.hid = ctx->rpc_event_last_hid;
            break;
        }
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (kind == WEBRTC_RPC_EVENT_COUNT) {
        return;
    }

    char output[JETKVM_RPC_EVENT_OUTPUT_MAX];
    size_t output_size = 0;
    if (!render_rpc_event(ctx, kind, &snapshot, output, sizeof(output),
                          &output_size)) {
        complete_rpc_event_render_failure(
            ctx, kind, generation, session_id, stream_id, revision);
        return;
    }

    portENTER_CRITICAL(&ctx->lock);
    bool still_current = rpc_event_session_current_locked(ctx) &&
                         ctx->active_peer_generation == generation &&
                         ctx->active_peer_session == session_id &&
                         ctx->rpc_data_channel_stream_id == stream_id &&
                         ctx->rpc_event_revision[kind] == revision &&
                         (ctx->rpc_event_pending_mask &
                          WEBRTC_RPC_EVENT_MASK(kind));
    portEXIT_CRITICAL(&ctx->lock);
    if (!still_current) {
        return;
    }

    bool sent = data_channel_send(
        ctx, stream_id, ESP_PEER_DATA_CHANNEL_STRING,
        (const uint8_t *)output, output_size);
    uint32_t bit = WEBRTC_RPC_EVENT_MASK(kind);
    portENTER_CRITICAL(&ctx->lock);
    if (sent) {
        ctx->stats.rpc_event_notifications_sent++;
    }
    still_current = rpc_event_session_current_locked(ctx) &&
                    ctx->active_peer_generation == generation &&
                    ctx->active_peer_session == session_id &&
                    ctx->rpc_data_channel_stream_id == stream_id &&
                    ctx->rpc_event_revision[kind] == revision &&
                    (ctx->rpc_event_pending_mask & bit);
    if (still_current && sent) {
        ctx->rpc_event_pending_mask &= ~bit;
        ctx->rpc_event_retry_count[kind] = 0;
        ctx->rpc_event_retry_not_before_us[kind] = 0;
        ctx->stats.rpc_event_pending_mask = ctx->rpc_event_pending_mask;
    } else if (still_current) {
        ctx->rpc_event_retry_count[kind]++;
        if (ctx->rpc_event_retry_count[kind] >=
            JETKVM_RPC_EVENT_MAX_RETRIES) {
            ctx->rpc_event_pending_mask &= ~bit;
            ctx->stats.rpc_event_pending_mask =
                ctx->rpc_event_pending_mask;
            ctx->stats.rpc_event_notifications_dropped++;
        } else {
            ctx->rpc_event_retry_not_before_us[kind] =
                now_us + JETKVM_RPC_EVENT_RETRY_US;
        }
    }
    portEXIT_CRITICAL(&ctx->lock);
}

void send_one_pending_hidrpc_state(transport_ctx_t *ctx,
                                          int64_t now_us)
{
    webrtc_hidrpc_state_kind_t kind = WEBRTC_HIDRPC_STATE_COUNT;
    webrtc_rpc_hid_state_t hid = {0};
    bool macro_active = false;
    bool macro_is_paste = false;
    uint32_t revision = 0;
    uint32_t generation = 0;
    uint32_t session_id = 0;
    uint16_t stream_id = 0;

    portENTER_CRITICAL(&ctx->lock);
    if (hidrpc_state_session_current_locked(ctx)) {
        for (unsigned offset = 0;
             offset < (unsigned)WEBRTC_HIDRPC_STATE_COUNT;
             ++offset) {
            unsigned candidate =
                (ctx->hidrpc_state_cursor + offset) %
                (unsigned)WEBRTC_HIDRPC_STATE_COUNT;
            uint32_t bit = WEBRTC_HIDRPC_STATE_MASK(candidate);
            if (!(ctx->hidrpc_state_pending_mask & bit) ||
                now_us <
                    ctx->hidrpc_state_retry_not_before_us[candidate]) {
                continue;
            }
            kind = (webrtc_hidrpc_state_kind_t)candidate;
            ctx->hidrpc_state_cursor = (uint8_t)(
                (candidate + 1U) %
                (unsigned)WEBRTC_HIDRPC_STATE_COUNT);
            revision = ctx->hidrpc_state_revision[candidate];
            generation = ctx->active_peer_generation;
            session_id = ctx->active_peer_session;
            stream_id = ctx->hidrpc_data_channel_stream_id;
            hid = ctx->rpc_event_last_hid;
            macro_active = ctx->hidrpc_macro_active;
            macro_is_paste = ctx->hidrpc_macro_is_paste;
            break;
        }
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (kind == WEBRTC_HIDRPC_STATE_COUNT) {
        return;
    }

    uint8_t output[8];
    size_t output_size = 0;
    if (!webrtc_hidrpc_state_render(
            kind, &hid, macro_active, macro_is_paste,
            output, sizeof(output), &output_size)) {
        return;
    }

    uint32_t bit = WEBRTC_HIDRPC_STATE_MASK(kind);
    portENTER_CRITICAL(&ctx->lock);
    bool still_current = hidrpc_state_session_current_locked(ctx) &&
                         ctx->active_peer_generation == generation &&
                         ctx->active_peer_session == session_id &&
                         ctx->hidrpc_data_channel_stream_id == stream_id &&
                         ctx->hidrpc_state_revision[kind] == revision &&
                         (ctx->hidrpc_state_pending_mask & bit);
    portEXIT_CRITICAL(&ctx->lock);
    if (!still_current) {
        return;
    }

    bool sent = data_channel_send(ctx, stream_id,
                                  ESP_PEER_DATA_CHANNEL_DATA,
                                  output, output_size);
    portENTER_CRITICAL(&ctx->lock);
    if (sent) {
        ctx->stats.hidrpc_state_notifications_sent++;
    }
    still_current = hidrpc_state_session_current_locked(ctx) &&
                    ctx->active_peer_generation == generation &&
                    ctx->active_peer_session == session_id &&
                    ctx->hidrpc_data_channel_stream_id == stream_id &&
                    ctx->hidrpc_state_revision[kind] == revision &&
                    (ctx->hidrpc_state_pending_mask & bit);
    if (still_current && sent) {
        ctx->hidrpc_state_pending_mask &= ~bit;
        ctx->hidrpc_state_retry_count[kind] = 0;
        ctx->hidrpc_state_retry_not_before_us[kind] = 0;
        ctx->stats.hidrpc_state_pending_mask =
            ctx->hidrpc_state_pending_mask;
    } else if (still_current) {
        ctx->hidrpc_state_retry_count[kind]++;
        if (ctx->hidrpc_state_retry_count[kind] >=
            JETKVM_HIDRPC_STATE_MAX_RETRIES) {
            ctx->hidrpc_state_pending_mask &= ~bit;
            ctx->stats.hidrpc_state_pending_mask =
                ctx->hidrpc_state_pending_mask;
            ctx->stats.hidrpc_state_notifications_dropped++;
        } else {
            ctx->hidrpc_state_retry_not_before_us[kind] =
                now_us + JETKVM_HIDRPC_STATE_RETRY_US;
        }
    }
    portEXIT_CRITICAL(&ctx->lock);
}

/* HDMI loss invalidates the public generation immediately, but the old peer
 * still exists until the peer task performs teardown.  Give that exact peer a
 * final no-signal snapshot before closing its DataChannel.  This special slot
 * is deliberately separate from current-generation events. */
void send_terminal_video_event(transport_ctx_t *ctx)
{
    uint32_t session_id = 0;
    uint32_t generation = 0;
    uint16_t stream_id = 0;
    portENTER_CRITICAL(&ctx->lock);
    bool valid = ctx->rpc_terminal_video_event_pending && ctx->peer &&
                 !ctx->closing_peer && ctx->rpc_data_channel_open &&
                 ctx->active_peer_session ==
                     ctx->rpc_terminal_video_session_id &&
                 ctx->active_peer_generation ==
                     ctx->rpc_terminal_video_generation &&
                 ctx->rpc_data_channel_stream_id ==
                     ctx->rpc_terminal_video_stream_id;
    if (valid) {
        session_id = ctx->rpc_terminal_video_session_id;
        generation = ctx->rpc_terminal_video_generation;
        stream_id = ctx->rpc_terminal_video_stream_id;
    }
    ctx->rpc_terminal_video_event_pending = false;
    ctx->rpc_terminal_video_session_id = 0;
    ctx->rpc_terminal_video_generation = 0;
    ctx->rpc_terminal_video_stream_id = 0;
    portEXIT_CRITICAL(&ctx->lock);
    if (!valid) {
        return;
    }

    webrtc_rpc_event_snapshot_t snapshot = {
        .video = {
            .ready = false,
            .streaming = WEBRTC_RPC_VIDEO_INACTIVE,
        },
    };
    char output[JETKVM_RPC_EVENT_OUTPUT_MAX];
    size_t output_size = 0;
    if (!render_video_input_event(ctx, &snapshot, output, sizeof(output),
                                  &output_size)) {
        portENTER_CRITICAL(&ctx->lock);
        ctx->stats.rpc_event_render_errors++;
        ctx->stats.rpc_event_notifications_dropped++;
        portEXIT_CRITICAL(&ctx->lock);
        return;
    }
    bool sent = data_channel_send(
        ctx, stream_id, ESP_PEER_DATA_CHANNEL_STRING,
        (const uint8_t *)output, output_size);
    portENTER_CRITICAL(&ctx->lock);
    if (sent && ctx->active_peer_session == session_id &&
        ctx->active_peer_generation == generation) {
        ctx->stats.rpc_event_notifications_sent++;
    } else if (!sent) {
        ctx->stats.rpc_event_notifications_dropped++;
    }
    portEXIT_CRITICAL(&ctx->lock);
}

int peer_on_channel_open(esp_peer_data_channel_info_t *channel, void *arg)
{
    transport_ctx_t *ctx = arg;
    if (!channel || !channel->label) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    bool recognized = false;
    bool reject_channel = false;
    bool guest = false;
    bool guest_current = false;
    webrtc_transport_guest_channels_t guest_handler = {0};
    portENTER_CRITICAL(&ctx->lock);
    ctx->stats.data_channel_opens++;
    if (guest_channels_label(channel->label)) {
        /* Terminal and file channels: the session that may use RPC may use
         * them too; OS Services decides whether it takes one. */
        guest = true;
        guest_current = peer_session_current_locked(ctx);
        guest_handler = ctx->guest_handler;
    } else if (strcmp(channel->label, "rpc") == 0) {
        ctx->rpc_data_channel_open = true;
        ctx->rpc_data_channel_stream_id = channel->stream_id;
        ctx->stats.rpc_data_channel_open = true;
        ctx->stats.rpc_data_channel_stream_id = channel->stream_id;
        ctx->rpc_event_next_hid_poll_us = 0;
        ctx->rpc_network_next_poll_us = 0;
        ctx->rpc_event_hid_valid = false;
        /* The UI waits for the version and capabilities instead of asking. */
        queue_rpc_events_locked(
            ctx, WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_VIDEO_INPUT) |
                 WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_LOCAL_VERSION) |
                 WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_DEVICE_CAPABILITIES));
        recognized = true;
    } else if (strcmp(channel->label, "hidrpc") == 0) {
        clear_hidrpc_state_notifications_locked(ctx);
        ctx->hidrpc_data_channel_open = true;
        ctx->hidrpc_data_channel_stream_id = channel->stream_id;
        ctx->stats.hidrpc_data_channel_open = true;
        ctx->stats.hidrpc_data_channel_stream_id = channel->stream_id;
        recognized = true;
    } else if (strcmp(channel->label, "hidrpc-unreliable-ordered") == 0) {
        ctx->hidrpc_unreliable_ordered_data_channel_open = true;
        ctx->hidrpc_unreliable_ordered_data_channel_stream_id =
            channel->stream_id;
        ctx->stats.hidrpc_unreliable_ordered_data_channel_open = true;
        ctx->stats.hidrpc_unreliable_ordered_data_channel_stream_id =
            channel->stream_id;
        recognized = true;
    } else if (strcmp(channel->label, "hidrpc-unreliable-nonordered") == 0) {
        ctx->hidrpc_unreliable_unordered_data_channel_open = true;
        ctx->hidrpc_unreliable_unordered_data_channel_stream_id =
            channel->stream_id;
        ctx->stats.hidrpc_unreliable_unordered_data_channel_open = true;
        ctx->stats.hidrpc_unreliable_unordered_data_channel_stream_id =
            channel->stream_id;
        recognized = true;
    } else {
        /* Production supports only RPC/HID DataChannels.  This also closes
         * every legacy upload label immediately without retaining its label,
         * protocol, writer, or buffering code in the firmware. */
        reject_channel = true;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (guest) {
        recognized = guest_current &&
            guest_channels_open(&ctx->guest_channels, &guest_handler,
                                channel->stream_id, channel->label);
    }
    ESP_LOGI(TAG, "data channel open label=%s stream=%u recognized=%d",
             channel->label, channel->stream_id, recognized);
    if (reject_channel) {
        close_data_channel(ctx, channel->label);
    } else if (guest && !recognized) {
        /* Several channels may share a guest label: close this stream. */
        char label[PEER_SCTP_STREAM_LABEL_CAPACITY];
        (void)peer_sctp_stream_label(label, channel->stream_id);
        close_data_channel(ctx, label);
    }
    return ESP_PEER_ERR_NONE;
}

int peer_on_channel_close(esp_peer_data_channel_info_t *channel, void *arg)
{
    transport_ctx_t *ctx = arg;
    if (!channel) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    bool reliable_hid_closed = false;
    uint32_t hid_generation = 0;
    webrtc_transport_guest_channels_t guest_handler = {0};
    data_channel_queue_drop_stream(&ctx->send_queue, channel->stream_id);
    portENTER_CRITICAL(&ctx->lock);
    publish_send_queue_locked(ctx);
    ctx->stats.data_channel_closes++;
    guest_handler = ctx->guest_handler;
    if (ctx->rpc_data_channel_open &&
        ctx->rpc_data_channel_stream_id == channel->stream_id) {
        if (ctx->reboot_slot.stream_id == channel->stream_id &&
            webrtc_reboot_slot_cancel(&ctx->reboot_slot)) {
            ctx->reboot_executing = false;
        }
        ctx->rpc_data_channel_open = false;
        ctx->stats.rpc_data_channel_open = false;
        clear_rpc_events_locked(ctx);
    }
    if (ctx->hidrpc_data_channel_open &&
        ctx->hidrpc_data_channel_stream_id == channel->stream_id) {
        ctx->hidrpc_data_channel_open = false;
        ctx->stats.hidrpc_data_channel_open = false;
        clear_hidrpc_state_notifications_locked(ctx);
        reliable_hid_closed = true;
        hid_generation = ctx->active_peer_generation;
        if (ctx->active_hid_generation == hid_generation) {
            ctx->active_hid_generation = 0;
            ctx->stats.hidrpc_active_generation = 0;
        }
    }
    if (ctx->hidrpc_unreliable_ordered_data_channel_open &&
        ctx->hidrpc_unreliable_ordered_data_channel_stream_id ==
            channel->stream_id) {
        ctx->hidrpc_unreliable_ordered_data_channel_open = false;
        ctx->stats.hidrpc_unreliable_ordered_data_channel_open = false;
    }
    if (ctx->hidrpc_unreliable_unordered_data_channel_open &&
        ctx->hidrpc_unreliable_unordered_data_channel_stream_id ==
            channel->stream_id) {
        ctx->hidrpc_unreliable_unordered_data_channel_open = false;
        ctx->stats.hidrpc_unreliable_unordered_data_channel_open = false;
    }
    portEXIT_CRITICAL(&ctx->lock);
    (void)guest_channels_closed(&ctx->guest_channels, &guest_handler,
                                channel->stream_id);
    if (reliable_hid_closed) {
        /* The reliable channel carries keyboard/button state.  Disable this
         * generation immediately so a missing reconnect cannot leave input
         * logically held while video continues. */
        hid_input_invalidate_generation(hid_generation,
                                        "reliable HID channel closed");
    }
    ESP_LOGI(TAG, "data channel close label=%s stream=%u",
             channel->label ? channel->label : "?", channel->stream_id);
    return ESP_PEER_ERR_NONE;
}




















typedef struct {
    transport_ctx_t *transport;
    uint32_t generation;
    bool movement_only;
} hidrpc_submit_context_t;

static void hidrpc_submit_event(const jetkvm_hidrpc_event_t *event, void *arg)
{
    hidrpc_submit_context_t *submit = arg;
    if (!submit || !submit->transport) {
        return;
    }
    transport_ctx_t *ctx = submit->transport;
    portENTER_CRITICAL(&ctx->lock);
    bool active = hid_generation_active_locked(ctx, submit->generation);
    if (!active) {
        ctx->stats.hidrpc_events_dropped++;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (!active) {
        return;
    }

    jetkvm_hid_input_submit_result_t result =
        submit->movement_only
            ? jetkvm_hid_input_submit_movement(submit->generation, event)
            : jetkvm_hid_input_submit_event(submit->generation, event);
    portENTER_CRITICAL(&ctx->lock);
    if (result == JETKVM_HID_INPUT_SUBMIT_ACCEPTED) {
        ctx->stats.hidrpc_events_accepted++;
    } else {
        ctx->stats.hidrpc_events_dropped++;
        if (result == JETKVM_HID_INPUT_SUBMIT_BACKPRESSURE) {
            ctx->stats.hidrpc_events_backpressured++;
        } else if (result == JETKVM_HID_INPUT_SUBMIT_NOT_SUPPORTED) {
            ctx->stats.hidrpc_events_unsupported++;
        }
    }
    portEXIT_CRITICAL(&ctx->lock);
}

void hid_input_invalidate_generation(uint32_t generation,
                                            const char *reason)
{
    if (generation == 0U) {
        return;
    }
    esp_err_t err = jetkvm_hid_input_invalidate_generation(generation);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "HID generation %" PRIu32
                 " invalidation failed (%s): %s",
                 generation, reason ? reason : "unspecified",
                 esp_err_to_name(err));
    }
}

typedef struct {
    transport_ctx_t *transport;
    uint32_t session_id;
    uint32_t generation;
    uint16_t stream_id;
    bool accepted;
    /* The send queue's number for the response if it waits there. */
    uint32_t first_queued;
} reboot_request_context_t;

static jetkvm_reboot_request_result_t rpc_request_reboot(bool force,
                                                         void *arg)
{
    reboot_request_context_t *request = arg;
    if (!request || !request->transport) {
        return JETKVM_REBOOT_REQUEST_UNAVAILABLE;
    }
    transport_ctx_t *ctx = request->transport;
    jetkvm_reboot_request_result_t result =
        JETKVM_REBOOT_REQUEST_UNAVAILABLE;
    portENTER_CRITICAL(&ctx->lock);
    ctx->stats.reboot_requests++;
    bool current = reboot_rpc_peer_current_locked(
        ctx, request->session_id, request->generation, request->stream_id);
    if (current &&
        webrtc_reboot_slot_accept(
            &ctx->reboot_slot, force, request->session_id,
            request->generation, request->stream_id)) {
        request->accepted = true;
        ctx->stats.reboot_accepted++;
        result = JETKVM_REBOOT_REQUEST_ACCEPTED;
    } else if (current &&
               (ctx->reboot_slot.occupied || ctx->reboot_executing)) {
        ctx->stats.reboot_busy++;
        result = JETKVM_REBOOT_REQUEST_BUSY;
    }
    portEXIT_CRITICAL(&ctx->lock);
    return result;
}

/* This runs inline on the peer task, immediately after its response send
 * attempt.  There is deliberately no task notification before both wire
 * attempts finish, so the reboot worker cannot race the RPC response. */
static void complete_accepted_reboot_request(
    reboot_request_context_t *request, bool response_expected,
    bool response_sent)
{
    if (!request || !request->accepted || !request->transport) {
        return;
    }
    transport_ctx_t *ctx = request->transport;

    portENTER_CRITICAL(&ctx->lock);
    bool owned = webrtc_reboot_slot_matches(
        &ctx->reboot_slot, request->session_id, request->generation,
        request->stream_id);
    bool current_before_event = owned && reboot_rpc_peer_current_locked(
        ctx, request->session_id, request->generation, request->stream_id);
    if (response_expected && response_sent) {
        ctx->stats.reboot_response_sent++;
    } else {
        /* A JSON-RPC notification has no response to order before the event,
         * so it is deliberately not allowed to release the reboot worker. */
        ctx->stats.reboot_send_failures++;
    }
    portEXIT_CRITICAL(&ctx->lock);

    char will_reboot[96];
    size_t will_reboot_size = 0;
    bool rendered = webrtc_reboot_render_will_event(
        will_reboot, sizeof(will_reboot), &will_reboot_size);
    bool event_sent = current_before_event && rendered &&
        data_channel_send_exact_rpc_peer(
        ctx, request->session_id, request->generation, request->stream_id,
        (const uint8_t *)will_reboot, will_reboot_size);

    TaskHandle_t task = NULL;
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&ctx->lock);
    owned = webrtc_reboot_slot_matches(
        &ctx->reboot_slot, request->session_id, request->generation,
        request->stream_id);
    if (event_sent) {
        ctx->stats.reboot_will_reboot_sent++;
    } else {
        ctx->stats.reboot_send_failures++;
    }
    bool current_after_event = owned && reboot_rpc_peer_current_locked(
        ctx, request->session_id, request->generation, request->stream_id);
    bool both_sends_succeeded = response_expected && response_sent &&
                                event_sent;
    if (current_after_event && both_sends_succeeded &&
        webrtc_reboot_slot_mark_dispatch_ready(
            &ctx->reboot_slot, request->session_id,
            request->generation, request->stream_id)) {
        ctx->reboot_slot.dispatched_us = now_us;
        ctx->reboot_slot.sent_us = now_us;
        ctx->reboot_slot.first_queued = request->first_queued;
        ctx->reboot_slot.last_queued = ctx->send_queue.queued;
        publish_send_queue_locked(ctx);
        task = ctx->reboot_task;
    } else if (owned) {
        webrtc_reboot_slot_clear(&ctx->reboot_slot);
        ctx->reboot_executing = false;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (task) {
        xTaskNotifyGive(task);
    }
}

int peer_on_data(esp_peer_data_frame_t *frame, void *arg)
{
    transport_ctx_t *ctx = arg;
    if (!frame || !frame->data || frame->size <= 0) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    bool is_rpc = false;
    bool is_hidrpc = false;
    bool is_hidrpc_reliable = false;
    uint32_t rpc_access_epoch = 0;
    bool rpc_cloud_authority = false;
    uint32_t rpc_input_generation = 0;
    uint32_t hid_input_generation = 0;
    uint32_t input_session_id = 0;
    bool guest_current = false;
    webrtc_transport_guest_channels_t guest_handler = {0};
    portENTER_CRITICAL(&ctx->lock);
    ctx->stats.data_channel_messages_rx++;
    ctx->stats.data_channel_bytes_rx += (uint32_t)frame->size;
    is_rpc = ctx->rpc_data_channel_open &&
             ctx->rpc_data_channel_stream_id == frame->stream_id;
    is_hidrpc_reliable = ctx->hidrpc_data_channel_open &&
                         ctx->hidrpc_data_channel_stream_id == frame->stream_id;
    is_hidrpc = is_hidrpc_reliable ||
        (ctx->hidrpc_unreliable_ordered_data_channel_open &&
         ctx->hidrpc_unreliable_ordered_data_channel_stream_id ==
             frame->stream_id) ||
        (ctx->hidrpc_unreliable_unordered_data_channel_open &&
         ctx->hidrpc_unreliable_unordered_data_channel_stream_id ==
             frame->stream_id);
    if (is_rpc && rpc_event_session_current_locked(ctx)) {
        rpc_input_generation = ctx->active_peer_generation;
        rpc_access_epoch = ctx->local_access_epoch;
        rpc_cloud_authority = ctx->active_peer_role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED;
        input_session_id = ctx->active_peer_session;
    }
    if ((is_rpc || is_hidrpc) &&
        hid_generation_active_locked(ctx, ctx->active_peer_generation)) {
        hid_input_generation = ctx->active_peer_generation;
    }
    if (!is_rpc && !is_hidrpc) {
        guest_current = peer_session_current_locked(ctx);
        guest_handler = ctx->guest_handler;
    }
    portEXIT_CRITICAL(&ctx->lock);

    hidrpc_submit_context_t hid_submit = {
        .transport = ctx,
        .generation = hid_input_generation,
        .movement_only = is_hidrpc && !is_hidrpc_reliable,
    };

    if (is_hidrpc) {
        if (frame->type != ESP_PEER_DATA_CHANNEL_DATA) {
            portENTER_CRITICAL(&ctx->lock);
            ctx->stats.data_channel_protocol_errors++;
            portEXIT_CRITICAL(&ctx->lock);
            return ESP_PEER_ERR_NONE;
        }
        if (hid_input_generation == 0U) {
            portENTER_CRITICAL(&ctx->lock);
            ctx->stats.hidrpc_inactive_messages++;
            portEXIT_CRITICAL(&ctx->lock);
            return ESP_PEER_ERR_NONE;
        }
        uint8_t response[2];
        size_t response_size = 0;
        jetkvm_hidrpc_context_t hidrpc_context = {
            .on_event = hidrpc_submit_event,
            .ctx = &hid_submit,
        };
        jetkvm_control_result_t result =
            jetkvm_control_handle_hidrpc_with_context(
                &hidrpc_context, frame->data, (size_t)frame->size,
                response, sizeof(response), &response_size);
        if (result == JETKVM_CONTROL_RESPONSE_READY &&
            is_hidrpc_reliable) {
            bool sent = data_channel_send(
                ctx, frame->stream_id, ESP_PEER_DATA_CHANNEL_DATA,
                response, response_size);
            portENTER_CRITICAL(&ctx->lock);
            if (sent) {
                ctx->stats.hidrpc_handshakes++;
                bool current_handshake =
                    frame->stream_id ==
                        ctx->hidrpc_data_channel_stream_id &&
                    hid_input_generation != 0U &&
                    hid_generation_active_locked(ctx, hid_input_generation);
                if (current_handshake) {
                    ctx->hidrpc_handshake_complete = true;
                    ctx->stats.hidrpc_handshake_complete = true;
                    ctx->hidrpc_macro_observed_valid = false;
                    queue_hidrpc_state_locked(
                        ctx, WEBRTC_HIDRPC_STATE_ALL_MASK);
                }
            }
            portEXIT_CRITICAL(&ctx->lock);
        } else if (result == JETKVM_CONTROL_EVENT_HANDLED) {
            /* The callback copied or rejected the event without blocking.
             * Its outcome is accounted separately from wire validity. */
        } else if (result == JETKVM_CONTROL_NO_RESPONSE) {
            portENTER_CRITICAL(&ctx->lock);
            ctx->stats.hidrpc_unsupported_messages++;
            portEXIT_CRITICAL(&ctx->lock);
        } else {
            portENTER_CRITICAL(&ctx->lock);
            ctx->stats.data_channel_protocol_errors++;
            portEXIT_CRITICAL(&ctx->lock);
        }
        return ESP_PEER_ERR_NONE;
    }

    if (is_rpc) {
        if (frame->type != ESP_PEER_DATA_CHANNEL_STRING) {
            ESP_LOGW(TAG, "RPC rejected frame type=%d size=%d stream=%u",
                     (int)frame->type, (int)frame->size,
                     (unsigned)frame->stream_id);
            portENTER_CRITICAL(&ctx->lock);
            ctx->stats.data_channel_protocol_errors++;
            portEXIT_CRITICAL(&ctx->lock);
            return ESP_PEER_ERR_NONE;
        }
        /* peer_task is the sole caller of esp_peer_main_loop(), hence the sole
         * owner of peer_on_data and of this transport-lifetime scratch. */
        char *response = ctx->rpc_scratch;
        if (response == NULL) {
            ESP_LOGW(TAG,
                     "RPC scratch unavailable: int_free=%u int_largest=%u psram_free=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(
                         MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            portENTER_CRITICAL(&ctx->lock);
            ctx->stats.data_channel_protocol_errors++;
            portEXIT_CRITICAL(&ctx->lock);
            return ESP_PEER_ERR_NO_MEM;
        }
        char device_id[32] = "unknown_device_id";
        char video_codec_preference[
            JETKVM_VIDEO_CODEC_PREFERENCE_CAPACITY] = "auto";
        if (jetkvm_device_state_get_id(device_id, sizeof(device_id)) != ESP_OK) {
            memcpy(device_id, "unknown_device_id",
                   sizeof("unknown_device_id"));
        }
        jetkvm_device_cloud_public_state_t cloud_state = { 0 };
        bool have_cloud_state =
            jetkvm_device_state_cloud_public_snapshot(&cloud_state) == ESP_OK;
        (void)jetkvm_settings_get_video_codec_preference(
            video_codec_preference, sizeof(video_codec_preference));
        char app_version[JETKVM_OTA_VERSION_CAPACITY];
        char system_version[JETKVM_OTA_VERSION_CAPACITY];
        jetkvm_ota_local_versions(app_version, sizeof(app_version),
                                  system_version, sizeof(system_version));
        size_t response_size = 0;
        reboot_request_context_t reboot_request = {
            .transport = ctx,
            .session_id = input_session_id,
            .generation = rpc_input_generation,
            .stream_id = frame->stream_id,
        };
        jetkvm_control_context_t control_context = {
            .app_version = app_version,
            .system_version = system_version,
            .sku = JETKVM_DEVICE_SKU,
            .device_id = device_id,
            .cloud_registered = have_cloud_state && cloud_state.registered,
            .cloud_url = have_cloud_state ? cloud_state.cloud_url : NULL,
            .cloud_app_url = have_cloud_state ?
                                 cloud_state.cloud_app_url : NULL,
            .stream_quality_factor = jetkvm_video_quality_get(),
            .get_video_log_status = rpc_get_video_log_status_adapter,
            .video_log_status_ctx = ctx,
            .queue_quality_change = rpc_queue_video_quality_change,
            .video_codec_preference = video_codec_preference,
            .set_video_codec_preference = rpc_set_video_codec_preference,
            .set_video_stream_paused = rpc_set_video_stream_paused_adapter,
            .set_video_stream_paused_ctx = ctx,
            .set_cloud_urls = rpc_set_cloud_urls,
            .request_reboot = rpc_request_reboot,
            .request_reboot_ctx = &reboot_request,
            .get_keyboard_led_state =
                jetkvm_hid_input_get_keyboard_led_state,
            .get_key_down_state = jetkvm_hid_input_get_key_down_state,
            .get_usb_state = jetkvm_hid_input_get_usb_state,
            .usb_configuration = {
                .get = rpc_get_usb_configuration,
            },
            .virtual_media = {
                .get_state = rpc_get_virtual_media_state_adapter,
                .get_space = rpc_get_virtual_media_space_adapter,
                .list_files = rpc_list_virtual_media_files_adapter,
                .delete_file = rpc_delete_virtual_media_file_adapter,
                .ctx = ctx,
            },
            .wake_host = jetkvm_hid_input_wake_host,
            .send_wol = rpc_send_wol,
            .wol_devices = {
                .get = rpc_get_wol_devices_adapter,
                .set = rpc_set_wol_devices_adapter,
            },
            .keyboard_macros = {
                .get = rpc_get_keyboard_macros_adapter,
                .set = rpc_set_keyboard_macros_adapter,
            },
            .jiggler = {
                .get_state = rpc_get_jiggler_state_adapter,
                .set_state = rpc_set_jiggler_state_adapter,
                .get_config = rpc_get_jiggler_config_adapter,
                .set_config = rpc_set_jiggler_config_adapter,
            },
        };
        /* Taken on demand by getNetworkState through the hook; every other
         * RPC frame skips the network lock and the lwIP lease inspection. */
        control_context.snapshot_network = jetkvm_network_snapshot;
        control_context.get_network_settings = jetkvm_network_settings_get;
        control_context.get_wifi_settings = jetkvm_c5_wifi_get_settings;
        control_context.set_wifi_band = jetkvm_c5_wifi_set_band;
        control_context.scan_wifi_networks = jetkvm_c5_wifi_scan_networks;
        control_context.save_wifi_network = jetkvm_c5_wifi_save_network;
        control_context.remove_wifi_network = jetkvm_c5_wifi_remove_network;
        control_context.connect_wifi_network = jetkvm_c5_wifi_connect_network;
        control_context.set_network_settings = jetkvm_network_settings_set;
        control_context.renew_dhcp_lease = jetkvm_network_renew_dhcp;
        control_context.get_mac_address_settings = jetkvm_mac_address_settings_get;
        control_context.set_mac_address_settings = jetkvm_mac_address_settings_set;
        control_context.get_public_ip = jetkvm_cloud_public_ip_get;
        /* Methods that block (USB re-enumeration, HTTP(S), flash, the card
         * work of an upload's start) run on the RPC worker; here they only
         * report BUSY or unavailable. */
        rpc_worker_peer_fallbacks(&control_context);
        if (hid_input_generation != 0U) {
            control_context.hidrpc = (jetkvm_hidrpc_context_t) {
                .on_event = hidrpc_submit_event,
                .ctx = &hid_submit,
            };
        }
        portENTER_CRITICAL(&ctx->lock);
        control_context.video.ready = ctx->mode_valid;
        control_context.video.streaming =
            ctx->started && ctx->mode_valid && !ctx->video_paused ?
                JETKVM_VIDEO_STREAM_ACTIVE : JETKVM_VIDEO_STREAM_INACTIVE;
        control_context.video.upload_pauses_video =
            (ctx->video_pause_sources &
             WEBRTC_VIDEO_PAUSE_STORAGE_UPLOAD) != 0;
        control_context.video.error =
            ctx->mode_valid || ctx->video_switching ? "" : "no_signal";
        control_context.video.width = ctx->mode_valid ? ctx->width : 0;
        control_context.video.height = ctx->mode_valid ? ctx->height : 0;
        control_context.video.fps = ctx->mode_valid ? ctx->fps : 0;
        portEXIT_CRITICAL(&ctx->lock);
        bool access_locked = !rpc_cloud_authority && jetkvm_access_lock_epoch(rpc_access_epoch);
        portENTER_CRITICAL(&ctx->lock);
        bool current = rpc_input_generation && rpc_event_session_current_locked(ctx) &&
            ctx->generation == rpc_input_generation && ctx->session_id == input_session_id;
        portEXIT_CRITICAL(&ctx->lock);
        bool admitted = current && (rpc_cloud_authority || access_locked);
        const rpc_worker_origin_t origin = {
            .session_id = input_session_id,
            .generation = rpc_input_generation,
            .stream_id = frame->stream_id,
            .access_epoch = rpc_access_epoch,
            .cloud_authority = rpc_cloud_authority,
        };
        if (admitted && rpc_worker_defer(frame->data, (size_t)frame->size, &origin)) {
            if (access_locked) jetkvm_access_unlock();
            return ESP_PEER_ERR_NONE;
        }
        jetkvm_control_result_t result = admitted ?
            jetkvm_control_handle_rpc_with_context(
                &control_context, frame->data, (size_t)frame->size,
                response, JETKVM_MEMORY_RPC_SCRATCH_BYTES, &response_size) : JETKVM_CONTROL_NO_RESPONSE;
        if (access_locked) jetkvm_access_unlock();
        bool response_expected = result == JETKVM_CONTROL_RESPONSE_READY;
        bool sent = false;
        if (response_expected) {
            reboot_request.first_queued = ctx->send_queue.queued + 1U;
            sent = reboot_request.accepted ?
                data_channel_send_exact_rpc_peer(
                    ctx, reboot_request.session_id,
                    reboot_request.generation, reboot_request.stream_id,
                    (const uint8_t *)response, response_size) :
                data_channel_send(
                    ctx, frame->stream_id, ESP_PEER_DATA_CHANNEL_STRING,
                    (const uint8_t *)response, response_size);
            portENTER_CRITICAL(&ctx->lock);
            if (sent) {
                ctx->stats.rpc_responses++;
            }
            portEXIT_CRITICAL(&ctx->lock);
        } else if (result != JETKVM_CONTROL_NO_RESPONSE) {
            ESP_LOGW(TAG,
                     "RPC handler failed result=%d input=%d response=%u int_free=%u int_largest=%u psram_free=%u",
                     (int)result, (int)frame->size,
                     (unsigned)response_size,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(
                         MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            portENTER_CRITICAL(&ctx->lock);
            ctx->stats.data_channel_protocol_errors++;
            portEXIT_CRITICAL(&ctx->lock);
        }
        complete_accepted_reboot_request(
            &reboot_request, response_expected, sent);
        return ESP_PEER_ERR_NONE;
    }

    /* A revoked session's keystrokes are not delivered while its peer
     * waits for teardown. */
    if (guest_current &&
        guest_channels_message(
            &ctx->guest_channels, &guest_handler, frame->stream_id,
            frame->type == ESP_PEER_DATA_CHANNEL_STRING, frame->data,
            (size_t)frame->size)) {
        return ESP_PEER_ERR_NONE;
    }

    portENTER_CRITICAL(&ctx->lock);
    ctx->stats.data_channel_protocol_errors++;
    portEXIT_CRITICAL(&ctx->lock);
    return ESP_PEER_ERR_NONE;
}

esp_err_t webrtc_transport_send_rpc_notification(const uint8_t *data,
                                                 size_t size)
{
    if (!data || size == 0 || size > WEBRTC_OTA_NOTIFY_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    transport_ctx_t *ctx = webrtc_transport_context();
    portENTER_CRITICAL(&ctx->lock);
    if (!ctx->started) {
        portEXIT_CRITICAL(&ctx->lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->ota_notify_count >= WEBRTC_OTA_NOTIFY_SLOTS) {
        /* Drop the oldest pending payload, never the producer.  A fresh
         * progress event carries strictly newer state than the victim. */
        ctx->ota_notify_head =
            (uint8_t)((ctx->ota_notify_head + 1U) % WEBRTC_OTA_NOTIFY_SLOTS);
        ctx->ota_notify_count--;
        ctx->ota_notify_dropped++;
    }
    uint8_t slot = (uint8_t)((ctx->ota_notify_head + ctx->ota_notify_count) %
                             WEBRTC_OTA_NOTIFY_SLOTS);
    memcpy(ctx->ota_notify_payloads[slot], data, size);
    ctx->ota_notify_sizes[slot] = (uint16_t)size;
    ctx->ota_notify_count++;
    portEXIT_CRITICAL(&ctx->lock);
    notify_peer_task(ctx);
    return ESP_OK;
}

void drain_pending_rpc_notifications(transport_ctx_t *ctx)
{
    for (;;) {
        char payload[WEBRTC_OTA_NOTIFY_BYTES];
        uint16_t size = 0;
        uint16_t stream_id = 0;
        bool channel_open = false;
        portENTER_CRITICAL(&ctx->lock);
        if (ctx->ota_notify_count > 0U) {
            uint8_t head = ctx->ota_notify_head;
            size = ctx->ota_notify_sizes[head];
            memcpy(payload, ctx->ota_notify_payloads[head], size);
            ctx->ota_notify_head =
                (uint8_t)((head + 1U) % WEBRTC_OTA_NOTIFY_SLOTS);
            ctx->ota_notify_count--;
            channel_open = ctx->rpc_data_channel_open;
            stream_id = ctx->rpc_data_channel_stream_id;
        }
        portEXIT_CRITICAL(&ctx->lock);
        if (size == 0) {
            return;
        }
        if (!channel_open) {
            portENTER_CRITICAL(&ctx->lock);
            ctx->ota_notify_dropped++;
            portEXIT_CRITICAL(&ctx->lock);
            continue;
        }
        (void)data_channel_send(ctx, stream_id, ESP_PEER_DATA_CHANNEL_STRING,
                                (const uint8_t *)payload, size);
    }
}

/* Refresh lease expiry after renewals, and clear stale lease cards when the
 * interface changes. Poll the firmware state once a second while connected. */
void poll_rpc_network_state(transport_ctx_t *ctx, int64_t now_us)
{
    portENTER_CRITICAL(&ctx->lock);
    if (!rpc_event_session_current_locked(ctx) || now_us < ctx->rpc_network_next_poll_us) {
        portEXIT_CRITICAL(&ctx->lock);
        return;
    }
    ctx->rpc_network_next_poll_us = now_us + 1000000;
    uint32_t generation = ctx->active_peer_generation;
    uint16_t stream_id = ctx->rpc_data_channel_stream_id;
    portEXIT_CRITICAL(&ctx->lock);

    jetkvm_network_state_t state;
    jetkvm_network_snapshot(&state);
    char output[1536];
    size_t length;
    if (!jetkvm_control_render_network_notification(&state, output, sizeof(output), &length)) return;
    portENTER_CRITICAL(&ctx->lock);
    bool current = rpc_event_session_current_locked(ctx) &&
        ctx->active_peer_generation == generation && ctx->rpc_data_channel_stream_id == stream_id;
    portEXIT_CRITICAL(&ctx->lock);
    if (current) data_channel_send(ctx, stream_id, ESP_PEER_DATA_CHANNEL_STRING, (const uint8_t *)output, length);
}

void webrtc_transport_video_state_changed(void)
{
    transport_ctx_t *ctx = webrtc_transport_context();
    portENTER_CRITICAL(&ctx->lock);
    queue_video_input_event_locked(ctx);
    portEXIT_CRITICAL(&ctx->lock);
    notify_peer_task(ctx);
}

void rpc_worker_notify_done(void)
{
    notify_peer_task(webrtc_transport_context());
}

void send_pending_rpc_worker_replies(transport_ctx_t *ctx)
{
    rpc_worker_reply_t reply;
    while (rpc_worker_take_reply(&reply)) {
        if (data_channel_send_exact_rpc_peer(ctx, reply.session_id, reply.generation,
                reply.stream_id, (const uint8_t *)rpc_worker_reply_text(&reply),
                reply.length)) {
            portENTER_CRITICAL(&ctx->lock);
            ctx->stats.rpc_responses++;
            portEXIT_CRITICAL(&ctx->lock);
        }
        /* Sent, refused, or for a session that has gone: the buffer is done. */
        rpc_worker_release_reply(&reply);
        if (reply.will_reboot) {
            char event[128];
            size_t length = 0;
            if (webrtc_reboot_render_will_event(event, sizeof(event), &length)) {
                (void)data_channel_send_exact_rpc_peer(ctx, reply.session_id, reply.generation,
                    reply.stream_id, (const uint8_t *)event, length);
            }
            jetkvm_factory_reset_dispatch_complete();
        }
        if (reply.publish_deregistration) {
            jetkvm_device_state_cloud_finish_deregistration();
        }
    }
}

/* --- OS Services channels ------------------------------------------------ */

void webrtc_transport_set_guest_channels(
    const webrtc_transport_guest_channels_t *channels)
{
    transport_ctx_t *ctx = webrtc_transport_context();
    portENTER_CRITICAL(&ctx->lock);
    ctx->guest_handler = channels ? *channels :
        (webrtc_transport_guest_channels_t){0};
    portEXIT_CRITICAL(&ctx->lock);
}

void webrtc_transport_guest_output_pending(void)
{
    transport_ctx_t *ctx = webrtc_transport_context();
    atomic_store(&ctx->guest_output_pending, true);
    notify_peer_task(ctx);
}

static guest_send_result_t guest_send(void *arg, uint16_t stream_id,
                                      bool text, const uint8_t *data,
                                      size_t length)
{
    transport_ctx_t *ctx = arg;
    /* RPC and HIDRPC messages that wait for the send buffer take the room
     * the browser's acknowledgements free; output waits until they went. */
    if (ctx->send_queue.head != NULL) {
        return GUEST_SEND_BLOCKED;
    }
    esp_peer_handle_t peer = NULL;
    portENTER_CRITICAL(&ctx->lock);
    if (peer_session_current_locked(ctx)) {
        peer = ctx->peer;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (peer == NULL || length > INT_MAX) {
        return GUEST_SEND_FAILED;
    }
    esp_peer_data_frame_t frame = {
        .type = text ? ESP_PEER_DATA_CHANNEL_STRING : ESP_PEER_DATA_CHANNEL_DATA,
        .stream_id = stream_id,
        .data = (uint8_t *)data,
        .size = (int)length,
    };
    const int ret = esp_peer_send_data(peer, &frame);
    portENTER_CRITICAL(&ctx->lock);
    if (ret == ESP_PEER_ERR_NONE) {
        ctx->stats.data_channel_messages_tx++;
        ctx->stats.data_channel_bytes_tx += length;
    } else if (ret != ESP_PEER_ERR_WOULD_BLOCK) {
        ctx->stats.data_channel_send_errors++;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (ret == ESP_PEER_ERR_WOULD_BLOCK) {
        return GUEST_SEND_BLOCKED;
    }
    return ret == ESP_PEER_ERR_NONE ? GUEST_SEND_OK : GUEST_SEND_FAILED;
}

/* By stream: the browser chose the label, and others may share it. The
 * vendor reports the close inside the call, which frees the slot. */
static void guest_close(void *arg, uint16_t stream_id)
{
    char label[PEER_SCTP_STREAM_LABEL_CAPACITY];
    (void)peer_sctp_stream_label(label, stream_id);
    close_data_channel(arg, label);
}

void send_guest_channel_output(transport_ctx_t *ctx, int64_t now_us)
{
    if (now_us < ctx->guest_output_not_before_us) {
        return;
    }
    const bool asked = atomic_exchange(&ctx->guest_output_pending, false);
    if (!asked && now_us < ctx->guest_output_next_poll_us) {
        return;
    }
    ctx->guest_output_next_poll_us = now_us + GUEST_OUTPUT_POLL_US;
    webrtc_transport_guest_channels_t handler;
    bool current = false;
    portENTER_CRITICAL(&ctx->lock);
    handler = ctx->guest_handler;
    current = peer_session_current_locked(ctx);
    portEXIT_CRITICAL(&ctx->lock);
    if (!current) {
        /* Teardown closes the channels; their output is not wanted. */
        return;
    }
    const guest_peer_t peer = {
        .send = guest_send,
        .close = guest_close,
        .ctx = ctx,
    };
    const int64_t start_us = esp_timer_get_time();
    const bool more = guest_channels_send(&ctx->guest_channels, &handler,
                                          &peer, GUEST_CHANNELS_SEND_BUDGET);
    const int64_t end_us = esp_timer_get_time();
    ctx->guest_output_not_before_us =
        end_us + GUEST_OUTPUT_PACE * (end_us - start_us);
    if (more) {
        /* A full send buffer or the budget: a later loop tries again. */
        atomic_store(&ctx->guest_output_pending, true);
    }
}

void reset_guest_channels(transport_ctx_t *ctx)
{
    webrtc_transport_guest_channels_t handler;
    portENTER_CRITICAL(&ctx->lock);
    handler = ctx->guest_handler;
    portEXIT_CRITICAL(&ctx->lock);
    guest_channels_reset(&ctx->guest_channels, &handler);
}

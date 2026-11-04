#include "webrtc_data_channels_internal.h"
#include "jetkvm_virtual_media_http.h"
#include "jetkvm_access.h"
/* WebRTC signaling queues and local/cloud session ownership. */
#include "webrtc_sessions_internal.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "jetkvm_memory_policy.h"
#include "signaling_budget.h"
#include "webrtc_transport_private.h"

static const char *TAG = "webrtc_sessions";

static void owned_ice_server_list_free(owned_ice_server_list_t *list)
{
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->count; ++i) {
        jetkvm_memory_policy_control_free(list->servers[i].stun_url);
        jetkvm_memory_policy_control_free(list->servers[i].user);
        jetkvm_memory_policy_control_free(list->servers[i].psw);
    }
    jetkvm_memory_policy_control_free(list);
}
static esp_err_t copy_bounded_string(const char *source, size_t max_size,
                                     bool required, char **destination)
{
    *destination = NULL;
    if (!source) {
        return required ? ESP_ERR_INVALID_ARG : ESP_OK;
    }
    size_t size = strnlen(source, max_size + 1U);
    if ((required && size == 0) || size > max_size) {
        return ESP_ERR_INVALID_ARG;
    }
    char *copy = jetkvm_memory_policy_control_alloc(size + 1U);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, source, size + 1U);
    *destination = copy;
    return ESP_OK;
}

esp_err_t owned_ice_server_list_create(
    const webrtc_transport_ice_server_t *servers, size_t server_count,
    owned_ice_server_list_t **result)
{
    if (!result || server_count > WEBRTC_TRANSPORT_MAX_ICE_SERVERS ||
        (server_count != 0 && !servers)) {
        return ESP_ERR_INVALID_ARG;
    }
    *result = NULL;
    if (server_count == 0) {
        return ESP_OK;
    }
    owned_ice_server_list_t *list =
        jetkvm_memory_policy_control_alloc(sizeof(*list));
    if (!list) {
        return ESP_ERR_NO_MEM;
    }
    memset(list, 0, sizeof(*list));
    list->refs = 1;
    list->count = (uint8_t)server_count;
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < server_count; ++i) {
        err = copy_bounded_string(
            servers[i].url, WEBRTC_TRANSPORT_MAX_ICE_SERVER_URL_SIZE,
            true, &list->servers[i].stun_url);
        if (err == ESP_OK) {
            err = copy_bounded_string(
                servers[i].username,
                WEBRTC_TRANSPORT_MAX_ICE_USERNAME_SIZE,
                false, &list->servers[i].user);
        }
        if (err == ESP_OK) {
            err = copy_bounded_string(
                servers[i].credential,
                WEBRTC_TRANSPORT_MAX_ICE_CREDENTIAL_SIZE,
                false, &list->servers[i].psw);
        }
        if (err != ESP_OK) {
            owned_ice_server_list_free(list);
            return err;
        }
    }
    *result = list;
    return ESP_OK;
}

void owned_ice_server_list_retain_locked(
    owned_ice_server_list_t *list)
{
    if (list) {
        list->refs++;
    }
}

void owned_ice_server_list_release(
    transport_ctx_t *ctx, owned_ice_server_list_t *list)
{
    if (!list) {
        return;
    }
    bool release = false;
    portENTER_CRITICAL(&ctx->lock);
    if (list->refs != 0) {
        list->refs--;
        release = list->refs == 0;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (release) {
        owned_ice_server_list_free(list);
    }
}

void record_signaling_drop(transport_ctx_t *ctx)
{
    portENTER_CRITICAL(&ctx->lock);
    ctx->stats.signaling_messages_dropped++;
    portEXIT_CRITICAL(&ctx->lock);
}

static bool release_signaling_budget(transport_ctx_t *ctx, size_t bytes)
{
    if (bytes == 0U) {
        return true;
    }
    portENTER_CRITICAL(&ctx->lock);
    const bool released = webrtc_signaling_budget_release(
        &ctx->signaling_budget, bytes);
    portEXIT_CRITICAL(&ctx->lock);
    if (!released) {
        ESP_LOGE(TAG, "signaling budget release invariant failed: %u bytes",
                 (unsigned)bytes);
    }
    return released;
}

static webrtc_signal_event_t *allocate_outbound_event(
    webrtc_signal_type_t type, const uint8_t *data, size_t size)
{
    if ((type != WEBRTC_SIGNAL_SDP &&
         type != WEBRTC_SIGNAL_CANDIDATE) ||
        data == NULL || size == 0U ||
        size > WEBRTC_SIGNAL_PAYLOAD_MAX_BYTES) {
        return NULL;
    }
    const size_t allocation_size = sizeof(webrtc_signal_event_t) + size + 1U;
    webrtc_signal_event_t *event =
        jetkvm_memory_policy_control_alloc(allocation_size);
    if (event == NULL) {
        return NULL;
    }
    memset(event, 0, sizeof(*event));
    event->type = type;
    event->data = (char *)(event + 1);
    event->size = size;
    event->allocation = event;
    memcpy(event->data, data, size);
    event->data[size] = '\0';
    return event;
}

static void free_event_data(transport_ctx_t *ctx,
                            webrtc_signal_event_t *event)
{
    if (event == NULL) {
        return;
    }
    void *allocation = event->allocation;
    const size_t budget_bytes = event->budget_bytes;
    const bool copied_owner = allocation != NULL && allocation != event;
    if (allocation != NULL) {
        jetkvm_memory_policy_control_free(allocation);
    }
    (void)release_signaling_budget(ctx, budget_bytes);
    if (copied_owner || allocation == NULL) {
        memset(event, 0, sizeof(*event));
    }
}

void free_inbound_signal(transport_ctx_t *ctx,
                                inbound_signal_t *signal)
{
    if (signal == NULL) {
        return;
    }
    const size_t budget_bytes = signal->budget_bytes;
    jetkvm_memory_policy_control_free(signal);
    (void)release_signaling_budget(ctx, budget_bytes);
}

bool outbound_push(transport_ctx_t *ctx, webrtc_signal_type_t type,
                          const uint8_t *data, size_t size)
{
    webrtc_signal_event_t *copy = allocate_outbound_event(type, data, size);
    if (copy == NULL) {
        record_signaling_drop(ctx);
        return false;
    }
    const size_t resident_bytes = sizeof(*copy) + size + 1U;

    webrtc_signal_event_t *evicted = NULL;
    webrtc_transport_cloud_signal_cb_t cloud_cb = NULL;
    void *cloud_cb_ctx = NULL;
    webrtc_transport_session_role_t role =
        WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING;
    uint32_t session_id = 0;
    uint32_t generation = 0;
    portENTER_CRITICAL(&ctx->lock);
    if (!ctx->session_active || ctx->active_peer_session != ctx->session_id ||
        ctx->active_peer_generation == 0 ||
        ctx->active_peer_generation != ctx->generation ||
        ctx->active_peer_role != ctx->session_role) {
        ctx->stats.signaling_messages_dropped++;
        portEXIT_CRITICAL(&ctx->lock);
        free_event_data(ctx, copy);
        return false;
    }
    role = ctx->active_peer_role;
    session_id = ctx->active_peer_session;
    generation = ctx->active_peer_generation;
    if (role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED) {
        cloud_cb = ctx->cloud_signal_cb;
        cloud_cb_ctx = ctx->cloud_signal_ctx;
        if (!cloud_cb ||
            (type != WEBRTC_SIGNAL_SDP &&
             type != WEBRTC_SIGNAL_CANDIDATE) ||
            !webrtc_cloud_callback_gate_try_begin(
                &ctx->cloud_callback_gate, session_id, generation)) {
            ctx->stats.signaling_messages_dropped++;
            portEXIT_CRITICAL(&ctx->lock);
            free_event_data(ctx, copy);
            return false;
        }
        portEXIT_CRITICAL(&ctx->lock);
        webrtc_transport_cloud_signal_type_t cloud_type =
            type == WEBRTC_SIGNAL_SDP ?
                WEBRTC_TRANSPORT_CLOUD_SIGNAL_ANSWER :
                WEBRTC_TRANSPORT_CLOUD_SIGNAL_CANDIDATE;
        bool delivered = cloud_cb(session_id, generation, cloud_type,
                                  copy->data, copy->size, cloud_cb_ctx);
        portENTER_CRITICAL(&ctx->lock);
        webrtc_cloud_callback_gate_end(
            &ctx->cloud_callback_gate, session_id, generation);
        if (delivered) {
            ctx->stats.signaling_messages_tx++;
        } else {
            ctx->stats.signaling_messages_dropped++;
        }
        portEXIT_CRITICAL(&ctx->lock);
        free_event_data(ctx, copy);
        return delivered;
    }
    size_t replace_bytes = 0U;
    if (ctx->outbound_count == SIGNAL_QUEUE_DEPTH) {
        evicted = ctx->outbound[ctx->outbound_head];
        replace_bytes = evicted != NULL ? evicted->budget_bytes : 0U;
    }
    if (!webrtc_signaling_budget_try_replace(
            &ctx->signaling_budget, replace_bytes, resident_bytes)) {
        ctx->stats.signaling_messages_dropped++;
        portEXIT_CRITICAL(&ctx->lock);
        free_event_data(ctx, copy);
        return false;
    }
    copy->budget_bytes = resident_bytes;
    if (ctx->outbound_count == SIGNAL_QUEUE_DEPTH) {
        ctx->outbound[ctx->outbound_head] = NULL;
        ctx->outbound_head = (ctx->outbound_head + 1) % SIGNAL_QUEUE_DEPTH;
        ctx->outbound_count--;
        ctx->stats.signaling_messages_dropped++;
        if (evicted != NULL) {
            /* try_replace already consumed this charge. */
            evicted->budget_bytes = 0U;
        }
    }
    uint8_t tail = (ctx->outbound_head + ctx->outbound_count) % SIGNAL_QUEUE_DEPTH;
    copy->session_id = ctx->active_peer_session;
    copy->generation = ctx->active_peer_generation;
    ctx->outbound[tail] = copy;
    ctx->outbound_count++;
    ctx->stats.signaling_messages_tx++;
    portEXIT_CRITICAL(&ctx->lock);
    free_event_data(ctx, evicted);
    return true;
}

static bool outbound_pop(transport_ctx_t *ctx, webrtc_signal_event_t **event)
{
    bool found = false;
    portENTER_CRITICAL(&ctx->lock);
    if (ctx->outbound_count) {
        *event = ctx->outbound[ctx->outbound_head];
        ctx->outbound[ctx->outbound_head] = NULL;
        ctx->outbound_head = (ctx->outbound_head + 1) % SIGNAL_QUEUE_DEPTH;
        ctx->outbound_count--;
        found = true;
    }
    portEXIT_CRITICAL(&ctx->lock);
    return found;
}

void flush_outbound(transport_ctx_t *ctx)
{
    webrtc_signal_event_t *event = NULL;
    while (outbound_pop(ctx, &event)) {
        free_event_data(ctx, event);
        event = NULL;
    }
}

void flush_inbound(transport_ctx_t *ctx)
{
    if (!ctx->inbound_queue) {
        return;
    }
    inbound_signal_t *signal = NULL;
    while (xQueueReceive(ctx->inbound_queue, &signal, 0) == pdTRUE) {
        free_inbound_signal(ctx, signal);
        signal = NULL;
    }
}

static esp_err_t session_create_with_role(
    webrtc_transport_session_role_t role, uint32_t access_epoch, bool sender_playout_delay,
    webrtc_transport_cloud_signal_cb_t cloud_signal_cb,
    webrtc_transport_cloud_lifecycle_cb_t cloud_lifecycle_cb,
    void *cloud_signal_ctx, owned_ice_server_list_t *ice_servers,
    uint32_t *session_id, uint32_t *generation)
{
    if (!session_id || !generation ||
        (role != WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING &&
         role != WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLED &&
         role != WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED) ||
        (role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED &&
         !cloud_signal_cb)) {
        return ESP_ERR_INVALID_ARG;
    }
    transport_ctx_t *ctx = webrtc_transport_context();
    uint32_t id = esp_random();
    if (!id) {
        id = 1;
    }
    owned_ice_server_list_t *old_ice_servers = NULL;
    for (;;) {
        portENTER_CRITICAL(&ctx->lock);
        if (!ctx->started) {
            portEXIT_CRITICAL(&ctx->lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (!ctx->cloud_callback_gate.active && !ctx->cloud_close_pending) {
            break;
        }
        portEXIT_CRITICAL(&ctx->lock);
        /* The callback contract requires a nonblocking queue/copy sink.  Do
         * not replace its ctx while any transport callback is inside it. */
        vTaskDelay(1);
    }
    old_ice_servers = ctx->session_ice_servers;
    ctx->session_ice_servers = ice_servers;
    ctx->session_id = id;
    ctx->session_active = true;
    ctx->session_role = role;
    ctx->local_access_epoch = access_epoch;
    ctx->cloud_signal_cb = role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED ?
                               cloud_signal_cb : NULL;
    ctx->cloud_lifecycle_cb =
        role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED ?
            cloud_lifecycle_cb : NULL;
    ctx->cloud_signal_ctx = role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED ?
                                cloud_signal_ctx : NULL;
    clear_cloud_invalidation_locked(ctx);
    ctx->session_playout_delay_requested = sender_playout_delay;
    ctx->session_last_poll_us = esp_timer_get_time();
    ctx->stats.session_id = id;
    ctx->stats.browser_session_active = true;
    ctx->stats.rtp_playout_delay_requested = sender_playout_delay;
    ctx->stats.rtp_playout_delay_offered = false;
    ctx->stats.rtp_playout_delay_negotiated = false;
    ctx->stats.rtp_playout_delay_extension_id = 0;
    next_generation_locked(ctx);
    ctx->session_playout_delay_session_id = id;
    ctx->rebuild_requested = true;
    ctx->reconnect_not_before_us = 0;
    ctx->waiting_for_sync = true;
    ctx->stats.waiting_for_sync_frame = true;
    *session_id = id;
    *generation = ctx->generation;
    portEXIT_CRITICAL(&ctx->lock);
    owned_ice_server_list_release(ctx, old_ice_servers);
    notify_peer_task(ctx);
    return ESP_OK;
}

esp_err_t webrtc_transport_session_create(uint32_t access_epoch, bool sender_playout_delay,
                                          uint32_t *session_id,
                                          uint32_t *generation)
{
    if (!jetkvm_access_lock_epoch(access_epoch)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = session_create_with_role(
        WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING, access_epoch,
        sender_playout_delay, NULL, NULL, NULL, NULL,
        session_id, generation);
    jetkvm_access_unlock();
    return err;
}

esp_err_t webrtc_transport_websocket_session_create(uint32_t access_epoch, uint32_t *session_id,
                                                    uint32_t *generation)
{
    if (!jetkvm_access_lock_epoch(access_epoch)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = session_create_with_role(
        WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLED, access_epoch,
        true, NULL, NULL, NULL, NULL, session_id, generation);
    jetkvm_access_unlock();
    return err;
}

esp_err_t webrtc_transport_cloud_session_create(
    const webrtc_transport_cloud_session_cfg_t *cfg,
    uint32_t *session_id, uint32_t *generation)
{
    if (!cfg || !cfg->on_signal) {
        return ESP_ERR_INVALID_ARG;
    }
    owned_ice_server_list_t *ice_servers = NULL;
    esp_err_t err = owned_ice_server_list_create(
        cfg->ice_servers, cfg->ice_server_count, &ice_servers);
    if (err != ESP_OK) {
        return err;
    }
    err = session_create_with_role(
        WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED, 0,
        cfg->sender_playout_delay, cfg->on_signal, cfg->on_lifecycle,
        cfg->ctx, ice_servers,
        session_id, generation);
    if (err != ESP_OK) {
        owned_ice_server_list_release(webrtc_transport_context(), ice_servers);
    }
    return err;
}

static esp_err_t session_poll_with_role(webrtc_transport_session_role_t role,
                                        uint32_t session_id,
                                        webrtc_signal_event_t *event,
                                        uint32_t *current_generation)
{
    if (!event || !current_generation) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(event, 0, sizeof(*event));
    transport_ctx_t *ctx = webrtc_transport_context();
    for (;;) {
        webrtc_signal_event_t *next = NULL;
        bool have_event = false;
        bool matched = false;
        portENTER_CRITICAL(&ctx->lock);
        bool valid = ctx->started && ctx->session_active &&
                     ctx->session_role == role &&
                     session_id == ctx->session_id;
        if (valid) {
            ctx->session_last_poll_us = esp_timer_get_time();
            *current_generation = ctx->generation;
            if (ctx->outbound_count) {
                next = ctx->outbound[ctx->outbound_head];
                ctx->outbound[ctx->outbound_head] = NULL;
                ctx->outbound_head =
                    (ctx->outbound_head + 1) % SIGNAL_QUEUE_DEPTH;
                ctx->outbound_count--;
                have_event = next != NULL;
                matched = next != NULL && next->session_id == session_id &&
                          next->generation == ctx->generation;
                if (!matched) {
                    ctx->stats.signaling_messages_dropped++;
                }
            }
        }
        portEXIT_CRITICAL(&ctx->lock);
        if (!valid) {
            return ESP_ERR_NOT_FOUND;
        }
        if (!have_event) {
            return ESP_OK;
        }
        if (matched) {
            *event = *next;
            return ESP_OK;
        }
        free_event_data(ctx, next);
    }
}

esp_err_t webrtc_transport_session_poll(uint32_t session_id,
                                        webrtc_signal_event_t *event,
                                        uint32_t *current_generation)
{
    return session_poll_with_role(WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLING,
                                  session_id, event, current_generation);
}

esp_err_t webrtc_transport_websocket_session_poll(uint32_t session_id,
                                                  webrtc_signal_event_t *event,
                                                  uint32_t *current_generation)
{
    return session_poll_with_role(WEBRTC_TRANSPORT_SESSION_LOCAL_CONTROLLED,
                                  session_id, event, current_generation);
}

bool webrtc_transport_session_superseded(uint32_t session_id)
{
    transport_ctx_t *ctx = webrtc_transport_context();
    portENTER_CRITICAL(&ctx->lock);
    bool superseded = ctx->started && session_id != 0 && ctx->session_id != session_id;
    portEXIT_CRITICAL(&ctx->lock);
    return superseded;
}

static bool close_session_matching(transport_ctx_t *ctx, uint32_t session_id,
                                   uint32_t generation, bool check_generation)
{
    bool changed = false;
    portENTER_CRITICAL(&ctx->lock);
    if (ctx->started && ctx->session_active && session_id == ctx->session_id &&
        ctx->session_role != WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED &&
        (!check_generation || generation == ctx->generation)) {
        ctx->session_active = false;
        ctx->stats.browser_session_active = false;
        next_generation_locked(ctx);
        ctx->rebuild_requested = true;
        changed = true;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (changed) {
        notify_peer_task(ctx);
    }
    return changed;
}

esp_err_t webrtc_transport_session_submit(uint32_t session_id, uint32_t generation,
                                          webrtc_signal_type_t type,
                                          const char *data, size_t size)
{
    transport_ctx_t *ctx = webrtc_transport_context();
    if (type == WEBRTC_SIGNAL_BYE) {
        if (!close_session_matching(ctx, session_id, generation, true)) {
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }
    if ((type != WEBRTC_SIGNAL_SDP && type != WEBRTC_SIGNAL_CANDIDATE) ||
        !data || !size || size > WEBRTC_SIGNAL_PAYLOAD_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&ctx->lock);
    bool valid = ctx->started && ctx->session_active &&
                 session_id == ctx->session_id &&
                 generation == ctx->generation &&
                 ctx->inbound_queue != NULL;
    if (valid) {
        ctx->submitters++;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (!valid) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t resident_bytes = sizeof(inbound_signal_t) + size + 1U;
    inbound_signal_t *signal =
        jetkvm_memory_policy_control_alloc(resident_bytes);
    if (signal == NULL) {
        portENTER_CRITICAL(&ctx->lock);
        ctx->submitters--;
        ctx->stats.signaling_messages_dropped++;
        portEXIT_CRITICAL(&ctx->lock);
        return ESP_ERR_NO_MEM;
    }
    memset(signal, 0, sizeof(*signal));
    signal->data = (char *)signal->payload;
    memcpy(signal->data, data, size);
    signal->data[size] = '\0';
    signal->size = size;
    signal->type = type;
    signal->session_id = session_id;
    signal->generation = generation;

    QueueHandle_t inbound_queue = NULL;
    portENTER_CRITICAL(&ctx->lock);
    valid = ctx->started && ctx->session_active &&
            session_id == ctx->session_id &&
            generation == ctx->generation &&
            ctx->inbound_queue != NULL;
    bool budget_claimed = valid && webrtc_signaling_budget_claim(
        &ctx->signaling_budget, resident_bytes);
    if (budget_claimed) {
        signal->budget_bytes = resident_bytes;
        inbound_queue = ctx->inbound_queue;
    } else {
        ctx->stats.signaling_messages_dropped++;
    }
    portEXIT_CRITICAL(&ctx->lock);
    if (!budget_claimed) {
        jetkvm_memory_policy_control_free(signal);
        portENTER_CRITICAL(&ctx->lock);
        ctx->submitters--;
        portEXIT_CRITICAL(&ctx->lock);
        return valid ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(inbound_queue, &signal, 0) != pdTRUE) {
        free_inbound_signal(ctx, signal);
        portENTER_CRITICAL(&ctx->lock);
        ctx->submitters--;
        ctx->stats.signaling_messages_dropped++;
        portEXIT_CRITICAL(&ctx->lock);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&ctx->lock);
    ctx->submitters--;
    ctx->stats.signaling_messages_rx++;
    ctx->session_last_poll_us = esp_timer_get_time();
    portEXIT_CRITICAL(&ctx->lock);
    notify_peer_task(ctx);
    return ESP_OK;
}

void webrtc_transport_session_close(uint32_t session_id)
{
    (void)close_session_matching(webrtc_transport_context(), session_id, 0, false);
}

static bool cloud_session_matches(uint32_t session_id, uint32_t generation)
{
    transport_ctx_t *ctx = webrtc_transport_context();
    portENTER_CRITICAL(&ctx->lock);
    bool matches = ctx->started && ctx->session_active &&
                   ctx->session_role ==
                       WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED &&
                   ctx->session_id == session_id &&
                   ctx->generation == generation;
    portEXIT_CRITICAL(&ctx->lock);
    return matches;
}

static void wait_cloud_callback_idle(void)
{
    transport_ctx_t *ctx = webrtc_transport_context();
    for (;;) {
        portENTER_CRITICAL(&ctx->lock);
        bool active = ctx->cloud_callback_gate.active;
        portEXIT_CRITICAL(&ctx->lock);
        if (!active) {
            return;
        }
        vTaskDelay(1);
    }
}

esp_err_t webrtc_transport_cloud_session_submit_offer(
    uint32_t session_id, uint32_t generation,
    const char *sdp, size_t size)
{
    if (!cloud_session_matches(session_id, generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    return webrtc_transport_session_submit(
        session_id, generation, WEBRTC_SIGNAL_SDP, sdp, size);
}

esp_err_t webrtc_transport_cloud_session_submit_candidate(
    uint32_t session_id, uint32_t generation,
    const char *candidate, size_t size)
{
    if (!cloud_session_matches(session_id, generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    return webrtc_transport_session_submit(
        session_id, generation, WEBRTC_SIGNAL_CANDIDATE, candidate, size);
}

esp_err_t webrtc_transport_cloud_session_close(
    uint32_t session_id, uint32_t generation)
{
    transport_ctx_t *ctx = webrtc_transport_context();
    webrtc_transport_cloud_lifecycle_cb_t lifecycle_cb = NULL;
    void *lifecycle_ctx = NULL;
    owned_ice_server_list_t *session_ice_servers = NULL;
    portENTER_CRITICAL(&ctx->lock);
    bool matches = ctx->started && ctx->session_active &&
        !ctx->cloud_close_pending &&
        ctx->session_role == WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED &&
        ctx->session_id == session_id && ctx->generation == generation;
    if (!matches) {
        portEXIT_CRITICAL(&ctx->lock);
        return ESP_ERR_INVALID_STATE;
    }
    lifecycle_cb = ctx->cloud_lifecycle_cb;
    lifecycle_ctx = ctx->cloud_signal_ctx;
    session_ice_servers = ctx->session_ice_servers;
    ctx->session_ice_servers = NULL;
    ctx->cloud_close_pending = true;
    ctx->session_active = false;
    ctx->cloud_signal_cb = NULL;
    ctx->cloud_lifecycle_cb = NULL;
    ctx->cloud_signal_ctx = NULL;
    clear_cloud_invalidation_locked(ctx);
    ctx->stats.browser_session_active = false;
    next_generation_locked(ctx);
    ctx->rebuild_requested = true;
    portEXIT_CRITICAL(&ctx->lock);

    owned_ice_server_list_release(ctx, session_ice_servers);
    notify_peer_task(ctx);
    wait_cloud_callback_idle();

    bool dispatch_closed = false;
    if (lifecycle_cb) {
        portENTER_CRITICAL(&ctx->lock);
        dispatch_closed = webrtc_cloud_callback_gate_try_begin(
            &ctx->cloud_callback_gate, session_id, generation);
        portEXIT_CRITICAL(&ctx->lock);
        if (dispatch_closed) {
            webrtc_transport_cloud_lifecycle_event_t event = {
                .type = WEBRTC_TRANSPORT_CLOUD_LIFECYCLE_SESSION_CLOSED,
                .session_id = session_id,
                .generation = generation,
            };
            lifecycle_cb(&event, lifecycle_ctx);
        }
    }
    portENTER_CRITICAL(&ctx->lock);
    if (dispatch_closed) {
        webrtc_cloud_callback_gate_end(
            &ctx->cloud_callback_gate, session_id, generation);
    }
    ctx->cloud_close_pending = false;
    portEXIT_CRITICAL(&ctx->lock);
    return ESP_OK;
}

void webrtc_transport_signal_event_release(webrtc_signal_event_t *event)
{
    if (event) {
        free_event_data(webrtc_transport_context(), event);
    }
}

void webrtc_transport_signal_record_drop(void)
{
    record_signaling_drop(webrtc_transport_context());
}

void webrtc_transport_revoke_local(void)
{
    transport_ctx_t *ctx = webrtc_transport_context();
    bool changed = false;
    uint32_t hid_generation = 0;
    portENTER_CRITICAL(&ctx->lock);
    if (ctx->session_active && ctx->session_role != WEBRTC_TRANSPORT_SESSION_CLOUD_CONTROLLED &&
        !jetkvm_access_epoch_valid(ctx->local_access_epoch)) {
        hid_generation = ctx->active_peer_generation;
        ctx->session_active = false;
        ctx->stats.browser_session_active = false;
        next_generation_locked(ctx);
        ctx->rebuild_requested = true;
        changed = true;
    }
    portEXIT_CRITICAL(&ctx->lock);
    hid_input_invalidate_generation(hid_generation, "local authority revoked");
    jetkvm_virtual_media_http_revoke_local();
    if (changed) notify_peer_task(ctx);
}

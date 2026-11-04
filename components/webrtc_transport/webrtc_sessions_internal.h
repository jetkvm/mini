/* Component-private signaling/session boundary. */
#pragma once

#include "webrtc_transport_ctx_internal.h"

transport_ctx_t *webrtc_transport_context(void);
uint32_t next_generation_locked(transport_ctx_t *ctx);
void clear_cloud_invalidation_locked(transport_ctx_t *ctx);
void notify_peer_task(transport_ctx_t *ctx);

esp_err_t owned_ice_server_list_create(
    const webrtc_transport_ice_server_t *servers, size_t server_count,
    owned_ice_server_list_t **result);
void owned_ice_server_list_retain_locked(owned_ice_server_list_t *list);
void owned_ice_server_list_release(
    transport_ctx_t *ctx, owned_ice_server_list_t *list);
void record_signaling_drop(transport_ctx_t *ctx);
void free_inbound_signal(transport_ctx_t *ctx, inbound_signal_t *signal);
bool outbound_push(transport_ctx_t *ctx, webrtc_signal_type_t type,
                   const uint8_t *data, size_t size);
void flush_outbound(transport_ctx_t *ctx);
void flush_inbound(transport_ctx_t *ctx);

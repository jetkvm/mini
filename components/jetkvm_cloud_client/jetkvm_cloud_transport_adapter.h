#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "jetkvm_cloud_signal.h"
#include "webrtc_transport.h"

#if WEBRTC_TRANSPORT_CLOUD_SESSION_API_VERSION != 2U
#error "jetkvm_cloud_client requires controlled cloud session API v2"
#endif

typedef struct {
    uint32_t session_id;
    uint32_t generation;
} jetkvm_cloud_transport_binding_t;

esp_err_t jetkvm_cloud_transport_create(
    const jetkvm_cloud_signal_offer_t *offer,
    webrtc_transport_cloud_signal_cb_t on_signal,
    webrtc_transport_cloud_lifecycle_cb_t on_lifecycle,
    void *ctx, jetkvm_cloud_transport_binding_t *binding);

esp_err_t jetkvm_cloud_transport_submit_offer(
    const jetkvm_cloud_transport_binding_t *binding,
    const char *sdp, size_t size);

esp_err_t jetkvm_cloud_transport_submit_candidate(
    const jetkvm_cloud_transport_binding_t *binding,
    const char *candidate, size_t size);

esp_err_t jetkvm_cloud_transport_close(
    const jetkvm_cloud_transport_binding_t *binding);

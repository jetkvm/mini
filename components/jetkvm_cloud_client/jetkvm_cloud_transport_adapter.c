#include "jetkvm_cloud_transport_adapter.h"

#include <string.h>

esp_err_t jetkvm_cloud_transport_create(
    const jetkvm_cloud_signal_offer_t *offer,
    webrtc_transport_cloud_signal_cb_t on_signal,
    webrtc_transport_cloud_lifecycle_cb_t on_lifecycle,
    void *ctx, jetkvm_cloud_transport_binding_t *binding)
{
    if (!offer || !on_signal || !binding ||
        offer->ice_server_count > WEBRTC_TRANSPORT_MAX_ICE_SERVERS) {
        return ESP_ERR_INVALID_ARG;
    }

    webrtc_transport_ice_server_t
        ice_servers[WEBRTC_TRANSPORT_MAX_ICE_SERVERS];
    memset(ice_servers, 0, sizeof(ice_servers));
    for (size_t i = 0; i < offer->ice_server_count; ++i) {
        ice_servers[i].url = offer->ice_servers[i].data;
    }

    const webrtc_transport_cloud_session_cfg_t config = {
        .on_signal = on_signal,
        .on_lifecycle = on_lifecycle,
        .ctx = ctx,
        .sender_playout_delay = true,
        .ice_servers = offer->ice_server_count ? ice_servers : NULL,
        .ice_server_count = offer->ice_server_count,
    };
    return webrtc_transport_cloud_session_create(
        &config, &binding->session_id, &binding->generation);
}

esp_err_t jetkvm_cloud_transport_submit_offer(
    const jetkvm_cloud_transport_binding_t *binding,
    const char *sdp, size_t size)
{
    if (!binding) {
        return ESP_ERR_INVALID_ARG;
    }
    return webrtc_transport_cloud_session_submit_offer(
        binding->session_id, binding->generation, sdp, size);
}

esp_err_t jetkvm_cloud_transport_submit_candidate(
    const jetkvm_cloud_transport_binding_t *binding,
    const char *candidate, size_t size)
{
    if (!binding) {
        return ESP_ERR_INVALID_ARG;
    }
    return webrtc_transport_cloud_session_submit_candidate(
        binding->session_id, binding->generation, candidate, size);
}

esp_err_t jetkvm_cloud_transport_close(
    const jetkvm_cloud_transport_binding_t *binding)
{
    if (!binding || binding->session_id == 0 || binding->generation == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return webrtc_transport_cloud_session_close(
        binding->session_id, binding->generation);
}

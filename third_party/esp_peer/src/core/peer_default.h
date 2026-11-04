/* peer default interface.
 * See LICENSE and LICENSES/ for copyright and licensing notices.
 */
#ifndef ESP_PEER_CORE_PEER_DEFAULT_H
#define ESP_PEER_CORE_PEER_DEFAULT_H
#include "peer_types.h"

/* Internal selected-ICE snapshot used by the default implementation. */
typedef struct {
    bool valid;
    uint8_t local_candidate_type;
    uint8_t local_transport;
    uint8_t pair_candidate_type;
    bool uses_tcp_transport;
} esp_peer_ice_summary_t;

const esp_peer_ops_t *esp_peer_get_default_impl(void);
int peer_default_get_paired_addr(esp_peer_handle_t handle, esp_peer_addr_t *addr);
int peer_default_set_peer_prefer_codec(esp_peer_handle_t handle, esp_peer_codec_caps_t caps);
int peer_default_get_ice_summary(esp_peer_handle_t handle, esp_peer_ice_summary_t *out);
#endif

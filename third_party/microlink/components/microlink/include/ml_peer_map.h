#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#ifndef ML_MAX_PEERS
#ifdef CONFIG_ML_MAX_PEERS
#define ML_MAX_PEERS CONFIG_ML_MAX_PEERS
#else
#define ML_MAX_PEERS 64
#endif
#endif
#ifndef ML_MAX_ENDPOINTS
#define ML_MAX_ENDPOINTS 8
#endif

typedef struct {
    enum {
        ML_PEER_ADD,
        ML_PEER_REMOVE,
        ML_PEER_UPDATE_ENDPOINT,
        ML_PEER_SYNC,
        ML_PEER_MAP_COMPLETE,
    } action;
    unsigned generation;
    uint64_t node_id;
    uint32_t vpn_ip;
    uint8_t public_key[32], disco_key[32];
    char hostname[64];
    uint16_t derp_region;
    struct {
        uint32_t ip;
        uint16_t port;
        bool is_ipv6;
    } endpoints[ML_MAX_ENDPOINTS];
    int endpoint_count;
    bool has_key, has_disco_key, has_endpoints, has_derp;
    /* Node.Online and Node.LastSeen (epoch seconds), or a change of them. */
    bool has_online, online, has_last_seen;
    int64_t last_seen;
} ml_peer_update_t;

typedef bool (*ml_peer_map_emit_t)(const ml_peer_update_t *update, void *ctx);
bool ml_node_id_parse(const cJSON *json, uint64_t *id);
/* initial means the first map on a new stream, including an empty tailnet.
 * Its peer list (Peers, else PeersChanged) is the complete set: SYNC starts
 * it and MAP_COMPLETE ends it, so that the peers that are not in it are
 * removed. */
bool ml_peer_map_apply(const cJSON *map, bool initial, ml_peer_map_emit_t emit, void *ctx);

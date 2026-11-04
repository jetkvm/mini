#include "ml_peer_map.h"
#include "ml_timestamp.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* A positive tailcfg.NodeID in decimal. */
static bool decimal_node_id(const char *text, uint64_t *id)
{
    if (!text || !*text) return false;
    uint64_t value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        unsigned digit = (unsigned)(*text - '0');
        if (value > (UINT64_C(9223372036854775807) - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *id = value;
    return value != 0;
}

bool ml_node_id_parse(const cJSON *json, uint64_t *id)
{
    if (!cJSON_IsNumber(json) || !isfinite(json->valuedouble) ||
        json->valuedouble < 1 ||
        floor(json->valuedouble) != json->valuedouble) return false;
    /* The private cJSON parser retains these tokens before double rounding. */
    if (json->valuedouble > 9007199254740991.0) return decimal_node_id(json->valuestring, id);
    *id = (uint64_t)json->valuedouble;
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool key(const cJSON *json, const char *prefix, uint8_t out[32], bool nonzero_required)
{
    if (!cJSON_IsString(json)) return false;
    const char *text = json->valuestring;
    size_t length = strlen(prefix);
    if (!strncmp(text, prefix, length)) text += length;
    if (strlen(text) != 64) return false;
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 32; ++i) {
        int hi = hex_digit(text[i * 2]), lo = hex_digit(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
        nonzero |= out[i];
    }
    return !nonzero_required || nonzero != 0;
}

static bool ipv4(const char *text, uint32_t *ip, uint16_t *port)
{
    unsigned a, b, c, d, number = 0;
    int end = 0;
    if (!text || sscanf(text, "%u.%u.%u.%u%n", &a, &b, &c, &d, &end) != 4 ||
        a > 255 || b > 255 || c > 255 || d > 255) return false;
    if (port) {
        int size = 0;
        if (text[end] != ':' || sscanf(text + end + 1, "%u%n", &number, &size) != 1 ||
            text[end + 1 + size] || !number || number > 65535) return false;
        *port = (uint16_t)number;
    } else if (text[end] && strcmp(text + end, "/32")) return false;
    *ip = (a << 24) | (b << 16) | (c << 8) | d;
    return *ip != 0;
}

static bool fields(const cJSON *node, ml_peer_update_t *update, bool patch)
{
    if (!cJSON_IsObject(node) || !ml_node_id_parse(cJSON_GetObjectItemCaseSensitive(node,
            patch ? "NodeID" : "ID"), &update->node_id)) return false;
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(node, "Key");
    if (value && !key(value, "nodekey:", update->public_key, true)) return false;
    update->has_key = value != NULL;
    if (!patch && !update->has_key) return false;
    value = cJSON_GetObjectItemCaseSensitive(node, "DiscoKey");
    if (value && !key(value, "discokey:", update->disco_key, false)) return false;
    update->has_disco_key = value != NULL;
    value = cJSON_GetObjectItemCaseSensitive(node, "Name");
    if (cJSON_IsString(value)) {
        snprintf(update->hostname, sizeof(update->hostname), "%s", value->valuestring);
        size_t length = strlen(update->hostname);
        if (length && update->hostname[length - 1] == '.') update->hostname[length - 1] = 0;
    }
    value = cJSON_GetObjectItemCaseSensitive(node, "Addresses");
    const cJSON *address;
    cJSON_ArrayForEach(address, value) {
        if (cJSON_IsString(address) && ipv4(address->valuestring, &update->vpn_ip, NULL)) break;
    }
    if (!patch && !update->vpn_ip) return false;
    value = cJSON_GetObjectItemCaseSensitive(node, "Endpoints");
    if (value && !cJSON_IsArray(value)) return false;
    update->has_endpoints = value != NULL;
    const cJSON *endpoint;
    cJSON_ArrayForEach(endpoint, value) {
        if (update->endpoint_count >= ML_MAX_ENDPOINTS) break;
        unsigned index = update->endpoint_count;
        if (cJSON_IsString(endpoint) && ipv4(endpoint->valuestring,
                &update->endpoints[index].ip, &update->endpoints[index].port))
            ++update->endpoint_count;
    }
    value = cJSON_GetObjectItemCaseSensitive(node, patch ? "DERPRegion" : "HomeDERP");
    if (value) {
        if (!cJSON_IsNumber(value) || value->valuedouble < 0 || value->valuedouble > 65535 ||
            floor(value->valuedouble) != value->valuedouble) return false;
        update->derp_region = (uint16_t)value->valueint;
        update->has_derp = true;
    } else {
        value = cJSON_GetObjectItemCaseSensitive(node, "DERP");
        if (cJSON_IsString(value)) {
            unsigned region;
            int end = 0;
            if (sscanf(value->valuestring, "127.3.3.40:%u%n", &region, &end) != 1 ||
                value->valuestring[end] || region > 65535) return false;
            update->derp_region = (uint16_t)region;
            update->has_derp = true;
        }
    }
    /* Only the order of the peers for the WireGuard slots reads the
     * presence, so a value that does not parse leaves it unknown. */
    value = cJSON_GetObjectItemCaseSensitive(node, "Online");
    if (cJSON_IsBool(value)) {
        update->has_online = true;
        update->online = cJSON_IsTrue(value);
    }
    value = cJSON_GetObjectItemCaseSensitive(node, "LastSeen");
    update->has_last_seen = cJSON_IsString(value) &&
        ml_timestamp_parse(value->valuestring, &update->last_seen);
    return true;
}

/* An expired peer, a jailed peer, which may not open a connection to this
 * node, and an unsigned peer under tailnet lock (UnsignedPeerAPIOnly), which
 * may reach only a peer API that the Mini does not serve, get no WireGuard
 * entry: nothing from them reaches the Mini. */
static bool shut_out(const cJSON *peer)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(peer, "Expired")) ||
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(peer, "IsJailed")) ||
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(peer, "UnsignedPeerAPIOnly"));
}

bool ml_peer_map_apply(const cJSON *map, bool initial, ml_peer_map_emit_t emit, void *ctx)
{
    if (!cJSON_IsObject(map) || !emit) return false;
    const cJSON *full = cJSON_GetObjectItemCaseSensitive(map, "Peers");
    if (full && !cJSON_IsArray(full)) return false;
    bool complete = full && cJSON_GetArraySize(full) > 0;
    bool snapshot = initial || complete;
    /* The first map of a response changes no earlier map, so the control
     * server may send the complete peer list as PeersChanged instead of
     * Peers (tailcfg.MapResponse). */
    const cJSON *peers = complete ? full : cJSON_GetObjectItemCaseSensitive(map, "PeersChanged");
    if (peers && !cJSON_IsArray(peers)) return false;
    ml_peer_update_t update = {0};
    const cJSON *peer;
    if (snapshot) {
        update.action = ML_PEER_SYNC;
        if (!emit(&update, ctx)) return false;
    }
    cJSON_ArrayForEach(peer, peers) {
        memset(&update, 0, sizeof(update));
        update.action = ML_PEER_ADD;
        if (!fields(peer, &update, false)) return false;
        if (shut_out(peer)) update.action = ML_PEER_REMOVE;
        if (!emit(&update, ctx)) return false;
    }
    if (!snapshot) {
        const cJSON *removed = cJSON_GetObjectItemCaseSensitive(map, "PeersRemoved");
        if (removed && !cJSON_IsArray(removed)) return false;
        cJSON_ArrayForEach(peer, removed) {
            memset(&update, 0, sizeof(update));
            update.action = ML_PEER_REMOVE;
            if (!ml_node_id_parse(peer, &update.node_id) || !emit(&update, ctx)) return false;
        }
    }
    const cJSON *patches = cJSON_GetObjectItemCaseSensitive(map, "PeersChangedPatch");
    if (patches && !cJSON_IsArray(patches)) return false;
    cJSON_ArrayForEach(peer, patches) {
        memset(&update, 0, sizeof(update));
        update.action = ML_PEER_UPDATE_ENDPOINT;
        if (!fields(peer, &update, true) || !emit(&update, ctx)) return false;
    }
    /* tailcfg.MapResponse.OnlineChange: node IDs in decimal, each with its
     * Node.Online. */
    const cJSON *online = cJSON_GetObjectItemCaseSensitive(map, "OnlineChange");
    if (!cJSON_IsObject(online)) online = NULL;
    cJSON_ArrayForEach(peer, online) {
        memset(&update, 0, sizeof(update));
        update.action = ML_PEER_UPDATE_ENDPOINT;
        if (!cJSON_IsBool(peer) || !decimal_node_id(peer->string, &update.node_id)) continue;
        update.has_online = true;
        update.online = cJSON_IsTrue(peer);
        if (!emit(&update, ctx)) return false;
    }
    if (snapshot) {
        memset(&update, 0, sizeof(update));
        update.action = ML_PEER_MAP_COMPLETE;
        if (!emit(&update, ctx)) return false;
    }
    return true;
}

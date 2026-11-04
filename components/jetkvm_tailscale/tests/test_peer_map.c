#include "ml_peer_map.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define KEY_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define KEY_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define KEY_C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
static ml_peer_update_t messages[256];
static unsigned count, reject_after;

static bool emit(const ml_peer_update_t *message, void *ctx)
{
    assert(ctx == messages);
    if (count == reject_after) return false;
    assert(count < sizeof(messages) / sizeof(messages[0]));
    messages[count++] = *message;
    return true;
}

static bool apply(const char *json, bool initial, unsigned limit)
{
    cJSON *map = cJSON_Parse(json);
    assert(map);
    count = 0;
    reject_after = limit;
    bool accepted = ml_peer_map_apply(map, initial, emit, messages);
    cJSON_Delete(map);
    return accepted;
}

void test_peer_map(void)
{
    /* Wire shapes follow tailscale/tailcfg.MapResponse and PeerChange.
     * This ID cannot fit cJSON's 32-bit valueint. IPv6 precedes IPv4. */
    const char *snapshot = "{\"Peers\":[{\"ID\":281474976710657,"
        "\"Name\":\"workstation.test.\",\"Key\":\"nodekey:" KEY_A "\","
        "\"DiscoKey\":\"discokey:" KEY_C "\","
        "\"Addresses\":[\"fd7a:115c:a1e0::5/128\",\"100.64.0.5/32\"],"
        "\"HomeDERP\":16,\"Endpoints\":[\"[2001:db8::1]:41641\","
        "\"192.168.1.10:41641\",\"999.1.1.1:1\",\"1.1.1.1:65536\"]}],"
        "\"PeersRemoved\":[281474976710657]}";
    assert(apply(snapshot, true, 70));
    assert(count == 3);
    assert(messages[0].action == ML_PEER_SYNC);
    assert(messages[1].action == ML_PEER_ADD);
    assert(messages[1].node_id == UINT64_C(281474976710657));
    assert(messages[1].vpn_ip == UINT32_C(0x64400005));
    assert(!strcmp(messages[1].hostname, "workstation.test"));
    for (unsigned i = 0; i < 32; ++i) assert(messages[1].public_key[i] == 0xaa);
    assert(messages[1].derp_region == 16 && messages[1].endpoint_count == 1);
    assert(messages[1].endpoints[0].ip == UINT32_C(0xc0a8010a));
    assert(messages[1].endpoints[0].port == 41641);
    assert(messages[2].action == ML_PEER_MAP_COMPLETE);
    assert(apply("{\"Peers\":[{\"ID\":7,\"Key\":\"nodekey:" KEY_A "\","
        "\"Addresses\":[\"100.64.0.5/32\"],\"Expired\":true}]}", true, 70));
    assert(count == 3);
    assert(messages[1].action == ML_PEER_REMOVE && messages[1].node_id == 7);
    assert(apply("{\"Peers\":[{\"ID\":8,\"Key\":\"nodekey:" KEY_B "\","
        "\"Addresses\":[\"100.64.0.8/32\"],\"IsJailed\":true}]}", true, 70));
    assert(count == 3);
    assert(messages[1].action == ML_PEER_REMOVE && messages[1].node_id == 8);
    /* tailcfg.Node.UnsignedPeerAPIOnly: an unsigned node under tailnet lock
     * gets no network access, and the client enforces that. */
    assert(apply("{\"Peers\":[{\"ID\":10,\"Key\":\"nodekey:" KEY_B "\","
        "\"Addresses\":[\"100.64.0.10/32\"],\"UnsignedPeerAPIOnly\":true}]}", true, 70));
    assert(count == 3);
    assert(messages[1].action == ML_PEER_REMOVE && messages[1].node_id == 10);
    assert(apply("{\"PeersChanged\":[{\"ID\":10,\"Key\":\"nodekey:" KEY_B "\","
        "\"Addresses\":[\"100.64.0.10/32\"],\"UnsignedPeerAPIOnly\":true}]}", false, 70));
    assert(count == 1 && messages[0].action == ML_PEER_REMOVE && messages[0].node_id == 10);

    assert(apply("{\"PeersRemoved\":[281474976710657,7]}", false, 70));
    assert(count == 2 && messages[0].action == ML_PEER_REMOVE);
    assert(messages[0].node_id == UINT64_C(281474976710657) && messages[1].node_id == 7);
    assert(apply("{\"PeersRemoved\":[9007199254740992,9007199254740993,9223372036854775807]}", false, 70));
    assert(count == 3 && messages[0].node_id == UINT64_C(9007199254740992));
    assert(messages[1].node_id == UINT64_C(9007199254740993));
    assert(messages[2].node_id == UINT64_C(9223372036854775807));
    assert(apply("{\"PeersChangedPatch\":[{\"NodeID\":281474976710657,"
        "\"Key\":\"nodekey:" KEY_B "\",\"DiscoKey\":\"discokey:" KEY_C "\","
        "\"Endpoints\":[],\"DERPRegion\":0}]}", false, 70));
    assert(count == 1 && messages[0].action == ML_PEER_UPDATE_ENDPOINT);
    assert(messages[0].node_id == UINT64_C(281474976710657));
    assert(messages[0].has_key && messages[0].public_key[0] == 0xbb);
    assert(messages[0].has_disco_key && messages[0].disco_key[0] == 0xcc);
    assert(messages[0].has_endpoints && messages[0].endpoint_count == 0);
    assert(messages[0].has_derp && messages[0].derp_region == 0);
    assert(apply("{\"PeersChangedPatch\":[{\"NodeID\":7,\"Online\":false,"
        "\"LastSeen\":\"2026-10-19T21:00:05Z\"}]}", false, 70));
    assert(count == 1 && !messages[0].has_endpoints && !messages[0].has_key);
    assert(messages[0].has_online && !messages[0].online);
    assert(messages[0].has_last_seen && messages[0].last_seen == INT64_C(1792443605));
    /* The presence only orders the peers for the WireGuard slots: a value
     * that does not parse leaves it unknown and refuses nothing. */
    assert(apply("{\"PeersChangedPatch\":[{\"NodeID\":7,\"Online\":1,\"LastSeen\":\"yesterday\"}]}", false, 70));
    assert(count == 1 && !messages[0].has_online && !messages[0].has_last_seen);
    assert(apply("{\"OnlineChange\":{\"7\":true,\"281474976710657\":false,\"x\":true,\"8\":1}}", false, 70));
    assert(count == 2 && messages[0].action == ML_PEER_UPDATE_ENDPOINT);
    assert(messages[0].node_id == 7 && messages[0].has_online && messages[0].online);
    assert(messages[1].node_id == UINT64_C(281474976710657) && !messages[1].online);
    assert(!messages[0].has_key && !messages[0].has_endpoints && !messages[0].has_derp);
    assert(apply("{\"KeepAlive\":true}", false, 70) && count == 0);
    assert(apply("{}", true, 70));
    assert(count == 2 && messages[0].action == ML_PEER_SYNC);
    assert(messages[1].action == ML_PEER_MAP_COMPLETE);
    /* controlplane.tailscale.com sends the first map of a response with the
     * complete peer list as PeersChanged: the snapshot keeps that peer. */
    const char *changed = "{\"PeersChanged\":[{\"ID\":7,\"Key\":\"nodekey:" KEY_A "\","
        "\"Addresses\":[\"100.64.0.7/32\"]}]}";
    assert(apply(changed, true, 70));
    assert(count == 3 && messages[0].action == ML_PEER_SYNC);
    assert(messages[1].action == ML_PEER_ADD && messages[1].node_id == 7);
    assert(messages[2].action == ML_PEER_MAP_COMPLETE);
    assert(apply(changed, false, 70));
    assert(count == 1 && messages[0].action == ML_PEER_ADD);
    assert(!apply(snapshot, true, 1) && count == 1);
    const char *bad[] = {
        "{\"PeersRemoved\":[1.5]}", "{\"PeersRemoved\":[-1]}",
        "{\"PeersRemoved\":[9223372036854775808]}",
        "{\"PeersRemoved\":[9.1e18]}",
        "{\"PeersRemoved\":[\"nodekey:" KEY_A "\"]}",
        "{\"PeersChangedPatch\":{\"nodekey:" KEY_A "\":{\"Endpoints\":[]}}}",
        "{\"PeersChangedPatch\":[{\"NodeID\":7,\"Key\":\"nodekey:123\"}]}",
        "{\"PeersChangedPatch\":[{\"NodeID\":7,\"Endpoints\":false}]}",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(!apply(bad[i], false, 70));
    /* More live peers than ML_MAX_PEERS: the map is taken whole, with each
     * peer's presence, and the peer manager picks the peers for the slots. */
    const unsigned sizes[] = {ML_MAX_PEERS, ML_MAX_PEERS + 1, 100, 200};
    for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        unsigned live = sizes[s];
        static char map[65536];
        size_t used = (size_t)snprintf(map, sizeof(map), "{\"Peers\":[");
        for (unsigned i = 0; i < live + 3; ++i)
            used += (size_t)snprintf(map + used, sizeof(map) - used,
                "%s{\"ID\":%u,\"Key\":\"nodekey:%064x\",\"Addresses\":[\"100.64.1.%u/32\"],"
                "\"Online\":%s,\"LastSeen\":\"2026-10-19T%02u:00:00Z\"%s}",
                i ? "," : "", i + 1, i + 1, i + 1, i % 2 ? "true" : "false", i % 24,
                i < 3 ? ",\"Expired\":true" : "");
        assert(used + 3 < sizeof(map));
        snprintf(map + used, sizeof(map) - used, "]}");
        assert(apply(map, true, 0xffffffffu));
        assert(count == live + 5 && messages[0].action == ML_PEER_SYNC);
        assert(messages[count - 1].action == ML_PEER_MAP_COMPLETE);
        unsigned adds = 0;
        for (unsigned i = 1; i < count - 1; ++i) {
            const ml_peer_update_t *peer = &messages[i];
            assert(peer->action == (i <= 3 ? ML_PEER_REMOVE : ML_PEER_ADD));
            assert(peer->has_online && peer->online == ((i - 1) % 2 == 1));
            assert(peer->has_last_seen && peer->last_seen == INT64_C(1792368000) + ((i - 1) % 24) * 3600);
            adds += peer->action == ML_PEER_ADD;
        }
        assert(adds == live);
    }
    puts("Tailscale numeric peer removal, key rotation, endpoint patches and full maps: PASS");
}

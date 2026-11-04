/* A tailnet with more live peers than WireGuard slots: MapResponse JSON goes
 * through the production parser and peer manager into the host WireGuard
 * device, which keeps the online, then the most recently seen, peers and the
 * peers with a session. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lwip/init.h"
#include "wireguardif.h"
#include "ml_peer_map.h"
#include "ml_peer_select.h"
#include PEER_ROAMING_TYPES

#define EXT_RAM_BSS_ATTR
#define DISCO_TXID_LEN 12
#define ML_DISCO_TRUST_DURATION_MS 15000
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define pdTRUE 1

typedef struct microlink {
    ml_peer_t peers[ML_MAX_PEERS];
    int peer_count;
    struct netif *wg_netif;
    void *config_httpd;
    int disco_sock4;
    struct { uint32_t priority_peer_ip; } config;
    void (*peer_cb)(struct microlink *, const microlink_peer_info_t *, void *);
    void *peer_cb_data;
    void *peer_update_queue;
    ml_peer_dir_t peer_dir;
    volatile unsigned peers_left_out;
    volatile bool peer_map_valid;
    volatile unsigned peer_map_generation, peer_map_failed_generation;
} microlink_t;
typedef struct { bool via_derp; uint32_t src_ip; uint16_t src_port; } ml_rx_packet_t;

static uint32_t now_ms = 1000;
uint32_t sys_now(void) { return now_ms; }
uint32_t wireguard_sys_now(void) { return sys_now(); }
void wireguard_random_bytes(void *data, size_t n) { memset(data, 0x42, n); }
void wireguard_tai64n_now(uint8_t *data) { memset(data, 0, 12); }
bool wireguard_is_under_load(void) { return false; }
static uint64_t ml_get_time_ms(void) { return sys_now(); }
static bool ml_lwip_lock(void) { return false; }
static void ml_lwip_unlock(bool acquired) { (void)acquired; }
/* Peers whose address ends in 255 are not in the allowlist. */
static bool ml_config_peer_is_allowed(void *config, uint32_t ip) { (void)config; return (ip & 0xff) != 0xff; }
static bool ml_at_socket_is_ready(void) { return true; }
static bool disco_has_udp_path(microlink_t *ml) { (void)ml; return false; }
static void microlink_ip_to_str(uint32_t ip, char *out) { (void)ip; strcpy(out, "100.64.0.1"); }
static void disco_send_call_me_maybe(microlink_t *ml, int idx) { (void)ml; (void)idx; }
static void disco_send_ping_to_peer(microlink_t *ml, int idx, bool force) { (void)ml; (void)idx; (void)force; }
static void disco_build_ping(microlink_t *ml, int idx, uint8_t *data, size_t *len) {
    (void)ml; (void)idx; (void)data; *len = 0;
}
static int disco_udp_sendto(microlink_t *ml, const uint8_t *data, size_t len, uint32_t ip, uint16_t port) {
    (void)ml; (void)data; (void)ip; (void)port; return (int)len;
}
static int mbedtls_base64_encode(unsigned char *out, size_t cap, size_t *n,
                                 const unsigned char *key, size_t len) {
    *n = cap;
    return wireguard_base64_encode(key, len, (char *)out, n) ? 0 : -1;
}

/* The queue from the control task: the batch of each response waits here. */
static ml_peer_batch_t *queue[4];
static unsigned queued, taken;
static int xQueueReceive(void *q, ml_peer_batch_t **batch, int ticks)
{
    (void)q; (void)ticks;
    if (taken == queued) return 0;
    *batch = queue[taken++];
    return pdTRUE;
}
#include PEER_ROAMING_FUNCTIONS
#include PEER_FILL_FUNCTIONS

static microlink_t ml;
static struct netif netif;
/* The updates in the batch of the last response. */
static unsigned batch_size;

static bool emit(const ml_peer_update_t *update, void *ctx)
{
    return ml_peer_batch_add(ctx, update);
}

/* The model of the tailnet: presence 1 online, 0 not given, -1 offline,
 * and the second of the day of LastSeen. */
#define IDS 1200
static bool live[IDS], session[IDS];
static int presence[IDS];
static unsigned seen[IDS];

static size_t node_json(char *out, size_t capacity, unsigned id)
{
    size_t used = (size_t)snprintf(out, capacity,
        "{\"ID\":%u,\"Key\":\"nodekey:%08x5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a19\","
        "\"Addresses\":[\"100.64.%u.%u/32\"],\"HomeDERP\":1",
        id, id, id / 256, id % 256);
    if (presence[id]) used += (size_t)snprintf(out + used, capacity - used, ",\"Online\":%s",
                                               presence[id] > 0 ? "true" : "false");
    used += (size_t)snprintf(out + used, capacity - used,
        ",\"LastSeen\":\"2026-10-19T%02u:%02u:%02uZ\"}", seen[id] / 3600, seen[id] / 60 % 60, seen[id] % 60);
    return used;
}

static void apply(const char *json, bool initial)
{
    cJSON *map = cJSON_Parse(json);
    assert(map);
    if (initial) ++ml.peer_map_generation;
    ml_peer_batch_t *batch = NULL;
    assert(ml_peer_map_apply(map, initial, emit, &batch));
    cJSON_Delete(map);
    /* As the control task sends it: one queue item per response. */
    batch_size = batch ? batch->count : 0;
    if (batch) {
        for (unsigned i = 0; i < batch->count; ++i) batch->updates[i].generation = ml.peer_map_generation;
        assert(queued < sizeof(queue) / sizeof(queue[0]));
        queue[queued++] = batch;
    }
    process_peer_updates(&ml);
    assert(taken == queued);
    taken = queued = 0;
}

static char text[1 << 20];

static void snapshot(void)
{
    size_t used = (size_t)snprintf(text, sizeof(text), "{\"Peers\":[");
    bool first = true;
    for (unsigned id = 1; id < IDS; ++id) {
        if (!live[id]) continue;
        if (!first) text[used++] = ',';
        first = false;
        used += node_json(text + used, sizeof(text) - used, id);
    }
    snprintf(text + used, sizeof(text) - used, "]}");
    apply(text, true);
}

static void changed(unsigned id)
{
    live[id] = true;
    size_t used = (size_t)snprintf(text, sizeof(text), "{\"PeersChanged\":[");
    used += node_json(text + used, sizeof(text) - used, id);
    snprintf(text + used, sizeof(text) - used, "]}");
    apply(text, false);
}

static void removed(unsigned id)
{
    live[id] = session[id] = false;
    snprintf(text, sizeof(text), "{\"PeersRemoved\":[%u]}", id);
    apply(text, false);
}

static void patch_presence(unsigned id, int now, unsigned second)
{
    presence[id] = now;
    seen[id] = second;
    snprintf(text, sizeof(text), "{\"PeersChangedPatch\":[{\"NodeID\":%u,\"Online\":%s,"
        "\"LastSeen\":\"2026-10-19T%02u:%02u:%02uZ\"}]}", id, now > 0 ? "true" : "false",
        second / 3600, second / 60 % 60, second % 60);
    apply(text, false);
}

static int slot(unsigned id)
{
    for (int i = 0; i < ml.peer_count; ++i)
        if (ml.peers[i].active && ml.peers[i].node_id == id) return i;
    return -1;
}

static bool installed(unsigned id) { return slot(id) >= 0; }

/* The WireGuard peer of each node ID, -1 without a slot. */
static void wireguard_peers(int index[IDS])
{
    for (unsigned id = 0; id < IDS; ++id) index[id] = -1;
    for (int i = 0; i < ml.peer_count; ++i)
        if (ml.peers[i].active) {
            assert(ml.peers[i].node_id < IDS && index[ml.peers[i].node_id] < 0);
            index[ml.peers[i].node_id] = ml.peers[i].wg_peer_index;
        }
}

static bool allowed(unsigned id) { return id % 256 != 255; }

/* A peer that a left-out peer would displace with a strictly better rank,
 * by the rules in the model. */
static bool strictly_above(unsigned a, unsigned b)
{
    if (session[a] != session[b]) return session[a];
    int rank_a = presence[a] + 1, rank_b = presence[b] + 1;
    if (rank_a != rank_b) return rank_a > rank_b;
    return presence[a] <= 0 && seen[a] > seen[b];
}

/* The slots match the directory and the WireGuard device, no left-out
 * peer outranks a peer with a slot, and the status counts the left out. */
static unsigned check(void)
{
    struct wireguard_device *device = netif.state;
    static int index[IDS];
    wireguard_peers(index);
    unsigned in = 0, out = 0, valid = 0, active = 0, holders[ML_MAX_PEERS];
    for (unsigned i = 0; i < WIREGUARD_MAX_PEERS; ++i) valid += device->peers[i].valid;
    for (int i = 0; i < ml.peer_count; ++i) active += ml.peers[i].active;
    for (unsigned id = 1; id < IDS; ++id) {
        if (!live[id] || !allowed(id)) {
            assert(index[id] < 0);
            continue;
        }
        if (index[id] < 0) {
            ++out;
            continue;
        }
        assert(in < ML_MAX_PEERS);
        holders[in++] = id;
        ml_peer_t *peer = &ml.peers[slot(id)];
        assert(peer->wg_peer_index >= 0);
        struct wireguard_peer *wireguard = &device->peers[peer->wg_peer_index];
        assert(wireguard->valid && !memcmp(wireguard->public_key, peer->public_key, 32));
    }
    assert(in == active && in == valid);
    assert(in == ML_MAX_PEERS || out == 0);
    assert(ml.peers_left_out == out);
    for (unsigned a = 1; a < IDS; ++a) {
        if (!live[a] || !allowed(a) || index[a] >= 0) continue;
        for (unsigned i = 0; i < in; ++i) assert(!strictly_above(a, holders[i]));
    }
    return in;
}

static void set_session(unsigned id, bool up)
{
    struct wireguard_device *device = netif.state;
    int idx = slot(id);
    assert(idx >= 0);
    struct wireguard_keypair *keypair = &device->peers[ml.peers[idx].wg_peer_index].curr_keypair;
    keypair->valid = up;
    keypair->keypair_millis = now_ms;
    session[id] = up;
}

/* The tailnet of one test: count peers, online every period-th, the others
 * offline and seen later the higher their ID. */
static void tailnet(unsigned count, unsigned period)
{
    memset(live, 0, sizeof(live));
    memset(session, 0, sizeof(session));
    for (unsigned id = 1; id <= count; ++id) {
        live[id] = true;
        presence[id] = id % period == 0 ? 1 : -1;
        seen[id] = id * 60;
    }
}

static unsigned random_state = 12345;
static unsigned random_below(unsigned n)
{
    random_state = random_state * 1103515245u + 12345u;
    return (random_state >> 8) % n;
}

int main(void)
{
    lwip_init();
    wireguardif_disable_socket_bind();
    struct wireguardif_init_data init = {
        .private_key = "c3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3Nzc3M=",
    };
    netif.state = &init;
    assert(wireguardif_init(&netif) == ERR_OK);
    ml.wg_netif = &netif;
    /* The slots stay at 64, in the client and in the WireGuard device. */
    assert(sizeof(ml.peers) / sizeof(ml.peers[0]) == 64 && WIREGUARD_MAX_PEERS == 64);

    /* 65, 100 and 200 live peers: the map is taken, and the slots go to the
     * online peers, then to the offline peers seen most recently. */
    const unsigned sizes[][2] = {{65, 2}, {100, 4}, {200, 5}};
    for (unsigned s = 0; s < 3; ++s) {
        unsigned count = sizes[s][0], period = sizes[s][1];
        tailnet(count, period);
        snapshot();
        assert(ml.peer_map_valid && check() == 64 && ml.peers_left_out == count - 64);
        unsigned online = 0, offline = 0;
        for (unsigned id = 1; id <= count; ++id) {
            if (presence[id] > 0) {
                assert(installed(id));
                ++online;
            }
        }
        for (unsigned id = count; id >= 1 && online + offline < 64; --id)
            if (presence[id] < 0) {
                assert(installed(id));
                ++offline;
            }
        printf("%u live peers: %u online and %u most recently seen in the 64 slots, %u left out\n",
               count, online, offline, ml.peers_left_out);
    }
    printf("directory of 200 peers: %u entries of %zu bytes\n", ml.peer_dir.capacity, sizeof(ml_peer_entry_t));

    /* From here the tailnet of 200: the 40 online and the offline 171 to
     * 199 in the slots. The worst offline peer in a slot is the least
     * recently seen one, 171. */
    assert(installed(172) && installed(171) && !installed(169));

    /* A new online peer (PeersChanged) takes the slot of the worst. */
    presence[201] = 1;
    seen[201] = 1;
    changed(201);
    assert(installed(201) && !installed(171) && check() == 64 && ml.peers_left_out == 137);

    /* A peer that leaves the tailnet (PeersRemoved) frees its slot for the
     * best peer left out: 171 again. */
    removed(200);
    assert(installed(171) && check() == 64 && ml.peers_left_out == 136);
    /* A left-out peer that leaves changes no slot. */
    removed(1);
    assert(installed(171) && check() == 64 && ml.peers_left_out == 135);

    /* A left-out peer that comes online (PeersChangedPatch) takes the slot
     * of the worst offline peer, and an online peer that goes offline and
     * is seen last keeps its slot. */
    patch_presence(2, 1, 1);
    assert(installed(2) && !installed(171) && check() == 64);
    patch_presence(5, -1, 86000);
    assert(installed(5) && installed(172) && check() == 64);
    /* OnlineChange, from servers that send no presence in patches. */
    presence[3] = 1;
    apply("{\"OnlineChange\":{\"3\":true}}", false);
    assert(installed(3) && !installed(172) && check() == 64);
    unsigned worst = 0;

    /* A peer with a WireGuard session keeps its slot when a better peer
     * comes; the next worst gives up its slot instead. */
    for (unsigned id = 1; id <= 201; ++id)
        if (installed(id) && presence[id] < 0 && (!worst || seen[id] < seen[worst])) worst = id;
    set_session(worst, true);
    int pinned_index = ml.peers[slot(worst)].wg_peer_index;
    presence[4] = 1;
    changed(4);
    assert(installed(4) && installed(worst) && check() == 64);
    /* The complete map again (a new stream) keeps the slots and their
     * WireGuard peers, with the session. */
    static int before[IDS], after[IDS];
    wireguard_peers(before);
    snapshot();
    assert(ml.peer_map_valid && check() == 64);
    wireguard_peers(after);
    assert(!memcmp(before, after, sizeof(before)));
    assert(after[worst] == pinned_index && session[worst] && peer_session_up(&ml, slot(worst)));
    /* Once the session ends, the next change gives its slot away. */
    set_session(worst, false);
    presence[6] = 1;
    changed(6);
    assert(installed(6) && !installed(worst) && check() == 64);

    /* A keypair older than REJECT_AFTER_TIME is no session, also while
     * WireGuard keeps it as the previous keypair: its peer gives up its
     * slot to a better one. */
    worst = 0;
    for (unsigned id = 1; id <= 201; ++id)
        if (installed(id) && presence[id] < 0 && (!worst || seen[id] < seen[worst])) worst = id;
    struct wireguard_device *device = netif.state;
    struct wireguard_keypair *previous = &device->peers[ml.peers[slot(worst)].wg_peer_index].prev_keypair;
    previous->valid = true;
    previous->keypair_millis = now_ms;
    assert(peer_session_up(&ml, slot(worst)));
    now_ms += (REJECT_AFTER_TIME + 1) * 1000;
    assert(!peer_session_up(&ml, slot(worst)) && previous->valid);
    presence[7] = 1;
    changed(7);
    assert(installed(7) && !installed(worst) && check() == 64);

    /* A peer that the allowlist does not admit gets no slot and is not
     * counted as left out. */
    presence[255] = 1;
    changed(255);
    assert(!installed(255) && check() == 64);

    /* Churn: random new peers, removals, presence patches, sessions and new
     * streams. One change moves at most one peer in and one out, besides a
     * peer whose session ended since, and a peer that keeps its slot keeps
     * its WireGuard peer. */
    unsigned next_id = 300, ended = 0, moves = 0;
    for (unsigned step = 0; step < 3000; ++step) {
        wireguard_peers(before);
        unsigned id = 1 + random_below(next_id - 1);
        unsigned action = random_below(20);
        unsigned second = random_below(86400);
        if (action == 0) {
            snapshot();
        } else if (action < 5 && next_id < IDS) {
            id = next_id++;
            presence[id] = (int)random_below(3) - 1;
            seen[id] = second;
            changed(id);
        } else if (action < 8 && live[id]) {
            removed(id);
        } else if (action < 18 && live[id]) {
            patch_presence(id, random_below(2) ? 1 : -1, second);
        } else if (live[id] && installed(id)) {
            ended += session[id];
            set_session(id, !session[id]);
            continue;
        } else {
            continue;
        }
        check();
        wireguard_peers(after);
        unsigned in = 0, out = 0;
        for (unsigned peer = 1; peer < IDS; ++peer) {
            in += before[peer] < 0 && after[peer] >= 0;
            out += before[peer] >= 0 && after[peer] < 0;
            if (before[peer] >= 0 && after[peer] >= 0) assert(after[peer] == before[peer]);
        }
        unsigned bound = ended + (action != 0);
        assert(in <= bound && out <= bound);
        moves += in;
        ended = 0;
    }
    unsigned live_count = 0;
    for (unsigned id = 1; id < IDS; ++id) live_count += live[id];
    printf("churn: %u live peers after 3000 steps, %u left out, %u peers moved into a slot\n",
           live_count, ml.peers_left_out, moves);

    /* A tailnet that shrinks below the slots leaves nobody out. */
    for (unsigned id = 1; id < IDS; ++id) live[id] = live[id] && id < 40;
    snapshot();
    assert(check() <= 39 && ml.peers_left_out == 0);

    /* A complete map that does not parse to its end names only the peers
     * before the fault: the peers after it keep their slots. */
    unsigned named = 0;
    while (!installed(++named)) {}
    static int kept[IDS];
    wireguard_peers(kept);
    size_t used = (size_t)snprintf(text, sizeof(text), "{\"Peers\":[");
    used += node_json(text + used, sizeof(text) - used, named);
    snprintf(text + used, sizeof(text) - used, ",{\"ID\":\"not a number\"}]}");
    cJSON *broken = cJSON_Parse(text);
    ml_peer_batch_t *partial = NULL;
    ++ml.peer_map_generation;
    assert(!ml_peer_map_apply(broken, true, emit, &partial) && partial && partial->count == 2);
    cJSON_Delete(broken);
    for (unsigned i = 0; i < partial->count; ++i) partial->updates[i].generation = ml.peer_map_generation;
    queue[queued++] = partial;
    process_peer_updates(&ml);
    taken = queued = 0;
    ml.peer_map_failed_generation = ml.peer_map_generation;
    wireguard_peers(after);
    assert(!memcmp(kept, after, sizeof(kept)));
    snapshot();
    assert(ml.peer_map_valid && check() <= 39 && ml.peers_left_out == 0);

    /* A map as large as the directory is one queue item. */
    tailnet(ML_PEER_DIR_MAX, 1);
    snapshot();
    unsigned allowed_count = 0;
    for (unsigned id = 1; id <= ML_PEER_DIR_MAX; ++id) allowed_count += allowed(id);
    assert(ml.peer_map_valid && ml.peer_dir.count == ML_PEER_DIR_MAX);
    assert(check() == 64 && ml.peers_left_out == allowed_count - 64);
    printf("map of %u peers: 1 queue item of %u updates, %u left out\n",
           ML_PEER_DIR_MAX, batch_size, ml.peers_left_out);
    /* A peer that does not fit the full directory counts once, also when
     * it changes again, and no longer once it leaves. */
    presence[1100] = 1;
    seen[1100] = 1;
    changed(1100);
    changed(1100);
    assert(!installed(1100) && check() == 64 && ml.peers_left_out == allowed_count - 63);
    removed(1100);
    assert(check() == 64 && ml.peers_left_out == allowed_count - 64);
    /* A complete map of new peers replaces the full directory: the peers
     * it no longer names make room for the new ones before they come. */
    for (unsigned id = 1; id < IDS; ++id) {
        live[id] = id > 1100 && id <= 1164;
        presence[id] = 1;
    }
    snapshot();
    assert(ml.peer_map_valid && ml.peer_dir.count == 64 && check() == 64 && ml.peers_left_out == 0);
    for (unsigned id = 1101; id <= 1164; ++id) assert(installed(id));
    ml_peer_dir_free(&ml.peer_dir);
    wireguardif_shutdown(&netif);
    puts("Peer fill: 65, 100 and 200 live peers, PeersChanged, PeersRemoved, presence and churn keep the 64 most useful: PASS");
}

#include "ml_peer_select.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* presence: 1 online, 0 not given, -1 offline; last_seen 0: not given. */
static ml_peer_update_t node(uint64_t id, int presence, int64_t last_seen)
{
    ml_peer_update_t peer = {.action = ML_PEER_ADD, .node_id = id, .vpn_ip = 0x64400000u + (uint32_t)id,
        .has_key = true, .has_online = presence != 0, .online = presence > 0,
        .has_last_seen = last_seen != 0, .last_seen = last_seen};
    memcpy(peer.public_key, &id, sizeof(id));
    peer.public_key[31] = 0x19;
    return peer;
}

static ml_peer_entry_t entry(uint64_t id, int presence, int64_t last_seen)
{
    return (ml_peer_entry_t){.peer = node(id, presence, last_seen), .allowed = true};
}

void test_peer_select(void)
{
    /* The order, rule by rule (ml_peer_select.c). */
    ml_peer_entry_t online = entry(9, 1, 0), unknown = entry(8, 0, 500), offline = entry(7, -1, 900);
    assert(ml_peer_ranks_above(&online, &unknown) && ml_peer_ranks_above(&unknown, &offline));
    ml_peer_entry_t recent = entry(10, -1, 1000), old = entry(1, -1, 100), never = entry(2, -1, 0);
    assert(ml_peer_ranks_above(&recent, &old) && ml_peer_ranks_above(&old, &never));
    /* LastSeen does not order online peers: the control server does not
     * update it while a node is online. */
    ml_peer_entry_t online_old = entry(3, 1, 100), online_new = entry(4, 1, 1000);
    assert(ml_peer_ranks_above(&online_old, &online_new));
    online_new.installed = true;
    assert(ml_peer_ranks_above(&online_new, &online_old));
    old.session = true;
    assert(ml_peer_ranks_above(&old, &online));
    never.priority = true;
    assert(ml_peer_ranks_above(&never, &old));
    assert(!ml_peer_ranks_above(&online, &online));

    /* The directory: one entry per node, found by ID, else by key. */
    ml_peer_dir_t dir = {0};
    for (unsigned id = 1; id <= 200; ++id) {
        ml_peer_update_t peer = node(id, id % 4 == 0 ? 1 : -1, (int64_t)id * 60);
        ml_peer_entry_t *stored = ml_peer_dir_put(&dir, &peer);
        assert(stored && stored->seen && stored->peer.node_id == id);
        stored->allowed = true;
    }
    assert(dir.count == 200 && dir.capacity == 256 && !dir.overflow_count);
    ml_peer_update_t again = node(5, 1, 0);
    ml_peer_dir_find(&dir, 5, NULL)->installed = true;
    ml_peer_entry_t *five = ml_peer_dir_put(&dir, &again);
    assert(dir.count == 200 && five->installed && five->peer.online);
    ml_peer_update_t by_key = node(5, 1, 0);
    assert(ml_peer_dir_find(&dir, 0, by_key.public_key) == five && !ml_peer_dir_find(&dir, 999, NULL));

    /* A presence patch changes no WireGuard field; an endpoint patch does. */
    ml_peer_update_t patch = {.node_id = 5, .has_online = true, .online = false,
        .has_last_seen = true, .last_seen = 77};
    assert(!ml_peer_dir_patch(five, &patch) && !five->peer.online && five->peer.last_seen == 77);
    patch = (ml_peer_update_t){.node_id = 5, .has_endpoints = true, .endpoint_count = 1};
    patch.endpoints[0].ip = 0xc0a80101;
    assert(ml_peer_dir_patch(five, &patch) && five->peer.endpoint_count == 1 && five->peer.has_key);

    /* 200 live peers in 64 slots: the 50 online, then the 14 offline peers
     * seen most recently, all above 100. */
    assert(ml_peer_select(&dir, 64) == 136);
    unsigned chosen = 0;
    for (unsigned i = 0; i < dir.count; ++i) {
        const ml_peer_entry_t *e = &dir.entries[i];
        chosen += e->selected;
        if (e->peer.has_online && e->peer.online) assert(e->selected);
        else assert(e->selected == (e->peer.node_id >= 182));
    }
    assert(chosen == 64);
    for (unsigned i = 0; i < dir.count; ++i)
        for (unsigned j = 0; j < dir.count; ++j)
            if (dir.entries[i].selected && !dir.entries[j].selected)
                assert(!ml_peer_ranks_above(&dir.entries[j], &dir.entries[i]));

    /* A peer that the allowlist does not admit is neither chosen nor left
     * out; fewer live peers than slots leave nobody out. */
    for (unsigned i = 0; i < dir.count; ++i) dir.entries[i].allowed = dir.entries[i].peer.node_id <= 50;
    assert(ml_peer_select(&dir, 64) == 0);
    for (unsigned i = 0; i < dir.count; ++i) assert(dir.entries[i].selected == dir.entries[i].allowed);

    /* Removal moves the last entry into the place. */
    ml_peer_entry_t *first = &dir.entries[0];
    uint64_t last_id = dir.entries[dir.count - 1].peer.node_id;
    ml_peer_dir_delete(&dir, first);
    assert(dir.count == 199 && first->peer.node_id == last_id);

    /* A full directory takes no more peers and counts each of them left
     * out once, until it leaves or a complete map starts. */
    for (unsigned id = 1000; dir.count < ML_PEER_DIR_MAX; ++id) {
        ml_peer_update_t peer = node(id, 1, 0);
        assert(ml_peer_dir_put(&dir, &peer));
    }
    ml_peer_update_t extra = node(5000, 1, 0);
    assert(!ml_peer_dir_put(&dir, &extra) && !ml_peer_dir_put(&dir, &extra));
    assert(dir.overflow_count == 1 && dir.capacity == ML_PEER_DIR_MAX);
    for (unsigned i = 0; i < dir.count; ++i) dir.entries[i].allowed = true;
    assert(ml_peer_select(&dir, 64) == ML_PEER_DIR_MAX - 64 + 1);
    assert(ml_peer_dir_forget(&dir, 5000) && !ml_peer_dir_forget(&dir, 5000));
    assert(ml_peer_select(&dir, 64) == ML_PEER_DIR_MAX - 64);
    /* Past the list of their IDs, more such peers are still counted. */
    for (unsigned id = 5000; id <= 5000 + ML_PEER_DIR_MAX; ++id) {
        ml_peer_update_t peer = node(id, 1, 0);
        assert(!ml_peer_dir_put(&dir, &peer));
    }
    assert(dir.overflow_count == ML_PEER_DIR_MAX && dir.untracked == 1);
    assert(ml_peer_select(&dir, 64) == 2 * ML_PEER_DIR_MAX - 64 + 1);
    ml_peer_dir_start_snapshot(&dir);
    assert(dir.snapshot && !dir.overflow_count && !dir.untracked && !dir.entries[0].seen);
    ml_peer_dir_free(&dir);
    assert(!dir.entries && !dir.count && !dir.capacity && !dir.overflow);

    /* A batch holds the updates of one response, and names the peers of
     * the complete map in it. */
    ml_peer_batch_t *batch = NULL;
    ml_peer_update_t update = {.action = ML_PEER_SYNC};
    assert(ml_peer_batch_add(&batch, &update));
    for (unsigned id = 1; id <= 100; ++id) {
        update = node(id, 1, 0);
        update.action = id == 100 ? ML_PEER_REMOVE : ML_PEER_ADD;
        assert(ml_peer_batch_add(&batch, &update));
    }
    update = (ml_peer_update_t){.action = ML_PEER_MAP_COMPLETE};
    assert(ml_peer_batch_add(&batch, &update));
    update = node(101, 1, 0);
    assert(ml_peer_batch_add(&batch, &update));
    assert(batch->count == 103 && batch->capacity == 128 && batch->updates[0].action == ML_PEER_SYNC);
    ml_peer_update_t one = node(1, 1, 0), hundred = node(100, 1, 0), after = node(101, 1, 0);
    ml_peer_update_t one_by_key = one;
    one_by_key.node_id = 0;
    assert(ml_peer_batch_lists(batch, 1, &one) && ml_peer_batch_lists(batch, 1, &one_by_key));
    assert(!ml_peer_batch_lists(batch, 1, &hundred) && !ml_peer_batch_lists(batch, 1, &after));
    free(batch);
    puts("Tailscale peer order for the WireGuard slots: online, recently seen, sessions, stable ties: PASS");
}

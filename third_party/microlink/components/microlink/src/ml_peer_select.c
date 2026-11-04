#include "ml_peer_select.h"
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
/* The directory and the batches stay out of internal RAM: about 250 bytes
 * per peer. */
#define PSRAM_REALLOC(pointer, size) heap_caps_realloc((pointer), (size), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define PSRAM_REALLOC realloc
#endif

bool ml_peer_batch_add(ml_peer_batch_t **batch, const ml_peer_update_t *update)
{
    ml_peer_batch_t *current = *batch;
    if (!current || current->count == current->capacity) {
        unsigned capacity = current ? current->capacity * 2 : 16;
        ml_peer_batch_t *grown = PSRAM_REALLOC(current,
            sizeof(*grown) + (size_t)capacity * sizeof(grown->updates[0]));
        if (!grown) return false;
        if (!current) grown->count = 0;
        grown->capacity = capacity;
        *batch = current = grown;
    }
    current->updates[current->count++] = *update;
    return true;
}

static bool same_peer(const ml_peer_update_t *a, const ml_peer_update_t *b)
{
    return (a->node_id && a->node_id == b->node_id) || !memcmp(a->public_key, b->public_key, 32);
}

bool ml_peer_batch_lists(const ml_peer_batch_t *batch, unsigned first, const ml_peer_update_t *peer)
{
    for (unsigned i = first; i < batch->count; ++i) {
        const ml_peer_update_t *update = &batch->updates[i];
        if (update->action == ML_PEER_MAP_COMPLETE) break;
        if (update->action == ML_PEER_ADD && same_peer(update, peer)) return true;
    }
    return false;
}

static unsigned overflow_index(const ml_peer_dir_t *dir, uint64_t node_id)
{
    unsigned i = 0;
    while (i < dir->overflow_count && dir->overflow[i] != node_id) ++i;
    return i;
}

static void note_overflow(ml_peer_dir_t *dir, uint64_t node_id)
{
    if (node_id && overflow_index(dir, node_id) < dir->overflow_count) return;
    if (node_id && dir->overflow_count == dir->overflow_capacity) {
        unsigned capacity = dir->overflow_capacity ? dir->overflow_capacity * 2 : ML_MAX_PEERS;
        uint64_t *grown = capacity <= ML_PEER_DIR_MAX ?
            PSRAM_REALLOC(dir->overflow, capacity * sizeof(*grown)) : NULL;
        if (grown) {
            dir->overflow = grown;
            dir->overflow_capacity = capacity;
        }
    }
    if (node_id && dir->overflow_count < dir->overflow_capacity)
        dir->overflow[dir->overflow_count++] = node_id;
    else
        ++dir->untracked;
}

bool ml_peer_dir_forget(ml_peer_dir_t *dir, uint64_t node_id)
{
    unsigned i = overflow_index(dir, node_id);
    if (!node_id || i == dir->overflow_count) return false;
    dir->overflow[i] = dir->overflow[--dir->overflow_count];
    return true;
}

void ml_peer_dir_start_snapshot(ml_peer_dir_t *dir)
{
    for (unsigned i = 0; i < dir->count; ++i) dir->entries[i].seen = false;
    dir->overflow_count = dir->untracked = 0;
    dir->snapshot = true;
}

ml_peer_entry_t *ml_peer_dir_find(ml_peer_dir_t *dir, uint64_t node_id, const uint8_t key[32])
{
    for (unsigned i = 0; i < dir->count; ++i)
        if (node_id && dir->entries[i].peer.node_id == node_id) return &dir->entries[i];
    for (unsigned i = 0; key && i < dir->count; ++i)
        if (!memcmp(dir->entries[i].peer.public_key, key, 32)) return &dir->entries[i];
    return NULL;
}

ml_peer_entry_t *ml_peer_dir_put(ml_peer_dir_t *dir, const ml_peer_update_t *node)
{
    ml_peer_entry_t *entry = ml_peer_dir_find(dir, node->node_id, node->public_key);
    if (!entry) {
        if (dir->count == dir->capacity) {
            unsigned capacity = dir->capacity ? dir->capacity * 2 : ML_MAX_PEERS;
            if (capacity > ML_PEER_DIR_MAX) capacity = ML_PEER_DIR_MAX;
            ml_peer_entry_t *grown = capacity > dir->capacity ?
                PSRAM_REALLOC(dir->entries, capacity * sizeof(*grown)) : NULL;
            if (!grown) {
                note_overflow(dir, node->node_id);
                return NULL;
            }
            dir->entries = grown;
            dir->capacity = capacity;
        }
        entry = &dir->entries[dir->count++];
        memset(entry, 0, sizeof(*entry));
        ml_peer_dir_forget(dir, node->node_id);
    }
    entry->peer = *node;
    entry->seen = true;
    return entry;
}

bool ml_peer_dir_patch(ml_peer_entry_t *entry, const ml_peer_update_t *patch)
{
    ml_peer_update_t *peer = &entry->peer;
    if (patch->has_key) memcpy(peer->public_key, patch->public_key, 32);
    if (patch->has_disco_key) memcpy(peer->disco_key, patch->disco_key, 32);
    if (patch->has_derp) peer->derp_region = patch->derp_region;
    if (patch->has_endpoints) {
        peer->endpoint_count = patch->endpoint_count;
        memcpy(peer->endpoints, patch->endpoints, sizeof(peer->endpoints));
    }
    if (patch->has_online) {
        peer->has_online = true;
        peer->online = patch->online;
    }
    if (patch->has_last_seen) {
        peer->has_last_seen = true;
        peer->last_seen = patch->last_seen;
    }
    return patch->has_key || patch->has_disco_key || patch->has_derp || patch->has_endpoints;
}

void ml_peer_dir_delete(ml_peer_dir_t *dir, ml_peer_entry_t *entry)
{
    *entry = dir->entries[--dir->count];
}

void ml_peer_dir_free(ml_peer_dir_t *dir)
{
    free(dir->entries);
    free(dir->overflow);
    memset(dir, 0, sizeof(*dir));
}

/* 2 online, 1 unknown, 0 offline. */
static int presence(const ml_peer_update_t *peer)
{
    return !peer->has_online ? 1 : peer->online ? 2 : 0;
}

/* The order of the peers for the WireGuard slots, first to last:
 * 1. the configured priority peer;
 * 2. an installed peer with a WireGuard session, so that a session in use
 *    keeps its slot across updates;
 * 3. online peers, then peers whose presence the map does not give, then
 *    offline peers (Node.Online);
 * 4. among the peers that are not online, the most recently seen first
 *    (Node.LastSeen, which the control server does not update while a node
 *    is online);
 * 5. an installed peer before one that is not, so that equal peers do not
 *    trade slots;
 * 6. the lower node ID, so that the order is total. */
bool ml_peer_ranks_above(const ml_peer_entry_t *a, const ml_peer_entry_t *b)
{
    if (a->priority != b->priority) return a->priority;
    if (a->session != b->session) return a->session;
    int presence_a = presence(&a->peer), presence_b = presence(&b->peer);
    if (presence_a != presence_b) return presence_a > presence_b;
    if (presence_a != 2) {
        int64_t seen_a = a->peer.has_last_seen ? a->peer.last_seen : 0;
        int64_t seen_b = b->peer.has_last_seen ? b->peer.last_seen : 0;
        if (seen_a != seen_b) return seen_a > seen_b;
    }
    if (a->installed != b->installed) return a->installed;
    return a->peer.node_id < b->peer.node_id;
}

unsigned ml_peer_select(ml_peer_dir_t *dir, unsigned slots)
{
    unsigned allowed = 0, chosen = 0;
    for (unsigned i = 0; i < dir->count; ++i) {
        dir->entries[i].selected = false;
        allowed += dir->entries[i].allowed;
    }
    for (; chosen < slots; ++chosen) {
        ml_peer_entry_t *best = NULL;
        for (unsigned i = 0; i < dir->count; ++i) {
            ml_peer_entry_t *entry = &dir->entries[i];
            if (entry->allowed && !entry->selected && (!best || ml_peer_ranks_above(entry, best)))
                best = entry;
        }
        if (!best) break;
        best->selected = true;
    }
    return allowed - chosen + dir->overflow_count + dir->untracked;
}

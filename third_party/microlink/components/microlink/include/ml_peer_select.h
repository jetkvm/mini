#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ml_peer_map.h"

/* The directory holds every live peer of the tailnet, also those without a
 * WireGuard slot, so that a tailnet with more live peers than ML_MAX_PEERS
 * gives the slots to the peers that ml_peer_select ranks first. */
#ifndef ML_PEER_DIR_MAX
#define ML_PEER_DIR_MAX 1024
#endif

typedef struct {
    ml_peer_update_t peer;  /* the last full node, with the patches since */
    bool seen;              /* named by the complete map being applied */
    bool installed;         /* holds a WireGuard slot */
    /* Set by the caller before ml_peer_select: the allowlist admits the
     * peer, it is the configured priority peer, and it has a WireGuard
     * session (only an installed peer has one). */
    bool allowed, priority, session;
    bool selected;          /* set by ml_peer_select */
} ml_peer_entry_t;

typedef struct {
    ml_peer_entry_t *entries;
    unsigned count, capacity;
    /* The node IDs of the live peers that did not fit the directory, each
     * once, and the number of those that did not fit this list either. */
    uint64_t *overflow;
    unsigned overflow_count, overflow_capacity, untracked;
    bool snapshot;          /* a complete map is being applied */
    bool changed;           /* a change since the last selection */
} ml_peer_dir_t;

/* The updates of one MapResponse, in order. The control task sends them to
 * the WireGuard task as one queue item, so that a map of any size takes one
 * place in the queue and a complete map arrives whole. */
typedef struct {
    unsigned count, capacity;
    ml_peer_update_t updates[];
} ml_peer_batch_t;

/* Appends a copy of the update. False when the batch cannot grow; it stays
 * valid, and NULL starts a new one. */
bool ml_peer_batch_add(ml_peer_batch_t **batch, const ml_peer_update_t *update);
/* True when an ADD of the complete map that starts at index first names the
 * peer, by node ID or by key. */
bool ml_peer_batch_lists(const ml_peer_batch_t *batch, unsigned first, const ml_peer_update_t *peer);

/* The entry of the node ID, else of the key (NULL: by ID only). */
ml_peer_entry_t *ml_peer_dir_find(ml_peer_dir_t *dir, uint64_t node_id, const uint8_t key[32]);
/* Stores a full node in its entry or a new one, marked seen. NULL, with the
 * peer counted as left out, when the directory cannot grow. */
ml_peer_entry_t *ml_peer_dir_put(ml_peer_dir_t *dir, const ml_peer_update_t *node);
/* Applies a PeersChangedPatch. True when a WireGuard field changed. */
bool ml_peer_dir_patch(ml_peer_entry_t *entry, const ml_peer_update_t *patch);
/* Moves the last entry into the place of this one. */
void ml_peer_dir_delete(ml_peer_dir_t *dir, ml_peer_entry_t *entry);
/* A peer outside the directory left the tailnet: it is no longer left out.
 * True when it was counted. */
bool ml_peer_dir_forget(ml_peer_dir_t *dir, uint64_t node_id);
/* A complete map starts: every entry is unseen until the map names it, and
 * the peers that did not fit are counted anew. */
void ml_peer_dir_start_snapshot(ml_peer_dir_t *dir);
void ml_peer_dir_free(ml_peer_dir_t *dir);
/* True when a ranks above b for a WireGuard slot. */
bool ml_peer_ranks_above(const ml_peer_entry_t *a, const ml_peer_entry_t *b);
/* Marks the allowed peers that rank first as selected, at most slots of
 * them, and returns the number of live peers that are left out. */
unsigned ml_peer_select(ml_peer_dir_t *dir, unsigned slots);

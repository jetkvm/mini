/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>

/* The TF card-detect pin, read every poll period. A reading counts when it
 * equals the reading before it, so a contact that bounces for less than one
 * period starts nothing. */
typedef struct {
    bool previous;
    /* A counted removal that the storage has not handled yet. */
    bool removed;
} jetkvm_tf_detect_t;

typedef struct {
    /* Release the card that the storage holds. */
    bool remove;
    /* Set up the card in the slot; after `remove` when both are set. */
    bool insert;
} jetkvm_tf_detect_action_t;

void jetkvm_tf_detect_start(jetkvm_tf_detect_t *detect, bool present);

/**
 * One reading of the pin. `storage_free` tells whether the poll got the
 * storage lock; without it the poll acts on nothing and keeps a counted
 * removal for the next poll that gets the lock, so a card that is taken
 * out and put back while a transfer holds the lock is still released
 * before the next one is set up. `card_recorded` is the storage's card.
 */
jetkvm_tf_detect_action_t jetkvm_tf_detect_poll(
    jetkvm_tf_detect_t *detect, bool present, bool storage_free,
    bool card_recorded);

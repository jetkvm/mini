#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "tf_detect_contract.h"

/* The storage as the firmware keeps it: whether it holds a card, and how
 * often each action ran. */
typedef struct {
    bool card;
    int removals;
    int insertions;
} storage_t;

static void poll(jetkvm_tf_detect_t *detect, storage_t *storage, bool present,
                 bool storage_free)
{
    jetkvm_tf_detect_action_t action = jetkvm_tf_detect_poll(
        detect, present, storage_free, storage->card);
    assert(storage_free || (!action.remove && !action.insert));
    if (action.remove) {
        assert(storage->card);
        storage->card = false;
        storage->removals++;
    }
    if (action.insert) {
        assert(!storage->card);
        storage->card = true;
        storage->insertions++;
    }
}

static void test_bounce_starts_nothing(void)
{
    jetkvm_tf_detect_t detect;
    storage_t storage = {0};
    jetkvm_tf_detect_start(&detect, false);
    const bool readings[] = {true, false, true, false};
    for (unsigned i = 0; i < sizeof(readings) / sizeof(readings[0]); ++i) {
        poll(&detect, &storage, readings[i], true);
    }
    assert(storage.insertions == 0);
    /* The second equal reading counts. */
    poll(&detect, &storage, false, true);
    poll(&detect, &storage, true, true);
    assert(storage.insertions == 0);
    poll(&detect, &storage, true, true);
    assert(storage.insertions == 1 && storage.card);
    /* A card that stays in is set up once. */
    poll(&detect, &storage, true, true);
    poll(&detect, &storage, true, true);
    assert(storage.insertions == 1);
}

static void test_removal_and_insertion(void)
{
    jetkvm_tf_detect_t detect;
    storage_t storage = {.card = true};
    jetkvm_tf_detect_start(&detect, true);
    /* One reading without the card is contact bounce. */
    poll(&detect, &storage, false, true);
    poll(&detect, &storage, true, true);
    assert(storage.removals == 0 && storage.card);
    poll(&detect, &storage, false, true);
    poll(&detect, &storage, false, true);
    assert(storage.removals == 1 && !storage.card);
    poll(&detect, &storage, false, true);
    assert(storage.removals == 1);
    poll(&detect, &storage, true, true);
    poll(&detect, &storage, true, true);
    assert(storage.insertions == 1 && storage.card);
}

static void test_swap_while_storage_busy(void)
{
    jetkvm_tf_detect_t detect;
    storage_t storage = {.card = true};
    jetkvm_tf_detect_start(&detect, true);
    /* A transfer holds the storage while the card is taken out and another
     * one is put in. */
    const bool readings[] = {false, false, false, true, true};
    for (unsigned i = 0; i < sizeof(readings) / sizeof(readings[0]); ++i) {
        poll(&detect, &storage, readings[i], false);
    }
    assert(storage.removals == 0 && storage.insertions == 0);
    /* The first poll with the storage free releases the old card and sets
     * up the new one. */
    poll(&detect, &storage, true, true);
    assert(storage.removals == 1 && storage.insertions == 1 && storage.card);
    poll(&detect, &storage, true, true);
    assert(storage.removals == 1 && storage.insertions == 1);
}

static void test_removal_while_storage_busy(void)
{
    jetkvm_tf_detect_t detect;
    storage_t storage = {.card = true};
    jetkvm_tf_detect_start(&detect, true);
    poll(&detect, &storage, false, false);
    poll(&detect, &storage, false, false);
    poll(&detect, &storage, false, true);
    assert(storage.removals == 1 && storage.insertions == 0 && !storage.card);
}

static void test_busy_bounce_is_no_removal(void)
{
    jetkvm_tf_detect_t detect;
    storage_t storage = {.card = true};
    jetkvm_tf_detect_start(&detect, true);
    poll(&detect, &storage, false, false);
    poll(&detect, &storage, true, false);
    poll(&detect, &storage, true, true);
    assert(storage.removals == 0 && storage.insertions == 0 && storage.card);
}

static void test_no_card_at_start(void)
{
    jetkvm_tf_detect_t detect;
    storage_t storage = {0};
    jetkvm_tf_detect_start(&detect, false);
    poll(&detect, &storage, false, true);
    poll(&detect, &storage, false, true);
    assert(storage.removals == 0 && storage.insertions == 0);
}

int main(void)
{
    test_bounce_starts_nothing();
    test_removal_and_insertion();
    test_swap_while_storage_busy();
    test_removal_while_storage_busy();
    test_busy_bounce_is_no_removal();
    test_no_card_at_start();
    puts("TF card detect contract: PASS");
    return 0;
}

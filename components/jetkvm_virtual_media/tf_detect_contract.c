/* SPDX-License-Identifier: MIT */
#include "tf_detect_contract.h"

void jetkvm_tf_detect_start(jetkvm_tf_detect_t *detect, bool present)
{
    detect->previous = present;
    detect->removed = false;
}

jetkvm_tf_detect_action_t jetkvm_tf_detect_poll(
    jetkvm_tf_detect_t *detect, bool present, bool storage_free,
    bool card_recorded)
{
    jetkvm_tf_detect_action_t action = {false, false};
    const bool counted = present == detect->previous;
    detect->previous = present;
    if (counted && !present) {
        detect->removed = true;
    }
    if (!storage_free) {
        return action;
    }
    action.remove = detect->removed && card_recorded;
    detect->removed = false;
    action.insert = counted && present && (!card_recorded || action.remove);
    return action;
}

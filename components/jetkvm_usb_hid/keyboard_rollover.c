#include "keyboard_rollover.h"

#include <stddef.h>
#include <string.h>

static bool usage_is_set(const jetkvm_keyboard_usage_state_t *state,
                         uint8_t usage)
{
    return (state->usages[usage >> 3U] & (uint8_t)(1U << (usage & 7U))) != 0;
}

void jetkvm_keyboard_usage_clear(jetkvm_keyboard_usage_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

bool jetkvm_keyboard_usage_update(jetkvm_keyboard_usage_state_t *state,
                                  uint8_t usage,
                                  bool pressed)
{
    const uint8_t mask = (uint8_t)(1U << (usage & 7U));
    uint8_t *const entry = &state->usages[usage >> 3U];
    const bool was_pressed = (*entry & mask) != 0;

    if (pressed) {
        *entry |= mask;
    } else {
        *entry &= (uint8_t)~mask;
    }
    return was_pressed != pressed;
}

void jetkvm_keyboard_usage_from_report(
    jetkvm_keyboard_usage_state_t *state,
    const uint8_t keycodes[JETKVM_KEYBOARD_ROLLOVER_KEY_COUNT])
{
    jetkvm_keyboard_usage_clear(state);
    bool rollover_report = true;
    for (size_t i = 0; i < JETKVM_KEYBOARD_ROLLOVER_KEY_COUNT; ++i) {
        rollover_report = rollover_report &&
                          keycodes[i] == JETKVM_KEYBOARD_ERROR_ROLLOVER;
    }
    if (rollover_report) {
        /* A raw rollover report does not reveal the underlying pressed set. */
        return;
    }
    for (size_t i = 0; i < JETKVM_KEYBOARD_ROLLOVER_KEY_COUNT; ++i) {
        const uint8_t usage = keycodes[i];
        if (usage != 0 && !(usage >= 0xe0U && usage <= 0xe7U)) {
            (void)jetkvm_keyboard_usage_update(state, usage, true);
        }
    }
}

void jetkvm_keyboard_usage_build_report(
    const jetkvm_keyboard_usage_state_t *state,
    uint8_t keycodes[JETKVM_KEYBOARD_ROLLOVER_KEY_COUNT])
{
    size_t count = 0;
    memset(keycodes, 0, JETKVM_KEYBOARD_ROLLOVER_KEY_COUNT);

    /* Usage zero is NoEvent; modifiers live in the report modifier byte. */
    for (unsigned int usage = 1; usage <= UINT8_MAX; ++usage) {
        if (usage >= 0xe0U && usage <= 0xe7U) {
            continue;
        }
        if (!usage_is_set(state, (uint8_t)usage)) {
            continue;
        }
        if (count == JETKVM_KEYBOARD_ROLLOVER_KEY_COUNT) {
            memset(keycodes, JETKVM_KEYBOARD_ERROR_ROLLOVER,
                   JETKVM_KEYBOARD_ROLLOVER_KEY_COUNT);
            return;
        }
        keycodes[count++] = (uint8_t)usage;
    }
}

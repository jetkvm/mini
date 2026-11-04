#pragma once

#include <stdbool.h>
#include <stdint.h>

#define JETKVM_KEYBOARD_ROLLOVER_KEY_COUNT 6U
#define JETKVM_KEYBOARD_USAGE_BITMAP_BYTES 32U
#define JETKVM_KEYBOARD_ERROR_ROLLOVER 0x01U

typedef struct {
    uint8_t usages[JETKVM_KEYBOARD_USAGE_BITMAP_BYTES];
} jetkvm_keyboard_usage_state_t;

void jetkvm_keyboard_usage_clear(jetkvm_keyboard_usage_state_t *state);

bool jetkvm_keyboard_usage_update(jetkvm_keyboard_usage_state_t *state,
                                  uint8_t usage,
                                  bool pressed);

void jetkvm_keyboard_usage_from_report(
    jetkvm_keyboard_usage_state_t *state,
    const uint8_t keycodes[JETKVM_KEYBOARD_ROLLOVER_KEY_COUNT]);

void jetkvm_keyboard_usage_build_report(
    const jetkvm_keyboard_usage_state_t *state,
    uint8_t keycodes[JETKVM_KEYBOARD_ROLLOVER_KEY_COUNT]);

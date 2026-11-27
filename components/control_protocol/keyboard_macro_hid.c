/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* Stored keyboard macros as HID macro steps, for runKeyboardMacro and the
 * MQTT keyboard_macro/set command (Go internal/keyboard). No cJSON here, so
 * host tests of the MQTT handler can link it. */

#include "jetkvm_control.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* USB HID usage IDs, in the order of keyboard_key_names.inc, from the same
 * table of the web UI (keyboardMappings.ts `keys`). */
static const uint8_t s_keyboard_macro_key_codes[] = {
#include "keyboard_key_codes.inc"
};

/* Modifier bits, in the order of s_keyboard_macro_modifier_names in
 * rpc_macros.c, from keyboardMappings.ts `modifiers`. */
static const uint8_t s_keyboard_macro_modifier_masks[] = {
    0x01U, 0x10U, 0x02U, 0x20U, 0x04U, 0x40U, 0x08U, 0x80U,
};

_Static_assert(
    sizeof(s_keyboard_macro_key_codes) ==
        JETKVM_CONTROL_KEYBOARD_MACRO_KEY_NAME_COUNT,
    "saved-macro key codes must match the key name table");
_Static_assert(
    sizeof(s_keyboard_macro_modifier_masks) ==
        JETKVM_CONTROL_KEYBOARD_MACRO_MAX_MODIFIERS,
    "saved-macro modifier masks must match the modifier name table");

static char ascii_lower(char value)
{
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

static bool ascii_equal_fold(const char *left, const char *right)
{
    while (*left != '\0' && ascii_lower(*left) == ascii_lower(*right)) {
        left++;
        right++;
    }
    return *left == '\0' && *right == '\0';
}

int jetkvm_control_keyboard_macro_find(
    const jetkvm_control_keyboard_macro_list_t *list, const char *id_or_name)
{
    if (list == NULL || id_or_name == NULL ||
        list->count > JETKVM_CONTROL_KEYBOARD_MACRO_MAX_COUNT) {
        return -1;
    }
    for (size_t i = 0U; i < list->count; ++i) {
        if (strncmp(list->macros[i].id, id_or_name,
                    sizeof(list->macros[i].id)) == 0) {
            return (int)i;
        }
    }
    for (size_t i = 0U; i < list->count; ++i) {
        if (memchr(list->macros[i].name, '\0',
                   sizeof(list->macros[i].name)) != NULL &&
            ascii_equal_fold(list->macros[i].name, id_or_name)) {
            return (int)i;
        }
    }
    return -1;
}

/* Timing of executeMacroRemote in the web UI and of Go keyboard.HIDSteps. */
#define KEYBOARD_MACRO_PRESS_DELAY_MS 20U

bool jetkvm_control_keyboard_macro_hid_steps(
    const jetkvm_control_keyboard_macro_t *macro,
    jetkvm_hidrpc_macro_step_t steps[JETKVM_CONTROL_KEYBOARD_MACRO_MAX_HID_STEPS],
    size_t *step_count)
{
    if (macro == NULL || steps == NULL || step_count == NULL ||
        macro->step_count > JETKVM_CONTROL_KEYBOARD_MACRO_MAX_STEPS) {
        return false;
    }
    size_t count = 0U;
    for (size_t i = 0U; i < macro->step_count; ++i) {
        const jetkvm_control_keyboard_macro_step_t *step = &macro->steps[i];
        if (step->key_count > JETKVM_CONTROL_KEYBOARD_MACRO_MAX_KEYS ||
            step->modifier_count >
                JETKVM_CONTROL_KEYBOARD_MACRO_MAX_MODIFIERS) {
            return false;
        }
        if (step->key_count == 0U && step->modifier_count == 0U) {
            continue;
        }
        jetkvm_hidrpc_macro_step_t press = {
            .delay_ms = KEYBOARD_MACRO_PRESS_DELAY_MS,
        };
        for (size_t j = 0U; j < step->modifier_count; ++j) {
            if (step->modifier_ids[j] >=
                JETKVM_CONTROL_KEYBOARD_MACRO_MAX_MODIFIERS) {
                return false;
            }
            press.modifier |=
                s_keyboard_macro_modifier_masks[step->modifier_ids[j]];
        }
        for (size_t j = 0U; j < step->key_count; ++j) {
            if (step->key_ids[j] >=
                JETKVM_CONTROL_KEYBOARD_MACRO_KEY_NAME_COUNT) {
                return false;
            }
            /* The USB gadget keeps six keys of a report, as in Go. */
            if (j < JETKVM_HIDRPC_KEY_COUNT) {
                press.keys[j] = s_keyboard_macro_key_codes[step->key_ids[j]];
            }
        }
        steps[count++] = press;
        steps[count++] = (jetkvm_hidrpc_macro_step_t){
            .delay_ms = step->delay_ms,
        };
    }
    *step_count = count;
    return true;
}

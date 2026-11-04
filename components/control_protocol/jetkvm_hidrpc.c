/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_control.h"

#include <string.h>

static uint16_t read_be_u16(const uint8_t *input)
{
    return (uint16_t)(((uint16_t)input[0] << 8) | (uint16_t)input[1]);
}

static uint32_t read_be_u32(const uint8_t *input)
{
    return ((uint32_t)input[0] << 24) |
           ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) |
           (uint32_t)input[3];
}

static int32_t read_be_i32(const uint8_t *input)
{
    uint32_t value = read_be_u32(input);
    if (value <= INT32_MAX) {
        return (int32_t)value;
    }
    return (int32_t)(-1 - (int64_t)(UINT32_MAX - value));
}

bool jetkvm_hidrpc_macro_step_at(
    const jetkvm_hidrpc_keyboard_macro_report_t *macro, uint32_t index,
    jetkvm_hidrpc_macro_step_t *step)
{
    if (!macro || !step || macro->step_count > JETKVM_HIDRPC_MACRO_MAX_STEPS ||
        index >= macro->step_count ||
        !macro->encoded_steps ||
        macro->encoded_steps_size / JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE !=
            macro->step_count ||
        macro->encoded_steps_size % JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE != 0U) {
        return false;
    }

    const uint8_t *encoded = macro->encoded_steps +
        (size_t)index * JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE;
    step->modifier = encoded[0];
    memcpy(step->keys, encoded + 1U, JETKVM_HIDRPC_KEY_COUNT);
    step->delay_ms = read_be_u16(encoded + 1U + JETKVM_HIDRPC_KEY_COUNT);
    return true;
}

static jetkvm_control_result_t dispatch_hidrpc_event(
    const jetkvm_hidrpc_context_t *context,
    const jetkvm_hidrpc_event_t *event)
{
    if (!context || !context->on_event) {
        return JETKVM_CONTROL_NO_RESPONSE;
    }
    context->on_event(event, context->ctx);
    return JETKVM_CONTROL_EVENT_HANDLED;
}

jetkvm_control_result_t jetkvm_control_handle_hidrpc(
    const uint8_t *input, size_t input_size,
    uint8_t *output, size_t output_capacity, size_t *output_size)
{
    return jetkvm_control_handle_hidrpc_with_context(
        NULL, input, input_size, output, output_capacity, output_size);
}

jetkvm_control_result_t jetkvm_control_handle_hidrpc_with_context(
    const jetkvm_hidrpc_context_t *context,
    const uint8_t *input, size_t input_size,
    uint8_t *output, size_t output_capacity, size_t *output_size)
{
    if (!input || !output || !output_size) {
        return JETKVM_CONTROL_INVALID_ARGUMENT;
    }
    *output_size = 0;

    if (input_size == 0U) {
        return JETKVM_CONTROL_MALFORMED_INPUT;
    }

    if (input[0] == JETKVM_HIDRPC_TYPE_HANDSHAKE) {
        if (input_size != 2U) {
            return JETKVM_CONTROL_MALFORMED_INPUT;
        }
        if (output_capacity < 2U) {
            return JETKVM_CONTROL_OUTPUT_TOO_SMALL;
        }
        output[0] = JETKVM_HIDRPC_TYPE_HANDSHAKE;
        output[1] = JETKVM_HIDRPC_VERSION;
        *output_size = 2U;
        return JETKVM_CONTROL_RESPONSE_READY;
    }

    jetkvm_hidrpc_event_t event = { 0 };
    switch (input[0]) {
    case JETKVM_HIDRPC_TYPE_KEYBOARD_REPORT:
        if (input_size != 2U + JETKVM_HIDRPC_KEY_COUNT) {
            return JETKVM_CONTROL_MALFORMED_INPUT;
        }
        event.type = JETKVM_HIDRPC_EVENT_KEYBOARD_REPORT;
        event.data.keyboard.modifier = input[1];
        memcpy(event.data.keyboard.keys, input + 2U,
               JETKVM_HIDRPC_KEY_COUNT);
        break;
    case JETKVM_HIDRPC_TYPE_ABS_MOUSE_REPORT:
        if (input_size != 10U) {
            return JETKVM_CONTROL_MALFORMED_INPUT;
        }
        event.type = JETKVM_HIDRPC_EVENT_ABS_MOUSE_REPORT;
        event.data.abs_mouse.x = read_be_i32(input + 1U);
        event.data.abs_mouse.y = read_be_i32(input + 5U);
        event.data.abs_mouse.buttons = input[9];
        break;
    case JETKVM_HIDRPC_TYPE_KEYPRESS_REPORT:
        if (input_size != 3U || input[2] > 1U) {
            return JETKVM_CONTROL_MALFORMED_INPUT;
        }
        event.type = JETKVM_HIDRPC_EVENT_KEYPRESS_REPORT;
        event.data.keypress.key = input[1];
        event.data.keypress.pressed = input[2] == 1U;
        break;
    case JETKVM_HIDRPC_TYPE_REL_MOUSE_REPORT:
        if (input_size != 4U) {
            return JETKVM_CONTROL_MALFORMED_INPUT;
        }
        event.type = JETKVM_HIDRPC_EVENT_REL_MOUSE_REPORT;
        event.data.rel_mouse.dx = (int8_t)input[1];
        event.data.rel_mouse.dy = (int8_t)input[2];
        event.data.rel_mouse.buttons = input[3];
        break;
    case JETKVM_HIDRPC_TYPE_KEYBOARD_MACRO_REPORT: {
        if (input_size < 6U || input[1] > 1U) {
            return JETKVM_CONTROL_MALFORMED_INPUT;
        }
        uint32_t step_count = read_be_u32(input + 2U);
        size_t encoded_steps_size = input_size - 6U;
        if (step_count > JETKVM_HIDRPC_MACRO_MAX_STEPS ||
            encoded_steps_size % JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE != 0U ||
            encoded_steps_size / JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE !=
                step_count) {
            return JETKVM_CONTROL_MALFORMED_INPUT;
        }
        event.type = JETKVM_HIDRPC_EVENT_KEYBOARD_MACRO_REPORT;
        event.data.macro.is_paste = input[1] == 1U;
        event.data.macro.step_count = step_count;
        event.data.macro.encoded_steps = input + 6U;
        event.data.macro.encoded_steps_size = encoded_steps_size;
        break;
    }
    case JETKVM_HIDRPC_TYPE_CANCEL_KEYBOARD_MACRO_REPORT:
        if (input_size != 1U) {
            return JETKVM_CONTROL_MALFORMED_INPUT;
        }
        event.type = JETKVM_HIDRPC_EVENT_CANCEL_KEYBOARD_MACRO_REPORT;
        break;
    case JETKVM_HIDRPC_TYPE_KEYPRESS_KEEPALIVE_REPORT:
        if (input_size != 1U) {
            return JETKVM_CONTROL_MALFORMED_INPUT;
        }
        event.type = JETKVM_HIDRPC_EVENT_KEYPRESS_KEEPALIVE_REPORT;
        break;
    default:
        return JETKVM_CONTROL_NO_RESPONSE;
    }

    return dispatch_hidrpc_event(context, &event);
}

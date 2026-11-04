#include "jetkvm_cloud_core.h"

#include <stdio.h>
#include <string.h>

static bool is_ascii_control_or_space(unsigned char value)
{
    return value <= 0x20U || value == 0x7fU;
}

static size_t bounded_strlen(const char *value, size_t maximum)
{
    size_t length = 0;
    if (!value) {
        return 0;
    }
    while (length < maximum && value[length]) {
        length++;
    }
    return length;
}

static bool valid_header_value(const char *value, size_t maximum,
                               bool allow_space)
{
    if (!value || !value[0]) {
        return false;
    }
    size_t length = bounded_strlen(value, maximum + 1U);
    if (length == 0 || length > maximum) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char current = (unsigned char)value[i];
        if (current < 0x20U || current == 0x7fU || current > 0x7eU ||
            (!allow_space && current == 0x20U)) {
            return false;
        }
    }
    return true;
}

jetkvm_cloud_core_status_t jetkvm_cloud_core_make_ws_url(
    const char *cloud_url, char *output, size_t output_capacity,
    bool *uses_tls)
{
    if (!cloud_url || !output || output_capacity == 0 || !uses_tls) {
        return JETKVM_CLOUD_CORE_INVALID_ARGUMENT;
    }
    size_t input_length = bounded_strlen(
        cloud_url, JETKVM_CLOUD_CORE_MAX_URL_SIZE + 1U);
    if (input_length == 0 || input_length > JETKVM_CLOUD_CORE_MAX_URL_SIZE) {
        return JETKVM_CLOUD_CORE_INVALID_URL;
    }

    const char *replacement = NULL;
    size_t scheme_length = 0;
    if (strncmp(cloud_url, "https://", 8) == 0) {
        replacement = "wss://";
        scheme_length = 8;
        *uses_tls = true;
    } else if (strncmp(cloud_url, "http://", 7) == 0) {
        replacement = "ws://";
        scheme_length = 7;
        *uses_tls = false;
    } else {
        return JETKVM_CLOUD_CORE_INVALID_URL;
    }

    const char *remainder = cloud_url + scheme_length;
    const char *authority_end = remainder;
    while (*authority_end && *authority_end != '/' &&
           *authority_end != '?' && *authority_end != '#') {
        authority_end++;
    }
    if (authority_end == remainder || *remainder == ':' ||
        authority_end[-1] == ':' || memchr(remainder, '@',
                                            (size_t)(authority_end - remainder))) {
        return JETKVM_CLOUD_CORE_INVALID_URL;
    }
    for (size_t i = 0; i < input_length; ++i) {
        unsigned char current = (unsigned char)cloud_url[i];
        if (is_ascii_control_or_space(current) || current > 0x7eU ||
            current == '#') {
            return JETKVM_CLOUD_CORE_INVALID_URL;
        }
    }

    size_t prefix_length = strlen(replacement);
    size_t remainder_length = input_length - scheme_length;
    size_t output_length = prefix_length + remainder_length;
    if (output_length + 1U > output_capacity) {
        return JETKVM_CLOUD_CORE_OUTPUT_TOO_SMALL;
    }
    memcpy(output, replacement, prefix_length);
    memcpy(output + prefix_length, remainder, remainder_length);
    output[output_length] = '\0';
    return JETKVM_CLOUD_CORE_OK;
}

jetkvm_cloud_core_status_t jetkvm_cloud_core_make_headers(
    const char *device_id, const char *app_version, const char *sku,
    const char *token, char *output, size_t output_capacity,
    size_t *output_length)
{
    if (output_length) {
        *output_length = 0;
    }
    if (!device_id || !app_version || !sku || !token || !output ||
        output_capacity == 0 || !output_length) {
        return JETKVM_CLOUD_CORE_INVALID_ARGUMENT;
    }
    if (!valid_header_value(device_id,
                            JETKVM_CLOUD_CORE_MAX_DEVICE_ID_SIZE, false) ||
        !valid_header_value(app_version,
                            JETKVM_CLOUD_CORE_MAX_APP_VERSION_SIZE, true) ||
        !valid_header_value(sku, JETKVM_CLOUD_CORE_MAX_SKU_SIZE, false) ||
        !valid_header_value(token, JETKVM_CLOUD_CORE_MAX_TOKEN_SIZE, false)) {
        return JETKVM_CLOUD_CORE_INVALID_HEADER_VALUE;
    }

    int rendered = snprintf(output, output_capacity,
                            "X-Device-ID: %s\r\n"
                            "X-App-Version: %s"
                            JETKVM_CLOUD_CORE_APP_VERSION_SUFFIX "\r\n"
                            "X-Device-SKU: %s\r\n"
                            "Authorization: Bearer %s\r\n",
                            device_id, app_version, sku, token);
    if (rendered < 0 || (size_t)rendered >= output_capacity) {
        if (output_capacity) {
            output[0] = '\0';
        }
        return JETKVM_CLOUD_CORE_OUTPUT_TOO_SMALL;
    }
    *output_length = (size_t)rendered;
    return JETKVM_CLOUD_CORE_OK;
}

void jetkvm_cloud_reassembler_init(jetkvm_cloud_reassembler_t *state,
                                   uint8_t *buffer, size_t capacity)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->buffer = buffer;
    state->capacity = capacity;
    if (buffer && capacity) {
        buffer[0] = '\0';
    }
}

void jetkvm_cloud_reassembler_reset(jetkvm_cloud_reassembler_t *state)
{
    if (!state) {
        return;
    }
    uint8_t *buffer = state->buffer;
    size_t capacity = state->capacity;
    memset(state, 0, sizeof(*state));
    state->buffer = buffer;
    state->capacity = capacity;
    if (buffer && capacity) {
        buffer[0] = '\0';
    }
}

static jetkvm_cloud_reassembly_result_t reassembly_fail(
    jetkvm_cloud_reassembler_t *state,
    jetkvm_cloud_reassembly_result_t result)
{
    jetkvm_cloud_reassembler_reset(state);
    return result;
}

jetkvm_cloud_reassembly_result_t jetkvm_cloud_reassembler_push(
    jetkvm_cloud_reassembler_t *state, uint8_t opcode, bool fin,
    size_t payload_length, size_t payload_offset,
    const uint8_t *data, size_t data_length)
{
    if (!state || !state->buffer || state->capacity < 2U ||
        (data_length != 0 && !data)) {
        return JETKVM_CLOUD_REASSEMBLY_PROTOCOL_ERROR;
    }
    if (opcode == JETKVM_CLOUD_WS_OPCODE_PING ||
        opcode == JETKVM_CLOUD_WS_OPCODE_PONG ||
        opcode == JETKVM_CLOUD_WS_OPCODE_CLOSE) {
        return JETKVM_CLOUD_REASSEMBLY_CONTROL;
    }
    if (state->complete) {
        return reassembly_fail(state, JETKVM_CLOUD_REASSEMBLY_PROTOCOL_ERROR);
    }
    if (opcode != JETKVM_CLOUD_WS_OPCODE_TEXT &&
        opcode != JETKVM_CLOUD_WS_OPCODE_CONTINUATION) {
        return reassembly_fail(state, JETKVM_CLOUD_REASSEMBLY_PROTOCOL_ERROR);
    }
    if (payload_offset > payload_length ||
        data_length > payload_length - payload_offset) {
        return reassembly_fail(state, JETKVM_CLOUD_REASSEMBLY_PROTOCOL_ERROR);
    }

    if (payload_offset == 0) {
        if (state->frame_active) {
            return reassembly_fail(state,
                                   JETKVM_CLOUD_REASSEMBLY_PROTOCOL_ERROR);
        }
        if (opcode == JETKVM_CLOUD_WS_OPCODE_TEXT) {
            if (state->message_active) {
                return reassembly_fail(state,
                                       JETKVM_CLOUD_REASSEMBLY_PROTOCOL_ERROR);
            }
            state->message_active = true;
        } else if (!state->message_active) {
            return reassembly_fail(state,
                                   JETKVM_CLOUD_REASSEMBLY_PROTOCOL_ERROR);
        }

        if (payload_length > JETKVM_CLOUD_CORE_MAX_MESSAGE_SIZE -
                                 state->message_length ||
            state->message_length + payload_length + 1U > state->capacity) {
            return reassembly_fail(state,
                                   JETKVM_CLOUD_REASSEMBLY_TOO_LARGE);
        }
        state->frame_base = state->message_length;
        state->frame_length = payload_length;
        state->frame_received = 0;
        state->frame_opcode = opcode;
        state->frame_fin = fin;
        state->frame_active = true;
    } else if (!state->frame_active ||
               opcode != state->frame_opcode || fin != state->frame_fin ||
               payload_length != state->frame_length ||
               payload_offset != state->frame_received) {
        return reassembly_fail(state, JETKVM_CLOUD_REASSEMBLY_PROTOCOL_ERROR);
    }

    if (payload_offset != state->frame_received) {
        return reassembly_fail(state, JETKVM_CLOUD_REASSEMBLY_PROTOCOL_ERROR);
    }
    if (data_length) {
        memcpy(state->buffer + state->frame_base + payload_offset,
               data, data_length);
    }
    state->frame_received += data_length;
    if (state->frame_received < state->frame_length) {
        return JETKVM_CLOUD_REASSEMBLY_INCOMPLETE;
    }
    if (state->frame_received != state->frame_length) {
        return reassembly_fail(state, JETKVM_CLOUD_REASSEMBLY_PROTOCOL_ERROR);
    }

    state->message_length = state->frame_base + state->frame_length;
    state->frame_active = false;
    state->frame_length = 0;
    state->frame_received = 0;
    if (!state->frame_fin) {
        return JETKVM_CLOUD_REASSEMBLY_INCOMPLETE;
    }

    state->message_active = false;
    state->complete = true;
    state->buffer[state->message_length] = '\0';
    return JETKVM_CLOUD_REASSEMBLY_COMPLETE;
}

const uint8_t *jetkvm_cloud_reassembler_message_data(
    const jetkvm_cloud_reassembler_t *state)
{
    return state && state->complete ? state->buffer : NULL;
}

size_t jetkvm_cloud_reassembler_message_size(
    const jetkvm_cloud_reassembler_t *state)
{
    return state && state->complete ? state->message_length : 0;
}

static uint32_t next_nonzero(uint32_t value)
{
    value++;
    return value == 0 ? 1U : value;
}

uint32_t jetkvm_cloud_epoch_connected(jetkvm_cloud_epoch_state_t *state)
{
    if (!state) {
        return 0;
    }
    state->connection_epoch = next_nonzero(state->connection_epoch);
    state->connected = true;
    return state->connection_epoch;
}

void jetkvm_cloud_epoch_disconnected(jetkvm_cloud_epoch_state_t *state,
                                     uint32_t connection_epoch)
{
    if (state && connection_epoch != 0 &&
        state->connection_epoch == connection_epoch) {
        state->connected = false;
    }
}

bool jetkvm_cloud_epoch_is_current_connection(
    const jetkvm_cloud_epoch_state_t *state, uint32_t connection_epoch)
{
    return state && state->connected && connection_epoch != 0 &&
           state->connection_epoch == connection_epoch;
}

bool jetkvm_cloud_epoch_bind_session(jetkvm_cloud_epoch_state_t *state,
                                     uint32_t connection_epoch,
                                     uint32_t session_id,
                                     uint32_t generation)
{
    if (!jetkvm_cloud_epoch_is_current_connection(state, connection_epoch) ||
        session_id == 0 || generation == 0) {
        return false;
    }
    state->session_active = true;
    state->session_connection_epoch = connection_epoch;
    state->session_id = session_id;
    state->generation = generation;
    return true;
}

bool jetkvm_cloud_epoch_matches_session(
    const jetkvm_cloud_epoch_state_t *state, uint32_t connection_epoch,
    uint32_t session_id, uint32_t generation)
{
    return jetkvm_cloud_epoch_is_current_connection(state, connection_epoch) &&
           state->session_active &&
           state->session_connection_epoch == connection_epoch &&
           state->session_id == session_id &&
           state->generation == generation;
}

bool jetkvm_cloud_epoch_replace_generation(
    jetkvm_cloud_epoch_state_t *state, uint32_t session_id,
    uint32_t invalidated_generation, uint32_t replacement_generation)
{
    if (!state || !state->session_active || session_id == 0 ||
        invalidated_generation == 0 || replacement_generation == 0 ||
        invalidated_generation == replacement_generation ||
        state->session_id != session_id ||
        state->generation != invalidated_generation) {
        return false;
    }
    state->generation = replacement_generation;
    return true;
}

void jetkvm_cloud_epoch_clear_session(jetkvm_cloud_epoch_state_t *state,
                                      uint32_t session_id,
                                      uint32_t generation)
{
    if (!state || !state->session_active || state->session_id != session_id ||
        state->generation != generation) {
        return;
    }
    state->session_active = false;
    state->session_connection_epoch = 0;
    state->session_id = 0;
    state->generation = 0;
}

void jetkvm_cloud_retry_request(jetkvm_cloud_retry_t *retry, int64_t now_us,
                                uint32_t delay_ms)
{
    if (!retry) {
        return;
    }
    retry->pending = true;
    retry->not_before_us = now_us + (int64_t)delay_ms * 1000;
}

bool jetkvm_cloud_retry_due(const jetkvm_cloud_retry_t *retry, int64_t now_us,
                            bool network_up)
{
    return retry && retry->pending && network_up &&
           now_us >= retry->not_before_us;
}

void jetkvm_cloud_retry_started(jetkvm_cloud_retry_t *retry)
{
    if (retry) {
        retry->pending = false;
    }
}

bool jetkvm_cloud_core_authenticated_step_requires_reconnect(
    bool identity_authenticated, bool step_succeeded)
{
    return identity_authenticated && !step_succeeded;
}

const char *jetkvm_cloud_core_status_name(jetkvm_cloud_core_status_t status)
{
    switch (status) {
    case JETKVM_CLOUD_CORE_OK: return "ok";
    case JETKVM_CLOUD_CORE_INVALID_ARGUMENT: return "invalid_argument";
    case JETKVM_CLOUD_CORE_INVALID_URL: return "invalid_url";
    case JETKVM_CLOUD_CORE_INVALID_HEADER_VALUE: return "invalid_header_value";
    case JETKVM_CLOUD_CORE_OUTPUT_TOO_SMALL: return "output_too_small";
    case JETKVM_CLOUD_CORE_PROTOCOL_ERROR: return "protocol_error";
    case JETKVM_CLOUD_CORE_MESSAGE_TOO_LARGE: return "message_too_large";
    default: return "unknown";
    }
}

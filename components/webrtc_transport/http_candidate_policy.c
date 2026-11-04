/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "http_candidate_policy.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    size_t begin;
    size_t end;
} candidate_token_t;

static bool ascii_digit(char value)
{
    return value >= '0' && value <= '9';
}

static bool ascii_alpha(char value)
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z');
}

static char ascii_lower(char value)
{
    if (value >= 'A' && value <= 'Z') {
        return (char)(value + ('a' - 'A'));
    }
    return value;
}

static bool token_equal_ci(const char *candidate,
                           const candidate_token_t *token,
                           const char *expected)
{
    size_t expected_size = strlen(expected);
    if (token->end - token->begin != expected_size) {
        return false;
    }
    for (size_t i = 0; i < expected_size; ++i) {
        if (ascii_lower(candidate[token->begin + i]) !=
            ascii_lower(expected[i])) {
            return false;
        }
    }
    return true;
}

static bool parse_decimal(const char *value, size_t size,
                          uint32_t maximum, uint32_t *parsed)
{
    if (value == NULL || size == 0 || parsed == NULL) {
        return false;
    }
    uint32_t result = 0;
    for (size_t i = 0; i < size; ++i) {
        if (!ascii_digit(value[i])) {
            return false;
        }
        uint32_t digit = (uint32_t)(value[i] - '0');
        if (result > (maximum - digit) / 10U) {
            return false;
        }
        result = result * 10U + digit;
    }
    *parsed = result;
    return true;
}

bool webrtc_http_ipv4_parse(const char *text, webrtc_http_ipv4_t *address)
{
    if (text == NULL || address == NULL || text[0] == '\0') {
        return false;
    }
    webrtc_http_ipv4_t parsed = { 0 };
    const char *cursor = text;
    for (size_t part = 0; part < 4; ++part) {
        const char *begin = cursor;
        while (ascii_digit(*cursor)) {
            ++cursor;
        }
        uint32_t octet = 0;
        if (!parse_decimal(begin, (size_t)(cursor - begin), 255U, &octet)) {
            return false;
        }
        parsed.octet[part] = (uint8_t)octet;
        if (part < 3) {
            if (*cursor != '.') {
                return false;
            }
            ++cursor;
        }
    }
    if (*cursor != '\0') {
        return false;
    }
    *address = parsed;
    return true;
}

bool webrtc_http_ipv4_format(const webrtc_http_ipv4_t *address,
                             char *output, size_t output_size)
{
    if (address == NULL || output == NULL || output_size == 0) {
        return false;
    }
    int written = snprintf(output, output_size, "%u.%u.%u.%u",
                           (unsigned)address->octet[0],
                           (unsigned)address->octet[1],
                           (unsigned)address->octet[2],
                           (unsigned)address->octet[3]);
    return written > 0 && (size_t)written < output_size;
}

bool webrtc_http_ipv4_equal(const webrtc_http_ipv4_t *left,
                            const webrtc_http_ipv4_t *right)
{
    return left != NULL && right != NULL &&
           memcmp(left->octet, right->octet, sizeof(left->octet)) == 0;
}

static bool ipv4_same_24(const webrtc_http_ipv4_t *left, const webrtc_http_ipv4_t *right)
{
    return memcmp(left->octet, right->octet, 3U) == 0;
}

static bool ipv4_tailnet(const webrtc_http_ipv4_t *address)
{
    return address && address->octet[0] == 100U &&
           (address->octet[1] & 0xc0U) == 64U;
}

bool webrtc_http_candidate_policy_for_device(
    const webrtc_http_candidate_policy_t *policy,
    webrtc_http_candidate_policy_t *device_policy)
{
    if (!policy || !device_policy || !policy->active || !policy->rewrite_enabled ||
        !ipv4_tailnet(&policy->owner) || !ipv4_tailnet(&policy->device)) return false;
    *device_policy = *policy;
    device_policy->owner = policy->device;
    device_policy->candidate_count = 0;
    return true;
}

bool webrtc_http_ipv4_private(const webrtc_http_ipv4_t *address)
{
    if (address == NULL) {
        return false;
    }
    const uint8_t *o = address->octet;
    return o[0] == 10U ||
           (o[0] == 172U && (o[1] & 0xF0U) == 16U) ||
           (o[0] == 192U && o[1] == 168U) ||
           (o[0] == 169U && o[1] == 254U);
}

void webrtc_http_candidate_policy_begin(
    webrtc_http_candidate_policy_t *policy, uint32_t session_id,
    const webrtc_http_ipv4_t *owner, const webrtc_http_ipv4_t *device)
{
    if (policy == NULL) {
        return;
    }
    memset(policy, 0, sizeof(*policy));
    policy->active = true;
    policy->session_id = session_id;
    if (owner != NULL) {
        policy->owner_bound = true;
        policy->owner = *owner;
    }
    /* A browser on the same private network, including another subnet or
     * VLAN behind a router, reaches the device without NAT, so the address
     * of its signaling socket is also the address of its ICE candidates.
     * So does a browser on the device's own /24, whatever its range (for
     * example a LAN numbered from 100.64.0.0/10). */
    if (owner != NULL && device != NULL &&
        ((webrtc_http_ipv4_private(owner) && webrtc_http_ipv4_private(device)) ||
         ipv4_same_24(owner, device) ||
         (ipv4_tailnet(owner) && ipv4_tailnet(device)))) {
        policy->rewrite_enabled = true;
        policy->device = *device;
    }
}

void webrtc_http_candidate_policy_clear(
    webrtc_http_candidate_policy_t *policy, uint32_t session_id)
{
    if (policy == NULL || !policy->active || policy->session_id != session_id) {
        return;
    }
    memset(policy, 0, sizeof(*policy));
}

webrtc_http_owner_result_t webrtc_http_candidate_policy_authorize(
    const webrtc_http_candidate_policy_t *policy, uint32_t session_id,
    const webrtc_http_ipv4_t *request_peer)
{
    if (policy == NULL || !policy->active || policy->session_id != session_id ||
        !policy->owner_bound) {
        return WEBRTC_HTTP_OWNER_ALLOW;
    }
    if (request_peer == NULL) {
        return WEBRTC_HTTP_OWNER_PEER_REQUIRED;
    }
    if (!webrtc_http_ipv4_equal(&policy->owner, request_peer)) {
        return WEBRTC_HTTP_OWNER_PEER_MISMATCH;
    }
    return WEBRTC_HTTP_OWNER_ALLOW;
}

static bool candidate_tokens(const char *candidate, size_t candidate_size,
                             candidate_token_t tokens[8])
{
    if (candidate == NULL || candidate_size == 0 || candidate[0] == ' ' ||
        candidate[candidate_size - 1U] == ' ') {
        return false;
    }
    for (size_t i = 0; i < candidate_size; ++i) {
        unsigned char value = (unsigned char)candidate[i];
        if (value < 0x20U || value > 0x7eU) {
            return false;
        }
    }

    size_t cursor = 0;
    for (size_t token_index = 0; token_index < 8U; ++token_index) {
        while (cursor < candidate_size && candidate[cursor] == ' ') {
            ++cursor;
        }
        if (cursor == candidate_size) {
            return false;
        }
        tokens[token_index].begin = cursor;
        while (cursor < candidate_size && candidate[cursor] != ' ') {
            ++cursor;
        }
        tokens[token_index].end = cursor;
    }
    return true;
}

static bool valid_foundation(const char *candidate,
                             const candidate_token_t *token)
{
    static const char prefix[] = "candidate:";
    size_t size = token->end - token->begin;
    if (token->begin != 0 || size <= sizeof(prefix) - 1U ||
        size > sizeof(prefix) - 1U + 32U ||
        memcmp(candidate, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }
    for (size_t i = sizeof(prefix) - 1U; i < size; ++i) {
        char value = candidate[i];
        if (!ascii_alpha(value) && !ascii_digit(value) &&
            value != '+' && value != '/') {
            return false;
        }
    }
    return true;
}

static bool valid_ipv4_token(const char *candidate,
                             const candidate_token_t *token)
{
    size_t size = token->end - token->begin;
    if (size == 0 || size > 15U) {
        return false;
    }
    char address[16];
    memcpy(address, candidate + token->begin, size);
    address[size] = '\0';
    webrtc_http_ipv4_t parsed;
    return webrtc_http_ipv4_parse(address, &parsed);
}

static bool valid_mdns_token(const char *candidate,
                             const candidate_token_t *token)
{
    static const char suffix[] = ".local";
    size_t size = token->end - token->begin;
    size_t suffix_size = sizeof(suffix) - 1U;
    if (size <= suffix_size || size > 253U) {
        return false;
    }
    for (size_t i = 0; i < suffix_size; ++i) {
        if (ascii_lower(candidate[token->end - suffix_size + i]) != suffix[i]) {
            return false;
        }
    }

    size_t label_size = 0;
    bool label_starts_with_hyphen = false;
    for (size_t i = token->begin; i < token->end; ++i) {
        char value = candidate[i];
        if (value == '.') {
            if (label_size == 0 || label_size > 63U ||
                label_starts_with_hyphen || candidate[i - 1U] == '-') {
                return false;
            }
            label_size = 0;
            label_starts_with_hyphen = false;
            continue;
        }
        if (!ascii_alpha(value) && !ascii_digit(value) && value != '-') {
            return false;
        }
        if (label_size == 0) {
            label_starts_with_hyphen = value == '-';
        }
        ++label_size;
    }
    return label_size != 0 && label_size <= 63U &&
           !label_starts_with_hyphen && candidate[token->end - 1U] != '-';
}

static bool valid_candidate(const char *candidate, size_t candidate_size,
                            candidate_token_t tokens[8], uint16_t *port)
{
    if (!candidate_tokens(candidate, candidate_size, tokens) ||
        !valid_foundation(candidate, &tokens[0]) ||
        tokens[1].end - tokens[1].begin != 1U ||
        candidate[tokens[1].begin] != '1' ||
        !token_equal_ci(candidate, &tokens[2], "udp") ||
        !token_equal_ci(candidate, &tokens[6], "typ") ||
        !token_equal_ci(candidate, &tokens[7], "host")) {
        return false;
    }
    uint32_t priority = 0;
    uint32_t parsed_port = 0;
    if (!parse_decimal(candidate + tokens[3].begin,
                       tokens[3].end - tokens[3].begin, UINT32_MAX,
                       &priority) ||
        priority == 0U ||
        !parse_decimal(candidate + tokens[5].begin,
                       tokens[5].end - tokens[5].begin, 65535U,
                       &parsed_port) ||
        parsed_port == 0U) {
        return false;
    }
    if (!valid_ipv4_token(candidate, &tokens[4]) &&
        !valid_mdns_token(candidate, &tokens[4])) {
        return false;
    }
    *port = (uint16_t)parsed_port;
    return true;
}

webrtc_http_candidate_result_t webrtc_http_candidate_policy_rewrite(
    webrtc_http_candidate_policy_t *policy, uint32_t session_id,
    const char *candidate, size_t candidate_size,
    char *output, size_t output_size)
{
    if (policy == NULL || !policy->active || policy->session_id != session_id ||
        !policy->owner_bound || !policy->rewrite_enabled) {
        return WEBRTC_HTTP_CANDIDATE_POLICY_UNAVAILABLE;
    }
    if (candidate_size == 0 || candidate_size > WEBRTC_HTTP_CANDIDATE_MAX) {
        return WEBRTC_HTTP_CANDIDATE_TOO_LONG;
    }
    candidate_token_t tokens[8];
    uint16_t port = 0;
    if (!valid_candidate(candidate, candidate_size, tokens, &port)) {
        return WEBRTC_HTTP_CANDIDATE_INVALID;
    }
    bool duplicate = false;
    for (unsigned i = 0; i < policy->candidate_count; ++i) {
        if (policy->candidate_ports[i] == port) {
            duplicate = true;
            break;
        }
    }
    if (!duplicate && policy->candidate_count >= WEBRTC_HTTP_CANDIDATE_LIMIT) {
        return WEBRTC_HTTP_CANDIDATE_LIMIT_REACHED;
    }

    char owner[16];
    if (!webrtc_http_ipv4_format(&policy->owner, owner, sizeof(owner))) {
        return WEBRTC_HTTP_CANDIDATE_INVALID;
    }
    size_t owner_size = strlen(owner);
    size_t address_size = tokens[4].end - tokens[4].begin;
    size_t rewritten_size = candidate_size - address_size + owner_size;
    if (output == NULL || output_size <= rewritten_size) {
        return WEBRTC_HTTP_CANDIDATE_OUTPUT_TOO_SMALL;
    }
    memcpy(output, candidate, tokens[4].begin);
    memcpy(output + tokens[4].begin, owner, owner_size);
    memcpy(output + tokens[4].begin + owner_size,
           candidate + tokens[4].end,
           candidate_size - tokens[4].end);
    output[rewritten_size] = '\0';
    if (!duplicate) {
        policy->candidate_ports[policy->candidate_count++] = port;
    }
    return WEBRTC_HTTP_CANDIDATE_OK;
}

webrtc_http_candidate_result_t webrtc_http_candidate_policy_rewrite_sdp(
    webrtc_http_candidate_policy_t *policy, uint32_t session_id,
    const char *sdp, size_t sdp_size, char *output, size_t output_size)
{
    if (!sdp || !output || !output_size) {
        return WEBRTC_HTTP_CANDIDATE_OUTPUT_TOO_SMALL;
    }
    size_t used = 0;
    for (size_t begin = 0; begin < sdp_size;) {
        size_t end = begin;
        while (end < sdp_size && sdp[end] != '\n') ++end;
        size_t next = end < sdp_size ? end + 1U : end;
        if (end > begin && sdp[end - 1U] == '\r') --end;
        const char *line = sdp + begin;
        size_t length = end - begin;
        char rewritten[WEBRTC_HTTP_CANDIDATE_REWRITTEN_MAX];
        bool candidate = length >= 12U && memcmp(line, "a=candidate:", 12U) == 0;
        if (candidate) {
            webrtc_http_candidate_result_t result = webrtc_http_candidate_policy_rewrite(
                policy, session_id, line + 2U, length - 2U,
                rewritten, sizeof(rewritten));
            /* Preserve the rest of the offer when one optional candidate
             * cannot be admitted by the local address/size/count policy. */
            if (result != WEBRTC_HTTP_CANDIDATE_OK) {
                begin = next;
                continue;
            }
            line = rewritten;
            length = strlen(line);
        }
        size_t prefix = candidate ? 2U : 0U;
        if (length + prefix + 2U >= output_size - used) {
            return WEBRTC_HTTP_CANDIDATE_OUTPUT_TOO_SMALL;
        }
        if (candidate) {
            memcpy(output + used, "a=", 2U);
            used += 2U;
        }
        memcpy(output + used, line, length);
        used += length;
        memcpy(output + used, "\r\n", 2U);
        used += 2U;
        begin = next;
    }
    output[used] = '\0';
    return WEBRTC_HTTP_CANDIDATE_OK;
}

webrtc_http_device_signal_t webrtc_http_candidate_policy_rewrite_device_signal(
    webrtc_http_candidate_policy_t *device_policy, uint32_t session_id, bool sdp,
    const char *signal, size_t signal_size, char *output, size_t output_size)
{
    if (signal == NULL || output == NULL) {
        return WEBRTC_HTTP_DEVICE_SIGNAL_FAIL;
    }
    if (sdp) {
        return webrtc_http_candidate_policy_rewrite_sdp(
                   device_policy, session_id, signal, signal_size, output,
                   output_size) == WEBRTC_HTTP_CANDIDATE_OK ?
            WEBRTC_HTTP_DEVICE_SIGNAL_SEND : WEBRTC_HTTP_DEVICE_SIGNAL_FAIL;
    }
    if (signal_size >= 2U && memcmp(signal, "a=", 2U) == 0) {
        signal += 2U;
        signal_size -= 2U;
    }
    while (signal_size != 0U &&
           (signal[signal_size - 1U] == '\r' || signal[signal_size - 1U] == '\n')) {
        --signal_size;
    }
    switch (webrtc_http_candidate_policy_rewrite(
                device_policy, session_id, signal, signal_size, output, output_size)) {
    case WEBRTC_HTTP_CANDIDATE_OK:
        return WEBRTC_HTTP_DEVICE_SIGNAL_SEND;
    case WEBRTC_HTTP_CANDIDATE_OUTPUT_TOO_SMALL:
    case WEBRTC_HTTP_CANDIDATE_POLICY_UNAVAILABLE:
        return WEBRTC_HTTP_DEVICE_SIGNAL_FAIL;
    default:
        return WEBRTC_HTTP_DEVICE_SIGNAL_SKIP;
    }
}

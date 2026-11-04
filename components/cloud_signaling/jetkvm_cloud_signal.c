#include "jetkvm_cloud_signal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "mbedtls/base64.h"

typedef enum {
    JSON_SAFETY_OK = 0,
    JSON_SAFETY_INVALID,
    JSON_SAFETY_TOO_DEEP,
} json_safety_result_t;

static bool size_add_overflow(size_t a, size_t b, size_t *result)
{
    if (a > SIZE_MAX - b) {
        return true;
    }
    *result = a + b;
    return false;
}

static bool size_multiply_overflow(size_t a, size_t b, size_t *result)
{
    if (a != 0 && b > SIZE_MAX / a) {
        return true;
    }
    *result = a * b;
    return false;
}

static bool valid_utf8(const uint8_t *data, size_t length)
{
    size_t i = 0;
    while (i < length) {
        const uint8_t first = data[i++];
        if (first <= 0x7f) {
            continue;
        }

        uint32_t codepoint = 0;
        size_t continuation_count = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            codepoint = first & 0x1fU;
            continuation_count = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            codepoint = first & 0x0fU;
            continuation_count = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            codepoint = first & 0x07U;
            continuation_count = 3;
        } else {
            return false;
        }

        if (continuation_count > length - i) {
            return false;
        }
        for (size_t j = 0; j < continuation_count; ++j) {
            const uint8_t next = data[i++];
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }

        if ((continuation_count == 1 && codepoint < 0x80U) ||
            (continuation_count == 2 && codepoint < 0x800U) ||
            (continuation_count == 3 && codepoint < 0x10000U) ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU) ||
            codepoint > 0x10ffffU) {
            return false;
        }
    }
    return true;
}

static bool is_zero_hex(char value)
{
    return value == '0';
}

static json_safety_result_t json_safety_check(const uint8_t *data,
                                               size_t length)
{
    if (memchr(data, '\0', length) != NULL || !valid_utf8(data, length)) {
        return JSON_SAFETY_INVALID;
    }

    bool in_string = false;
    bool escaped = false;
    size_t depth = 0;
    for (size_t i = 0; i < length; ++i) {
        const char value = (char)data[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (value == '\\') {
                if (i + 5 < length && data[i + 1] == 'u' &&
                    is_zero_hex((char)data[i + 2]) &&
                    is_zero_hex((char)data[i + 3]) &&
                    is_zero_hex((char)data[i + 4]) &&
                    is_zero_hex((char)data[i + 5])) {
                    /* cJSON strings are NUL-terminated and cannot preserve it. */
                    return JSON_SAFETY_INVALID;
                }
                escaped = true;
            } else if (value == '"') {
                in_string = false;
            }
            continue;
        }

        if (value == '"') {
            in_string = true;
        } else if (value == '{' || value == '[') {
            if (++depth > JETKVM_CLOUD_SIGNAL_MAX_JSON_DEPTH) {
                return JSON_SAFETY_TOO_DEEP;
            }
        } else if ((value == '}' || value == ']') && depth > 0) {
            --depth;
        }
    }
    return JSON_SAFETY_OK;
}

static jetkvm_cloud_signal_status_t parse_json_document(
    const uint8_t *data, size_t length, size_t maximum_length, cJSON **root)
{
    *root = NULL;
    if (length == 0) {
        return JETKVM_CLOUD_SIGNAL_INVALID_JSON;
    }
    if (length > maximum_length) {
        return JETKVM_CLOUD_SIGNAL_MESSAGE_TOO_LARGE;
    }

    const json_safety_result_t safety = json_safety_check(data, length);
    if (safety == JSON_SAFETY_TOO_DEEP) {
        return JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED;
    }
    if (safety != JSON_SAFETY_OK) {
        return JETKVM_CLOUD_SIGNAL_INVALID_JSON;
    }

    size_t allocation_size = 0;
    if (size_add_overflow(length, 1, &allocation_size)) {
        return JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED;
    }
    char *copy = cJSON_malloc(allocation_size);
    if (copy == NULL) {
        return JETKVM_CLOUD_SIGNAL_NO_MEMORY;
    }
    memcpy(copy, data, length);
    copy[length] = '\0';

    const char *parse_end = NULL;
    cJSON *parsed = cJSON_ParseWithLengthOpts(copy, allocation_size,
                                              &parse_end, true);
    if (parsed == NULL || parse_end != copy + length) {
        cJSON_Delete(parsed);
        cJSON_free(copy);
        return JETKVM_CLOUD_SIGNAL_INVALID_JSON;
    }
    cJSON_free(copy);
    *root = parsed;
    return JETKVM_CLOUD_SIGNAL_OK;
}

static size_t object_key_count(const cJSON *object, const char *key,
                               const cJSON **item)
{
    size_t count = 0;
    *item = NULL;
    for (const cJSON *child = object->child; child != NULL;
         child = child->next) {
        if (child->string != NULL && strcmp(child->string, key) == 0) {
            if (count == 0) {
                *item = child;
            }
            ++count;
        }
    }
    return count;
}

static jetkvm_cloud_signal_status_t require_unique_field(
    const cJSON *object, const char *key, const cJSON **item,
    jetkvm_cloud_signal_status_t invalid_status)
{
    if (object_key_count(object, key, item) != 1) {
        return invalid_status;
    }
    return JETKVM_CLOUD_SIGNAL_OK;
}

static jetkvm_cloud_signal_status_t optional_unique_field(
    const cJSON *object, const char *key, const cJSON **item,
    bool *is_present, jetkvm_cloud_signal_status_t invalid_status)
{
    const size_t count = object_key_count(object, key, item);
    if (count > 1) {
        return invalid_status;
    }
    *is_present = count == 1;
    return JETKVM_CLOUD_SIGNAL_OK;
}

static jetkvm_cloud_signal_status_t copy_string(
    const char *source, size_t maximum_length, bool allow_empty,
    jetkvm_cloud_signal_owned_string_t *destination)
{
    const size_t length = strlen(source);
    if ((!allow_empty && length == 0) || length > maximum_length) {
        return JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED;
    }

    size_t allocation_size = 0;
    if (size_add_overflow(length, 1, &allocation_size)) {
        return JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED;
    }
    char *copy = cJSON_malloc(allocation_size);
    if (copy == NULL) {
        return JETKVM_CLOUD_SIGNAL_NO_MEMORY;
    }
    memcpy(copy, source, allocation_size);
    destination->data = copy;
    destination->length = length;
    return JETKVM_CLOUD_SIGNAL_OK;
}

static void owned_string_free(jetkvm_cloud_signal_owned_string_t *value)
{
    cJSON_free(value->data);
    memset(value, 0, sizeof(*value));
}

void jetkvm_cloud_signal_offer_free(jetkvm_cloud_signal_offer_t *offer)
{
    if (offer == NULL) {
        return;
    }
    owned_string_free(&offer->sdp);
    owned_string_free(&offer->ip);
    owned_string_free(&offer->oidc_google);
    if (offer->ice_servers != NULL) {
        for (size_t i = 0; i < offer->ice_server_count; ++i) {
            owned_string_free(&offer->ice_servers[i]);
        }
    }
    cJSON_free(offer->ice_servers);
    memset(offer, 0, sizeof(*offer));
}

void jetkvm_cloud_signal_candidate_free(
    jetkvm_cloud_signal_candidate_t *candidate)
{
    if (candidate == NULL) {
        return;
    }
    owned_string_free(&candidate->candidate);
    owned_string_free(&candidate->sdp_mid);
    owned_string_free(&candidate->username_fragment);
    memset(candidate, 0, sizeof(*candidate));
}

static int base64_value(uint8_t value)
{
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '+') {
        return 62;
    }
    if (value == '/') {
        return 63;
    }
    return -1;
}

static bool strict_base64_decoded_size(const uint8_t *input,
                                       size_t input_length,
                                       size_t *decoded_length)
{
    if (input_length == 0 || input_length % 4 != 0) {
        return false;
    }

    size_t padding = 0;
    if (input[input_length - 1] == '=') {
        padding = 1;
        if (input[input_length - 2] == '=') {
            padding = 2;
        }
    }
    for (size_t i = 0; i < input_length - padding; ++i) {
        if (base64_value(input[i]) < 0) {
            return false;
        }
    }
    for (size_t i = input_length - padding; i < input_length; ++i) {
        if (input[i] != '=') {
            return false;
        }
    }

    if (padding == 1 &&
        (base64_value(input[input_length - 2]) & 0x03) != 0) {
        return false;
    }
    if (padding == 2 &&
        (base64_value(input[input_length - 3]) & 0x0f) != 0) {
        return false;
    }

    size_t groups_size = 0;
    if (size_multiply_overflow(input_length / 4, 3, &groups_size) ||
        groups_size < padding) {
        return false;
    }
    *decoded_length = groups_size - padding;
    return true;
}

static jetkvm_cloud_signal_status_t decode_base64_json(
    const char *encoded, uint8_t **decoded, size_t *decoded_length)
{
    *decoded = NULL;
    *decoded_length = 0;
    const size_t encoded_length = strlen(encoded);
    size_t expected_length = 0;
    if (!strict_base64_decoded_size((const uint8_t *)encoded, encoded_length,
                                    &expected_length)) {
        return JETKVM_CLOUD_SIGNAL_INVALID_BASE64;
    }
    if (expected_length == 0 ||
        expected_length > JETKVM_CLOUD_SIGNAL_MAX_SD_JSON_SIZE) {
        return JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED;
    }

    size_t allocation_size = 0;
    if (size_add_overflow(expected_length, 1, &allocation_size)) {
        return JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED;
    }
    uint8_t *plain = cJSON_malloc(allocation_size);
    if (plain == NULL) {
        return JETKVM_CLOUD_SIGNAL_NO_MEMORY;
    }

    size_t actual_length = 0;
    const int result = mbedtls_base64_decode(
        plain, expected_length, &actual_length,
        (const unsigned char *)encoded, encoded_length);
    if (result != 0 || actual_length != expected_length) {
        cJSON_free(plain);
        return JETKVM_CLOUD_SIGNAL_INVALID_BASE64;
    }
    plain[actual_length] = '\0';
    *decoded = plain;
    *decoded_length = actual_length;
    return JETKVM_CLOUD_SIGNAL_OK;
}

static jetkvm_cloud_signal_status_t validate_envelope(
    const cJSON *root, const char *expected_type, const cJSON **data)
{
    if (!cJSON_IsObject(root)) {
        return JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE;
    }

    const cJSON *type = NULL;
    jetkvm_cloud_signal_status_t status = require_unique_field(
        root, "type", &type, JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    if (status != JETKVM_CLOUD_SIGNAL_OK || !cJSON_IsString(type) ||
        strcmp(type->valuestring, expected_type) != 0) {
        return JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE;
    }

    status = require_unique_field(root, "data", data,
                                  JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    if (status != JETKVM_CLOUD_SIGNAL_OK || !cJSON_IsObject(*data)) {
        return JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE;
    }
    return JETKVM_CLOUD_SIGNAL_OK;
}

static jetkvm_cloud_signal_status_t parse_session_offer(
    const char *encoded, jetkvm_cloud_signal_owned_string_t *sdp)
{
    uint8_t *decoded = NULL;
    size_t decoded_length = 0;
    jetkvm_cloud_signal_status_t status = decode_base64_json(
        encoded, &decoded, &decoded_length);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        return status;
    }

    cJSON *session = NULL;
    status = parse_json_document(decoded, decoded_length,
                                 JETKVM_CLOUD_SIGNAL_MAX_SD_JSON_SIZE,
                                 &session);
    cJSON_free(decoded);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        return status == JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED ? status :
            JETKVM_CLOUD_SIGNAL_INVALID_SESSION_DESCRIPTION;
    }
    if (!cJSON_IsObject(session)) {
        cJSON_Delete(session);
        return JETKVM_CLOUD_SIGNAL_INVALID_SESSION_DESCRIPTION;
    }

    const cJSON *type = NULL;
    const cJSON *sdp_item = NULL;
    status = require_unique_field(
        session, "type", &type,
        JETKVM_CLOUD_SIGNAL_INVALID_SESSION_DESCRIPTION);
    if (status == JETKVM_CLOUD_SIGNAL_OK) {
        status = require_unique_field(
            session, "sdp", &sdp_item,
            JETKVM_CLOUD_SIGNAL_INVALID_SESSION_DESCRIPTION);
    }
    if (status != JETKVM_CLOUD_SIGNAL_OK || !cJSON_IsString(type) ||
        strcmp(type->valuestring, "offer") != 0 ||
        !cJSON_IsString(sdp_item)) {
        cJSON_Delete(session);
        return JETKVM_CLOUD_SIGNAL_INVALID_SESSION_DESCRIPTION;
    }

    status = copy_string(sdp_item->valuestring,
                         JETKVM_CLOUD_SIGNAL_MAX_SDP_SIZE, false, sdp);
    cJSON_Delete(session);
    return status;
}

jetkvm_cloud_signal_status_t jetkvm_cloud_signal_parse_offer(
    const uint8_t *message, size_t message_length,
    jetkvm_cloud_signal_offer_t *offer)
{
    if (offer == NULL) {
        return JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT;
    }
    memset(offer, 0, sizeof(*offer));
    if (message == NULL) {
        return JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT;
    }
    jetkvm_cloud_signal_offer_t parsed = { 0 };

    cJSON *root = NULL;
    jetkvm_cloud_signal_status_t status = parse_json_document(
        message, message_length, JETKVM_CLOUD_SIGNAL_MAX_MESSAGE_SIZE, &root);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        return status;
    }

    const cJSON *data = NULL;
    status = validate_envelope(root, "offer", &data);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        goto cleanup;
    }

    const cJSON *sd = NULL;
    status = require_unique_field(data, "sd", &sd,
                                  JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    if (status != JETKVM_CLOUD_SIGNAL_OK || !cJSON_IsString(sd)) {
        status = JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE;
        goto cleanup;
    }
    status = parse_session_offer(sd->valuestring, &parsed.sdp);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        goto cleanup;
    }

    const cJSON *ip = NULL;
    status = optional_unique_field(data, "ip", &ip, &parsed.has_ip,
                                   JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        goto cleanup;
    }
    if (parsed.has_ip) {
        if (!cJSON_IsString(ip)) {
            status = JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE;
            goto cleanup;
        }
        status = copy_string(ip->valuestring, JETKVM_CLOUD_SIGNAL_MAX_IP_SIZE,
                             true, &parsed.ip);
        if (status != JETKVM_CLOUD_SIGNAL_OK) {
            goto cleanup;
        }
    }

    const cJSON *oidc = NULL;
    status = optional_unique_field(data, "OidcGoogle", &oidc,
                                   &parsed.has_oidc_google,
                                   JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        goto cleanup;
    }
    if (parsed.has_oidc_google) {
        if (!cJSON_IsString(oidc)) {
            status = JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE;
            goto cleanup;
        }
        status = copy_string(oidc->valuestring,
                             JETKVM_CLOUD_SIGNAL_MAX_OIDC_GOOGLE_SIZE, true,
                             &parsed.oidc_google);
        if (status != JETKVM_CLOUD_SIGNAL_OK) {
            goto cleanup;
        }
    }

    const cJSON *ice_servers = NULL;
    status = optional_unique_field(data, "iceServers", &ice_servers,
                                   &parsed.has_ice_servers,
                                   JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        goto cleanup;
    }
    if (parsed.has_ice_servers) {
        if (!cJSON_IsArray(ice_servers)) {
            status = JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE;
            goto cleanup;
        }
        for (const cJSON *item = ice_servers->child; item != NULL;
             item = item->next) {
            if (++parsed.ice_server_count >
                JETKVM_CLOUD_SIGNAL_MAX_ICE_SERVERS) {
                status = JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED;
                goto cleanup;
            }
        }
        if (parsed.ice_server_count > 0) {
            const size_t ice_server_bytes = parsed.ice_server_count *
                sizeof(*parsed.ice_servers);
            parsed.ice_servers = cJSON_malloc(ice_server_bytes);
            if (parsed.ice_servers == NULL) {
                status = JETKVM_CLOUD_SIGNAL_NO_MEMORY;
                goto cleanup;
            }
            memset(parsed.ice_servers, 0, ice_server_bytes);
        }
        size_t index = 0;
        for (const cJSON *item = ice_servers->child; item != NULL;
             item = item->next, ++index) {
            if (!cJSON_IsString(item)) {
                status = JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE;
                goto cleanup;
            }
            status = copy_string(
                item->valuestring, JETKVM_CLOUD_SIGNAL_MAX_ICE_SERVER_SIZE,
                false, &parsed.ice_servers[index]);
            if (status != JETKVM_CLOUD_SIGNAL_OK) {
                goto cleanup;
            }
        }
    }

cleanup:
    cJSON_Delete(root);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        jetkvm_cloud_signal_offer_free(&parsed);
        return status;
    }
    *offer = parsed;
    return JETKVM_CLOUD_SIGNAL_OK;
}

jetkvm_cloud_signal_status_t jetkvm_cloud_signal_parse_candidate(
    const uint8_t *message, size_t message_length,
    jetkvm_cloud_signal_candidate_t *candidate)
{
    if (candidate == NULL) {
        return JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT;
    }
    memset(candidate, 0, sizeof(*candidate));
    if (message == NULL) {
        return JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT;
    }
    jetkvm_cloud_signal_candidate_t parsed = { 0 };

    cJSON *root = NULL;
    jetkvm_cloud_signal_status_t status = parse_json_document(
        message, message_length, JETKVM_CLOUD_SIGNAL_MAX_MESSAGE_SIZE, &root);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        return status;
    }

    const cJSON *data = NULL;
    status = validate_envelope(root, "new-ice-candidate", &data);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        goto cleanup;
    }

    const cJSON *candidate_item = NULL;
    const cJSON *sdp_mid = NULL;
    const cJSON *sdp_mline_index = NULL;
    status = require_unique_field(data, "candidate", &candidate_item,
                                  JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    if (status == JETKVM_CLOUD_SIGNAL_OK) {
        status = require_unique_field(data, "sdpMid", &sdp_mid,
                                      JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    }
    if (status == JETKVM_CLOUD_SIGNAL_OK) {
        status = require_unique_field(data, "sdpMLineIndex", &sdp_mline_index,
                                      JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    }
    if (status != JETKVM_CLOUD_SIGNAL_OK ||
        !cJSON_IsString(candidate_item) || !cJSON_IsString(sdp_mid) ||
        !cJSON_IsNumber(sdp_mline_index)) {
        status = JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE;
        goto cleanup;
    }

    const double line_index = sdp_mline_index->valuedouble;
    if (!isfinite(line_index) || line_index < 0.0 || line_index > 65535.0 ||
        floor(line_index) != line_index) {
        status = JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE;
        goto cleanup;
    }
    parsed.sdp_mline_index = (uint16_t)line_index;
    status = copy_string(candidate_item->valuestring,
                         JETKVM_CLOUD_SIGNAL_MAX_CANDIDATE_SIZE, false,
                         &parsed.candidate);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        goto cleanup;
    }
    status = copy_string(sdp_mid->valuestring,
                         JETKVM_CLOUD_SIGNAL_MAX_SDP_MID_SIZE, false,
                         &parsed.sdp_mid);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        goto cleanup;
    }

    const cJSON *username = NULL;
    status = optional_unique_field(data, "usernameFragment", &username,
                                   &parsed.has_username_fragment,
                                   JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        goto cleanup;
    }
    if (parsed.has_username_fragment) {
        if (cJSON_IsNull(username)) {
            parsed.has_username_fragment = false;
        } else if (!cJSON_IsString(username)) {
            status = JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE;
            goto cleanup;
        } else {
            status = copy_string(username->valuestring,
                                 JETKVM_CLOUD_SIGNAL_MAX_USERNAME_SIZE, false,
                                 &parsed.username_fragment);
        }
    }

cleanup:
    cJSON_Delete(root);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        jetkvm_cloud_signal_candidate_free(&parsed);
        return status;
    }
    *candidate = parsed;
    return JETKVM_CLOUD_SIGNAL_OK;
}

static jetkvm_cloud_signal_status_t copy_input_string(
    const char *source, size_t length, size_t maximum_length, bool allow_empty,
    char **copy)
{
    *copy = NULL;
    if (source == NULL || (!allow_empty && length == 0)) {
        return JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT;
    }
    if (length > maximum_length) {
        return JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED;
    }
    if (memchr(source, '\0', length) != NULL) {
        return JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT;
    }
    if (!valid_utf8((const uint8_t *)source, length)) {
        return JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT;
    }
    size_t allocation_size = 0;
    if (size_add_overflow(length, 1, &allocation_size)) {
        return JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED;
    }
    char *value = cJSON_malloc(allocation_size);
    if (value == NULL) {
        return JETKVM_CLOUD_SIGNAL_NO_MEMORY;
    }
    memcpy(value, source, length);
    value[length] = '\0';
    *copy = value;
    return JETKVM_CLOUD_SIGNAL_OK;
}

static jetkvm_cloud_signal_status_t emit_json_transactionally(
    cJSON *root, char *output, size_t output_capacity, size_t *output_length)
{
    char *rendered = cJSON_PrintUnformatted(root);
    if (rendered == NULL) {
        return JETKVM_CLOUD_SIGNAL_NO_MEMORY;
    }
    const size_t rendered_length = strlen(rendered);
    if (rendered_length > JETKVM_CLOUD_SIGNAL_MAX_MESSAGE_SIZE) {
        cJSON_free(rendered);
        return JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED;
    }

    size_t required_capacity = 0;
    if (size_add_overflow(rendered_length, 1, &required_capacity)) {
        cJSON_free(rendered);
        return JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED;
    }
    if (required_capacity > output_capacity) {
        cJSON_free(rendered);
        return JETKVM_CLOUD_SIGNAL_OUTPUT_TOO_SMALL;
    }
    memcpy(output, rendered, required_capacity);
    *output_length = rendered_length;
    cJSON_free(rendered);
    return JETKVM_CLOUD_SIGNAL_OK;
}

jetkvm_cloud_signal_status_t jetkvm_cloud_signal_encode_answer(
    const char *sdp, size_t sdp_length, char *output, size_t output_capacity,
    size_t *output_length)
{
    if (output_length == NULL) {
        return JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT;
    }
    *output_length = 0;
    if (sdp == NULL || output == NULL || output_capacity == 0) {
        return JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT;
    }

    char *sdp_copy = NULL;
    jetkvm_cloud_signal_status_t status = copy_input_string(
        sdp, sdp_length, JETKVM_CLOUD_SIGNAL_MAX_SDP_SIZE, false, &sdp_copy);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        return status;
    }

    cJSON *session = cJSON_CreateObject();
    if (session == NULL ||
        cJSON_AddStringToObject(session, "type", "answer") == NULL ||
        cJSON_AddStringToObject(session, "sdp", sdp_copy) == NULL) {
        cJSON_free(sdp_copy);
        cJSON_Delete(session);
        return JETKVM_CLOUD_SIGNAL_NO_MEMORY;
    }
    cJSON_free(sdp_copy);

    char *session_json = cJSON_PrintUnformatted(session);
    cJSON_Delete(session);
    if (session_json == NULL) {
        return JETKVM_CLOUD_SIGNAL_NO_MEMORY;
    }
    const size_t session_length = strlen(session_json);
    if (session_length > JETKVM_CLOUD_SIGNAL_MAX_SD_JSON_SIZE ||
        session_length > SIZE_MAX - 2) {
        cJSON_free(session_json);
        return JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED;
    }

    size_t base64_length = 0;
    if (size_multiply_overflow((session_length + 2) / 3, 4,
                               &base64_length)) {
        cJSON_free(session_json);
        return JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED;
    }
    size_t base64_capacity = 0;
    if (size_add_overflow(base64_length, 1, &base64_capacity)) {
        cJSON_free(session_json);
        return JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED;
    }
    char *base64 = cJSON_malloc(base64_capacity);
    if (base64 == NULL) {
        cJSON_free(session_json);
        return JETKVM_CLOUD_SIGNAL_NO_MEMORY;
    }
    size_t actual_base64_length = 0;
    const int base64_result = mbedtls_base64_encode(
        (unsigned char *)base64, base64_capacity, &actual_base64_length,
        (const unsigned char *)session_json, session_length);
    cJSON_free(session_json);
    if (base64_result != 0 || actual_base64_length != base64_length) {
        cJSON_free(base64);
        return JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED;
    }
    base64[base64_length] = '\0';

    cJSON *root = cJSON_CreateObject();
    if (root == NULL ||
        cJSON_AddStringToObject(root, "type", "answer") == NULL ||
        cJSON_AddStringToObject(root, "data", base64) == NULL) {
        cJSON_free(base64);
        cJSON_Delete(root);
        return JETKVM_CLOUD_SIGNAL_NO_MEMORY;
    }
    cJSON_free(base64);
    status = emit_json_transactionally(root, output, output_capacity,
                                       output_length);
    cJSON_Delete(root);
    return status;
}

jetkvm_cloud_signal_status_t jetkvm_cloud_signal_encode_candidate(
    const jetkvm_cloud_signal_candidate_view_t *candidate, char *output,
    size_t output_capacity, size_t *output_length)
{
    if (output_length == NULL) {
        return JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT;
    }
    *output_length = 0;
    if (candidate == NULL || output == NULL || output_capacity == 0) {
        return JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT;
    }

    char *candidate_copy = NULL;
    char *sdp_mid_copy = NULL;
    char *username_copy = NULL;
    jetkvm_cloud_signal_status_t status = copy_input_string(
        candidate->candidate, candidate->candidate_length,
        JETKVM_CLOUD_SIGNAL_MAX_CANDIDATE_SIZE, false, &candidate_copy);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        goto cleanup_strings;
    }
    status = copy_input_string(candidate->sdp_mid, candidate->sdp_mid_length,
                               JETKVM_CLOUD_SIGNAL_MAX_SDP_MID_SIZE, false,
                               &sdp_mid_copy);
    if (status != JETKVM_CLOUD_SIGNAL_OK) {
        goto cleanup_strings;
    }
    if (candidate->has_username_fragment) {
        status = copy_input_string(
            candidate->username_fragment,
            candidate->username_fragment_length,
            JETKVM_CLOUD_SIGNAL_MAX_USERNAME_SIZE, false, &username_copy);
        if (status != JETKVM_CLOUD_SIGNAL_OK) {
            goto cleanup_strings;
        }
    } else if (candidate->username_fragment != NULL ||
               candidate->username_fragment_length != 0) {
        status = JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT;
        goto cleanup_strings;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    if (root == NULL || data == NULL ||
        cJSON_AddStringToObject(root, "type", "new-ice-candidate") == NULL ||
        !cJSON_AddItemToObject(root, "data", data)) {
        cJSON_Delete(data);
        cJSON_Delete(root);
        status = JETKVM_CLOUD_SIGNAL_NO_MEMORY;
        goto cleanup_strings;
    }
    /* data is owned by root from here onward. */
    if (cJSON_AddStringToObject(data, "candidate", candidate_copy) == NULL ||
        cJSON_AddStringToObject(data, "sdpMid", sdp_mid_copy) == NULL ||
        cJSON_AddNumberToObject(data, "sdpMLineIndex",
                                candidate->sdp_mline_index) == NULL ||
        (candidate->has_username_fragment &&
         cJSON_AddStringToObject(data, "usernameFragment", username_copy) ==
             NULL) ||
        (!candidate->has_username_fragment &&
         cJSON_AddNullToObject(data, "usernameFragment") == NULL)) {
        cJSON_Delete(root);
        status = JETKVM_CLOUD_SIGNAL_NO_MEMORY;
        goto cleanup_strings;
    }

    status = emit_json_transactionally(root, output, output_capacity,
                                       output_length);
    cJSON_Delete(root);

cleanup_strings:
    cJSON_free(candidate_copy);
    cJSON_free(sdp_mid_copy);
    cJSON_free(username_copy);
    return status;
}

const char *jetkvm_cloud_signal_status_name(
    jetkvm_cloud_signal_status_t status)
{
    switch (status) {
    case JETKVM_CLOUD_SIGNAL_OK:
        return "ok";
    case JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT:
        return "invalid_argument";
    case JETKVM_CLOUD_SIGNAL_MESSAGE_TOO_LARGE:
        return "message_too_large";
    case JETKVM_CLOUD_SIGNAL_INVALID_JSON:
        return "invalid_json";
    case JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE:
        return "invalid_envelope";
    case JETKVM_CLOUD_SIGNAL_INVALID_BASE64:
        return "invalid_base64";
    case JETKVM_CLOUD_SIGNAL_INVALID_SESSION_DESCRIPTION:
        return "invalid_session_description";
    case JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED:
        return "limit_exceeded";
    case JETKVM_CLOUD_SIGNAL_NO_MEMORY:
        return "no_memory";
    case JETKVM_CLOUD_SIGNAL_OUTPUT_TOO_SMALL:
        return "output_too_small";
    default:
        return "unknown";
    }
}

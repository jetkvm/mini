/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_oidc_core.h"

#include <limits.h>
#include <string.h>

#include "cJSON.h"

#define OIDC_MAX_HEADER_DECODED 1024U
#define OIDC_MAX_CLAIMS_DECODED 4096U
#define OIDC_MAX_JWKS_DOCUMENT_KEYS 16U
#define OIDC_MIN_TRUSTED_UNIX_TIME 1577836800LL
#define OIDC_MAX_CLOCK_SKEW_S 300U
#define OIDC_MAX_CACHE_TTL_S 86400U
#define OIDC_MAX_REFRESH_INTERVAL_S 300U
#define OIDC_MAX_UNIX_TIME 253402300799LL
#define OIDC_MAX_JSON_DEPTH 8U
#define OIDC_NOT_BEFORE_LEEWAY_S 300U

static size_t bounded_string_length(const char *value, size_t maximum)
{
    size_t length = 0U;
    if (!value) {
        return maximum;
    }
    while (length < maximum && value[length] != '\0') {
        ++length;
    }
    return length;
}

static void secure_clear(void *buffer, size_t length)
{
    volatile uint8_t *cursor = (volatile uint8_t *)buffer;
    while (cursor && length > 0U) {
        *cursor++ = 0U;
        --length;
    }
}

static int base64url_value(unsigned char value)
{
    if (value >= 'A' && value <= 'Z') {
        return (int)(value - 'A');
    }
    if (value >= 'a' && value <= 'z') {
        return 26 + (int)(value - 'a');
    }
    if (value >= '0' && value <= '9') {
        return 52 + (int)(value - '0');
    }
    if (value == '-') {
        return 62;
    }
    if (value == '_') {
        return 63;
    }
    return -1;
}

static bool base64url_decode(const char *source, size_t source_length,
                             uint8_t *destination, size_t capacity,
                             size_t *decoded_length)
{
    if (!source || !destination || !decoded_length || source_length == 0U ||
        source_length % 4U == 1U) {
        return false;
    }

    size_t required = (source_length / 4U) * 3U;
    if (source_length % 4U == 2U) {
        required += 1U;
    } else if (source_length % 4U == 3U) {
        required += 2U;
    }
    if (required > capacity) {
        return false;
    }

    uint32_t accumulator = 0U;
    unsigned bits = 0U;
    size_t output = 0U;
    int last_value = 0;
    for (size_t index = 0U; index < source_length; ++index) {
        int value = base64url_value((unsigned char)source[index]);
        if (value < 0) {
            return false;
        }
        last_value = value;
        accumulator = (accumulator << 6U) | (uint32_t)value;
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            destination[output++] =
                (uint8_t)(accumulator >> bits);
            if (bits == 0U) {
                accumulator = 0U;
            } else {
                accumulator &= (1U << bits) - 1U;
            }
        }
    }

    if ((source_length % 4U == 2U && (last_value & 0x0f) != 0) ||
        (source_length % 4U == 3U && (last_value & 0x03) != 0) ||
        output != required) {
        secure_clear(destination, output);
        return false;
    }
    *decoded_length = output;
    return true;
}

static bool valid_utf8(const uint8_t *text, size_t length)
{
    for (size_t index = 0U; index < length;) {
        uint8_t first = text[index++];
        if (first < 0x80U) {
            continue;
        }
        size_t continuation_count = 0U;
        uint8_t second_minimum = 0x80U;
        uint8_t second_maximum = 0xbfU;
        if (first >= 0xc2U && first <= 0xdfU) {
            continuation_count = 1U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            continuation_count = 2U;
            if (first == 0xe0U) {
                second_minimum = 0xa0U;
            } else if (first == 0xedU) {
                second_maximum = 0x9fU;
            }
        } else if (first >= 0xf0U && first <= 0xf4U) {
            continuation_count = 3U;
            if (first == 0xf0U) {
                second_minimum = 0x90U;
            } else if (first == 0xf4U) {
                second_maximum = 0x8fU;
            }
        } else {
            return false;
        }
        if (length - index < continuation_count ||
            text[index] < second_minimum ||
            text[index] > second_maximum) {
            return false;
        }
        for (size_t offset = 1U; offset < continuation_count; ++offset) {
            if (text[index + offset] < 0x80U ||
                text[index + offset] > 0xbfU) {
                return false;
            }
        }
        index += continuation_count;
    }
    return true;
}

static int hexadecimal_value(uint8_t value)
{
    if (value >= '0' && value <= '9') {
        return (int)(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return 10 + (int)(value - 'a');
    }
    if (value >= 'A' && value <= 'F') {
        return 10 + (int)(value - 'A');
    }
    return -1;
}

static bool parse_unicode_escape(const uint8_t *json, size_t length,
                                 size_t *cursor, uint16_t *code_unit)
{
    if (!json || !cursor || !code_unit || length - *cursor < 4U) {
        return false;
    }
    uint16_t value = 0U;
    for (size_t offset = 0U; offset < 4U; ++offset) {
        int digit = hexadecimal_value(json[*cursor + offset]);
        if (digit < 0) {
            return false;
        }
        value = (uint16_t)((value << 4U) | (uint16_t)digit);
    }
    *cursor += 4U;
    *code_unit = value;
    return true;
}

static bool json_prescan(const uint8_t *json, size_t length,
                         size_t maximum_depth)
{
    if (!json || length == 0U || maximum_depth == 0U ||
        !valid_utf8(json, length)) {
        return false;
    }
    char nesting[OIDC_MAX_JSON_DEPTH];
    size_t depth = 0U;
    bool in_string = false;
    for (size_t cursor = 0U; cursor < length; ++cursor) {
        uint8_t current = json[cursor];
        if (current == 0U) {
            return false;
        }
        if (in_string) {
            if (current == '"') {
                in_string = false;
                continue;
            }
            if (current < 0x20U) {
                return false;
            }
            if (current != '\\') {
                continue;
            }
            if (++cursor >= length) {
                return false;
            }
            current = json[cursor];
            if (current == '"' || current == '\\' || current == '/' ||
                current == 'b' || current == 'f' || current == 'n' ||
                current == 'r' || current == 't') {
                continue;
            }
            if (current != 'u') {
                return false;
            }
            ++cursor;
            uint16_t first = 0U;
            if (!parse_unicode_escape(json, length, &cursor, &first) ||
                first == 0U) {
                return false;
            }
            --cursor;
            if (first >= 0xdc00U && first <= 0xdfffU) {
                return false;
            }
            if (first >= 0xd800U && first <= 0xdbffU) {
                if (length - (cursor + 1U) < 6U ||
                    json[cursor + 1U] != '\\' ||
                    json[cursor + 2U] != 'u') {
                    return false;
                }
                cursor += 3U;
                uint16_t second = 0U;
                if (!parse_unicode_escape(json, length, &cursor, &second) ||
                    second < 0xdc00U || second > 0xdfffU) {
                    return false;
                }
                --cursor;
            }
            continue;
        }

        if (current == '"') {
            in_string = true;
        } else if (current == '{' || current == '[') {
            if (depth >= maximum_depth || depth >= sizeof(nesting)) {
                return false;
            }
            nesting[depth++] = (char)current;
        } else if (current == '}' || current == ']') {
            if (depth == 0U ||
                (current == '}' && nesting[depth - 1U] != '{') ||
                (current == ']' && nesting[depth - 1U] != '[')) {
                return false;
            }
            --depth;
        } else if (current >= 0x80U ||
                   (current < 0x20U && current != ' ' &&
                    current != '\t' && current != '\r' &&
                    current != '\n')) {
            return false;
        }
    }
    return !in_string && depth == 0U;
}

static cJSON *parse_json_segment(const char *encoded, size_t encoded_length,
                                 size_t decoded_limit,
                                 uint8_t **decoded_out)
{
    if (!encoded || !decoded_out) {
        return NULL;
    }
    size_t maximum_decoded = (encoded_length / 4U) * 3U + 2U;
    if (maximum_decoded == 0U || maximum_decoded > decoded_limit) {
        return NULL;
    }
    uint8_t *decoded = cJSON_malloc(maximum_decoded + 1U);
    if (!decoded) {
        return NULL;
    }
    memset(decoded, 0, maximum_decoded + 1U);
    size_t decoded_length = 0U;
    if (!base64url_decode(encoded, encoded_length, decoded,
                          maximum_decoded, &decoded_length) ||
        decoded_length == 0U ||
        !json_prescan(decoded, decoded_length, OIDC_MAX_JSON_DEPTH)) {
        secure_clear(decoded, maximum_decoded + 1U);
        cJSON_free(decoded);
        return NULL;
    }
    decoded[decoded_length] = '\0';

    const char *parse_end = NULL;
    cJSON *json = cJSON_ParseWithLengthOpts((const char *)decoded,
                                            decoded_length + 1U,
                                            &parse_end, true);
    if (!json || !cJSON_IsObject(json) ||
        parse_end != (const char *)decoded + decoded_length) {
        cJSON_Delete(json);
        secure_clear(decoded, maximum_decoded + 1U);
        cJSON_free(decoded);
        return NULL;
    }
    *decoded_out = decoded;
    return json;
}

static cJSON *unique_object_item(const cJSON *object, const char *name,
                                 bool *duplicate)
{
    cJSON *found = NULL;
    cJSON *item = NULL;
    *duplicate = false;
    cJSON_ArrayForEach(item, object) {
        if (item->string && strcmp(item->string, name) == 0) {
            if (found) {
                *duplicate = true;
                return NULL;
            }
            found = item;
        }
    }
    return found;
}

static bool copy_json_string(const cJSON *item, char *destination,
                             size_t capacity, bool allow_empty)
{
    if (!cJSON_IsString(item) || !item->valuestring || !destination ||
        capacity == 0U) {
        return false;
    }
    size_t length = bounded_string_length(item->valuestring, capacity);
    if (length >= capacity || (!allow_empty && length == 0U)) {
        return false;
    }
    memcpy(destination, item->valuestring, length + 1U);
    return true;
}

static bool parse_positive_integer(const cJSON *item, int64_t *value)
{
    if (!cJSON_IsNumber(item) || !value) {
        return false;
    }
    double number = item->valuedouble;
    if (!(number >= 0.0) || number > (double)OIDC_MAX_UNIX_TIME) {
        return false;
    }
    int64_t integer = (int64_t)number;
    if ((double)integer != number) {
        return false;
    }
    *value = integer;
    return true;
}

static bool parse_header(cJSON *header, jetkvm_oidc_token_t *parsed)
{
    bool duplicate = false;
    cJSON *algorithm = unique_object_item(header, "alg", &duplicate);
    if (duplicate || !cJSON_IsString(algorithm) ||
        strcmp(algorithm->valuestring, "RS256") != 0) {
        return false;
    }
    cJSON *kid = unique_object_item(header, "kid", &duplicate);
    if (duplicate ||
        !copy_json_string(kid, parsed->kid, sizeof(parsed->kid), false)) {
        return false;
    }
    cJSON *type = unique_object_item(header, "typ", &duplicate);
    if (duplicate || (type && (!cJSON_IsString(type) ||
                               strcmp(type->valuestring, "JWT") != 0))) {
        return false;
    }
    cJSON *critical = unique_object_item(header, "crit", &duplicate);
    if (duplicate || critical) {
        return false;
    }
    cJSON *unencoded = unique_object_item(header, "b64", &duplicate);
    if (duplicate || unencoded) {
        return false;
    }
    return true;
}

static bool parse_audiences(const cJSON *audience,
                            jetkvm_oidc_token_t *parsed)
{
    if (cJSON_IsString(audience)) {
        if (!copy_json_string(audience, parsed->audiences[0],
                              sizeof(parsed->audiences[0]), false)) {
            return false;
        }
        parsed->audience_count = 1U;
        return true;
    }
    if (!cJSON_IsArray(audience)) {
        return false;
    }
    int count = cJSON_GetArraySize(audience);
    if (count <= 0 || count > (int)JETKVM_OIDC_MAX_AUDIENCES) {
        return false;
    }
    for (int index = 0; index < count; ++index) {
        cJSON *entry = cJSON_GetArrayItem(audience, index);
        if (!copy_json_string(entry, parsed->audiences[index],
                              sizeof(parsed->audiences[index]), false)) {
            return false;
        }
    }
    parsed->audience_count = (size_t)count;
    return true;
}

static bool parse_claims(cJSON *claims, jetkvm_oidc_token_t *parsed)
{
    bool duplicate = false;
    cJSON *issuer = unique_object_item(claims, "iss", &duplicate);
    if (duplicate ||
        !copy_json_string(issuer, parsed->issuer, sizeof(parsed->issuer),
                          false)) {
        return false;
    }
    cJSON *subject = unique_object_item(claims, "sub", &duplicate);
    if (duplicate ||
        !copy_json_string(subject, parsed->subject,
                          sizeof(parsed->subject), false)) {
        return false;
    }
    cJSON *audience = unique_object_item(claims, "aud", &duplicate);
    if (duplicate || !audience || !parse_audiences(audience, parsed)) {
        return false;
    }
    cJSON *expires = unique_object_item(claims, "exp", &duplicate);
    if (duplicate || !parse_positive_integer(expires,
                                              &parsed->expires_at_s)) {
        return false;
    }
    cJSON *issued = unique_object_item(claims, "iat", &duplicate);
    if (duplicate || !parse_positive_integer(issued,
                                              &parsed->issued_at_s)) {
        return false;
    }
    cJSON *not_before = unique_object_item(claims, "nbf", &duplicate);
    if (duplicate ||
        (not_before && !parse_positive_integer(not_before,
                                               &parsed->not_before_s))) {
        return false;
    }
    parsed->has_not_before = not_before != NULL;
    return true;
}

void jetkvm_oidc_core_default_config(jetkvm_oidc_core_config_t *config)
{
    if (!config) {
        return;
    }
    config->clock_skew_s = 60U;
    config->default_jwks_ttl_s = 3600U;
    config->max_jwks_ttl_s = OIDC_MAX_CACHE_TTL_S;
    config->minimum_refresh_interval_s = 30U;
}

jetkvm_oidc_status_t jetkvm_oidc_parse_token(
    const char *compact_token, size_t token_length,
    jetkvm_oidc_token_t *parsed)
{
    if (!compact_token || !parsed || token_length == 0U) {
        return JETKVM_OIDC_INVALID_ARGUMENT;
    }
    if (token_length > JETKVM_OIDC_MAX_TOKEN_BYTES) {
        return JETKVM_OIDC_INPUT_TOO_LARGE;
    }
    if (memchr(compact_token, '\0', token_length) != NULL) {
        return JETKVM_OIDC_MALFORMED_TOKEN;
    }
    memset(parsed, 0, sizeof(*parsed));

    const char *first_dot = memchr(compact_token, '.', token_length);
    if (!first_dot || first_dot == compact_token) {
        return JETKVM_OIDC_MALFORMED_TOKEN;
    }
    size_t remaining = token_length - (size_t)(first_dot - compact_token) - 1U;
    const char *second_dot = memchr(first_dot + 1, '.', remaining);
    if (!second_dot || second_dot == first_dot + 1 ||
        second_dot == compact_token + token_length - 1U ||
        memchr(second_dot + 1, '.',
               token_length - (size_t)(second_dot + 1 - compact_token))) {
        return JETKVM_OIDC_MALFORMED_TOKEN;
    }

    uint8_t *decoded = NULL;
    size_t header_length = (size_t)(first_dot - compact_token);
    cJSON *header = parse_json_segment(compact_token, header_length,
                                       OIDC_MAX_HEADER_DECODED, &decoded);
    if (!header) {
        return JETKVM_OIDC_MALFORMED_TOKEN;
    }
    bool valid_header = parse_header(header, parsed);
    cJSON_Delete(header);
    secure_clear(decoded, (header_length / 4U) * 3U + 3U);
    cJSON_free(decoded);
    if (!valid_header) {
        return JETKVM_OIDC_UNSUPPORTED_ALGORITHM;
    }

    size_t claims_encoded_length = (size_t)(second_dot - first_dot - 1);
    decoded = NULL;
    cJSON *claims = parse_json_segment(first_dot + 1,
                                       claims_encoded_length,
                                       OIDC_MAX_CLAIMS_DECODED, &decoded);
    if (!claims) {
        return JETKVM_OIDC_MALFORMED_TOKEN;
    }
    bool valid_claims = parse_claims(claims, parsed);
    cJSON_Delete(claims);
    secure_clear(decoded, (claims_encoded_length / 4U) * 3U + 3U);
    cJSON_free(decoded);
    if (!valid_claims) {
        secure_clear(parsed, sizeof(*parsed));
        return JETKVM_OIDC_MISSING_CLAIM;
    }

    const char *signature = second_dot + 1;
    size_t signature_encoded_length =
        token_length - (size_t)(signature - compact_token);
    if (!base64url_decode(signature, signature_encoded_length,
                          parsed->signature, sizeof(parsed->signature),
                          &parsed->signature_length) ||
        parsed->signature_length == 0U) {
        secure_clear(parsed, sizeof(*parsed));
        return JETKVM_OIDC_MALFORMED_TOKEN;
    }
    parsed->signing_input = (const uint8_t *)compact_token;
    parsed->signing_input_length = (size_t)(second_dot - compact_token);
    return JETKVM_OIDC_OK;
}

static bool valid_rsa_exponent(const uint8_t *exponent, size_t length)
{
    if (!exponent || length == 0U ||
        length > JETKVM_OIDC_MAX_RSA_EXPONENT_BYTES ||
        exponent[0] == 0U || (exponent[length - 1U] & 1U) == 0U) {
        return false;
    }
    bool greater_than_two = false;
    for (size_t index = 0U; index < length; ++index) {
        if (index + 1U < length || exponent[index] > 2U) {
            greater_than_two = true;
        }
    }
    return greater_than_two;
}

jetkvm_oidc_status_t jetkvm_oidc_parse_jwks(
    const char *jwks, size_t jwks_length,
    jetkvm_oidc_jwk_t *keys, size_t keys_capacity,
    size_t *key_count)
{
    if (!jwks || !keys || !key_count || keys_capacity == 0U) {
        return JETKVM_OIDC_INVALID_ARGUMENT;
    }
    *key_count = 0U;
    if (jwks_length == 0U || jwks_length > JETKVM_OIDC_MAX_JWKS_BYTES ||
        memchr(jwks, '\0', jwks_length)) {
        return jwks_length > JETKVM_OIDC_MAX_JWKS_BYTES
                   ? JETKVM_OIDC_INPUT_TOO_LARGE
                   : JETKVM_OIDC_INVALID_JWKS;
    }
    if (!json_prescan((const uint8_t *)jwks, jwks_length,
                      OIDC_MAX_JSON_DEPTH)) {
        return JETKVM_OIDC_INVALID_JWKS;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(jwks, jwks_length,
                                            &parse_end, false);
    /* cJSON stops at the closing brace; Google's document ends in "}\n". */
    while (parse_end && parse_end < jwks + jwks_length &&
           (*parse_end == ' ' || *parse_end == '\t' ||
            *parse_end == '\n' || *parse_end == '\r')) {
        ++parse_end;
    }
    if (!root || !cJSON_IsObject(root) || parse_end != jwks + jwks_length) {
        cJSON_Delete(root);
        return JETKVM_OIDC_INVALID_JWKS;
    }
    bool duplicate = false;
    cJSON *key_array = unique_object_item(root, "keys", &duplicate);
    int document_keys = key_array ? cJSON_GetArraySize(key_array) : 0;
    if (duplicate || !cJSON_IsArray(key_array) || document_keys <= 0 ||
        document_keys > (int)OIDC_MAX_JWKS_DOCUMENT_KEYS) {
        cJSON_Delete(root);
        return JETKVM_OIDC_INVALID_JWKS;
    }

    jetkvm_oidc_status_t status = JETKVM_OIDC_OK;
    size_t output = 0U;
    for (int index = 0; index < document_keys; ++index) {
        cJSON *item = cJSON_GetArrayItem(key_array, index);
        if (!cJSON_IsObject(item)) {
            status = JETKVM_OIDC_INVALID_JWKS;
            break;
        }
        cJSON *key_type = unique_object_item(item, "kty", &duplicate);
        if (duplicate || !cJSON_IsString(key_type)) {
            status = JETKVM_OIDC_INVALID_JWKS;
            break;
        }
        if (strcmp(key_type->valuestring, "RSA") != 0) {
            continue;
        }
        cJSON *use = unique_object_item(item, "use", &duplicate);
        if (duplicate || (use && !cJSON_IsString(use))) {
            status = JETKVM_OIDC_INVALID_JWKS;
            break;
        }
        if (use && strcmp(use->valuestring, "sig") != 0) {
            continue;
        }
        cJSON *algorithm = unique_object_item(item, "alg", &duplicate);
        if (duplicate || (algorithm && !cJSON_IsString(algorithm))) {
            status = JETKVM_OIDC_INVALID_JWKS;
            break;
        }
        if (algorithm && strcmp(algorithm->valuestring, "RS256") != 0) {
            continue;
        }
        if (output >= keys_capacity || output >= JETKVM_OIDC_MAX_KEYS) {
            status = JETKVM_OIDC_INVALID_JWKS;
            break;
        }

        jetkvm_oidc_jwk_t candidate = { 0 };
        cJSON *kid = unique_object_item(item, "kid", &duplicate);
        if (duplicate || !copy_json_string(kid, candidate.kid,
                                            sizeof(candidate.kid), false)) {
            status = JETKVM_OIDC_INVALID_JWKS;
            break;
        }
        cJSON *modulus = unique_object_item(item, "n", &duplicate);
        if (duplicate) {
            status = JETKVM_OIDC_INVALID_JWKS;
            break;
        }
        cJSON *exponent = unique_object_item(item, "e", &duplicate);
        if (duplicate || !cJSON_IsString(modulus) ||
            !cJSON_IsString(exponent) || !modulus->valuestring ||
            !exponent->valuestring ||
            !base64url_decode(modulus->valuestring,
                              strlen(modulus->valuestring),
                              candidate.modulus, sizeof(candidate.modulus),
                              &candidate.modulus_length) ||
            !base64url_decode(exponent->valuestring,
                              strlen(exponent->valuestring),
                              candidate.exponent, sizeof(candidate.exponent),
                              &candidate.exponent_length) ||
            candidate.modulus_length < 256U ||
            candidate.modulus[0] == 0U ||
            !valid_rsa_exponent(candidate.exponent,
                                candidate.exponent_length)) {
            secure_clear(&candidate, sizeof(candidate));
            status = JETKVM_OIDC_INVALID_JWKS;
            break;
        }
        for (size_t existing = 0U; existing < output; ++existing) {
            if (strcmp(keys[existing].kid, candidate.kid) == 0) {
                status = JETKVM_OIDC_INVALID_JWKS;
                break;
            }
        }
        if (status != JETKVM_OIDC_OK) {
            secure_clear(&candidate, sizeof(candidate));
            break;
        }
        keys[output++] = candidate;
        secure_clear(&candidate, sizeof(candidate));
    }
    cJSON_Delete(root);
    if (status != JETKVM_OIDC_OK || output == 0U) {
        secure_clear(keys, keys_capacity * sizeof(*keys));
        *key_count = 0U;
        return status == JETKVM_OIDC_OK ? JETKVM_OIDC_INVALID_JWKS : status;
    }
    *key_count = output;
    return JETKVM_OIDC_OK;
}

const jetkvm_oidc_jwk_t *jetkvm_oidc_find_key(
    const jetkvm_oidc_jwk_t *keys, size_t key_count,
    const char *kid)
{
    if (!keys || !kid || key_count > JETKVM_OIDC_MAX_KEYS) {
        return NULL;
    }
    for (size_t index = 0U; index < key_count; ++index) {
        if (strcmp(keys[index].kid, kid) == 0) {
            return &keys[index];
        }
    }
    return NULL;
}

jetkvm_oidc_status_t jetkvm_oidc_validate_claims(
    const jetkvm_oidc_token_t *token,
    jetkvm_oidc_validation_mode_t mode,
    const char *registration_client_id,
    int64_t now_s, uint32_t clock_skew_s,
    char *identity, size_t identity_capacity)
{
    if (!token || !identity || identity_capacity == 0U ||
        (mode != JETKVM_OIDC_REGISTRATION &&
         mode != JETKVM_OIDC_SESSION) ||
        clock_skew_s > OIDC_MAX_CLOCK_SKEW_S) {
        return JETKVM_OIDC_INVALID_ARGUMENT;
    }
    identity[0] = '\0';
    if (now_s < OIDC_MIN_TRUSTED_UNIX_TIME) {
        return JETKVM_OIDC_CLOCK_NOT_READY;
    }
    if (strcmp(token->issuer, JETKVM_OIDC_GOOGLE_ISSUER) != 0 &&
        strcmp(token->issuer, JETKVM_OIDC_GOOGLE_ISSUER_LEGACY) != 0) {
        return JETKVM_OIDC_INVALID_ISSUER;
    }
    if (token->audience_count == 0U ||
        token->audience_count > JETKVM_OIDC_MAX_AUDIENCES ||
        token->subject[0] == '\0') {
        return JETKVM_OIDC_MISSING_CLAIM;
    }
    if (token->expires_at_s < token->issued_at_s) {
        return JETKVM_OIDC_INVALID_TIME;
    }
    if (now_s > token->expires_at_s &&
        (uint64_t)(now_s - token->expires_at_s) > clock_skew_s) {
        return JETKVM_OIDC_TOKEN_EXPIRED;
    }
    if (token->issued_at_s > now_s &&
        (uint64_t)(token->issued_at_s - now_s) > clock_skew_s) {
        return JETKVM_OIDC_TOKEN_NOT_YET_VALID;
    }
    if (token->has_not_before && token->not_before_s > now_s &&
        (uint64_t)(token->not_before_s - now_s) >
            OIDC_NOT_BEFORE_LEEWAY_S) {
        return JETKVM_OIDC_TOKEN_NOT_YET_VALID;
    }

    if (mode == JETKVM_OIDC_REGISTRATION) {
        size_t client_length = bounded_string_length(
            registration_client_id, JETKVM_OIDC_MAX_AUDIENCE_BYTES + 1U);
        if (client_length == 0U ||
            client_length > JETKVM_OIDC_MAX_AUDIENCE_BYTES) {
            return JETKVM_OIDC_INVALID_AUDIENCE;
        }
        bool matched = false;
        for (size_t index = 0U; index < token->audience_count; ++index) {
            if (strcmp(token->audiences[index], registration_client_id) == 0) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return JETKVM_OIDC_INVALID_AUDIENCE;
        }
    }

    size_t audience_length = strlen(token->audiences[0]);
    size_t subject_length = strlen(token->subject);
    if (audience_length > SIZE_MAX - subject_length - 2U ||
        audience_length + subject_length + 2U > identity_capacity) {
        return JETKVM_OIDC_IDENTITY_TOO_LARGE;
    }
    memcpy(identity, token->audiences[0], audience_length);
    identity[audience_length] = ':';
    memcpy(identity + audience_length + 1U, token->subject,
           subject_length + 1U);
    return JETKVM_OIDC_OK;
}

static bool ascii_equal_ignore_case(const char *left, size_t left_length,
                                    const char *right)
{
    size_t right_length = strlen(right);
    if (left_length != right_length) {
        return false;
    }
    for (size_t index = 0U; index < left_length; ++index) {
        unsigned char a = (unsigned char)left[index];
        unsigned char b = (unsigned char)right[index];
        if (a >= 'A' && a <= 'Z') {
            a = (unsigned char)(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = (unsigned char)(b + ('a' - 'A'));
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool jetkvm_oidc_parse_cache_max_age(
    const char *cache_control, size_t cache_control_length,
    uint32_t *max_age_s)
{
    if (!cache_control || !max_age_s || cache_control_length == 0U ||
        cache_control_length > 512U) {
        return false;
    }
    bool found = false;
    uint32_t result = 0U;
    size_t cursor = 0U;
    while (cursor < cache_control_length) {
        while (cursor < cache_control_length &&
               (cache_control[cursor] == ' ' ||
                cache_control[cursor] == '\t' ||
                cache_control[cursor] == ',')) {
            ++cursor;
        }
        size_t directive_start = cursor;
        bool quoted = false;
        bool escaped = false;
        while (cursor < cache_control_length) {
            char current = cache_control[cursor];
            if (escaped) {
                escaped = false;
            } else if (quoted && current == '\\') {
                escaped = true;
            } else if (current == '"') {
                quoted = !quoted;
            } else if (current == ',' && !quoted) {
                break;
            }
            ++cursor;
        }
        if (quoted || escaped) {
            return false;
        }
        size_t directive_end = cursor;
        while (directive_end > directive_start &&
               (cache_control[directive_end - 1U] == ' ' ||
                cache_control[directive_end - 1U] == '\t')) {
            --directive_end;
        }
        if (cursor < cache_control_length) {
            ++cursor;
        }
        if (directive_end == directive_start) {
            continue;
        }

        size_t equals = directive_start;
        while (equals < directive_end && cache_control[equals] != '=') {
            ++equals;
        }
        size_t name_end = equals;
        while (name_end > directive_start &&
               (cache_control[name_end - 1U] == ' ' ||
                cache_control[name_end - 1U] == '\t')) {
            --name_end;
        }
        if (!ascii_equal_ignore_case(cache_control + directive_start,
                                     name_end - directive_start,
                                     "max-age")) {
            continue;
        }
        if (found || equals == directive_end) {
            return false;
        }

        size_t value_start = equals + 1U;
        while (value_start < directive_end &&
               (cache_control[value_start] == ' ' ||
                cache_control[value_start] == '\t')) {
            ++value_start;
        }
        size_t value_end = directive_end;
        while (value_end > value_start &&
               (cache_control[value_end - 1U] == ' ' ||
                cache_control[value_end - 1U] == '\t')) {
            --value_end;
        }
        if (value_end > value_start && cache_control[value_start] == '"') {
            if (value_end - value_start < 2U ||
                cache_control[value_end - 1U] != '"') {
                return false;
            }
            ++value_start;
            --value_end;
        }
        if (value_start == value_end) {
            return false;
        }
        uint64_t value = 0U;
        for (size_t index = value_start; index < value_end; ++index) {
            if (cache_control[index] < '0' ||
                cache_control[index] > '9') {
                return false;
            }
            value = value * 10U +
                    (uint64_t)(cache_control[index] - '0');
            if (value > UINT32_MAX) {
                return false;
            }
        }
        result = (uint32_t)value;
        found = true;
    }
    if (found) {
        *max_age_s = result;
    }
    return found;
}

static bool valid_core_config(const jetkvm_oidc_core_config_t *config)
{
    return config && config->clock_skew_s <= OIDC_MAX_CLOCK_SKEW_S &&
           config->default_jwks_ttl_s <= config->max_jwks_ttl_s &&
           config->max_jwks_ttl_s <= OIDC_MAX_CACHE_TTL_S &&
           config->minimum_refresh_interval_s > 0U &&
           config->minimum_refresh_interval_s <=
               OIDC_MAX_REFRESH_INTERVAL_S;
}

jetkvm_oidc_status_t jetkvm_oidc_core_validator_init(
    jetkvm_oidc_core_validator_t *validator,
    const jetkvm_oidc_core_config_t *config,
    jetkvm_oidc_fetch_jwks_fn fetch_jwks,
    jetkvm_oidc_release_jwks_fn release_jwks,
    void *io_context,
    jetkvm_oidc_verify_rs256_fn verify_rs256,
    void *crypto_context)
{
    if (!validator || !valid_core_config(config) || !fetch_jwks ||
        !verify_rs256) {
        return JETKVM_OIDC_INVALID_ARGUMENT;
    }
    memset(validator, 0, sizeof(*validator));
    validator->config = *config;
    validator->fetch_jwks = fetch_jwks;
    validator->release_jwks = release_jwks;
    validator->verify_rs256 = verify_rs256;
    validator->io_context = io_context;
    validator->crypto_context = crypto_context;
    return JETKVM_OIDC_OK;
}

void jetkvm_oidc_core_validator_invalidate(
    jetkvm_oidc_core_validator_t *validator)
{
    if (!validator) {
        return;
    }
    secure_clear(validator->keys, sizeof(validator->keys));
    validator->key_count = 0U;
    validator->cache_expires_at_ms = 0U;
    validator->cache_installed = false;
    validator->last_refresh_attempt_ms = 0U;
    validator->refresh_attempted = false;
}

static bool refresh_allowed(const jetkvm_oidc_core_validator_t *validator,
                            uint64_t now_ms)
{
    if (!validator->refresh_attempted) {
        return true;
    }
    if (now_ms < validator->last_refresh_attempt_ms) {
        return true;
    }
    uint64_t interval_ms =
        (uint64_t)validator->config.minimum_refresh_interval_s * 1000U;
    return now_ms - validator->last_refresh_attempt_ms >= interval_ms;
}

static jetkvm_oidc_status_t refresh_keys(
    jetkvm_oidc_core_validator_t *validator, uint64_t now_ms)
{
    validator->refresh_attempted = true;
    validator->last_refresh_attempt_ms = now_ms;

    jetkvm_oidc_jwks_response_t response = {
        .max_age_s = JETKVM_OIDC_JWKS_TTL_UNSPECIFIED,
    };
    jetkvm_oidc_status_t status =
        validator->fetch_jwks(validator->io_context, &response);
    if (status != JETKVM_OIDC_OK) {
        if (validator->release_jwks) {
            validator->release_jwks(validator->io_context, &response);
        }
        return status;
    }
    if (!response.data || response.length == 0U ||
        response.length > JETKVM_OIDC_MAX_JWKS_BYTES) {
        if (validator->release_jwks) {
            validator->release_jwks(validator->io_context, &response);
        }
        return response.length > JETKVM_OIDC_MAX_JWKS_BYTES
                   ? JETKVM_OIDC_INPUT_TOO_LARGE
                   : JETKVM_OIDC_INVALID_JWKS;
    }

    const size_t fresh_keys_bytes =
        JETKVM_OIDC_MAX_KEYS * sizeof(jetkvm_oidc_jwk_t);
    jetkvm_oidc_jwk_t *fresh_keys = cJSON_malloc(fresh_keys_bytes);
    if (!fresh_keys) {
        if (validator->release_jwks) {
            validator->release_jwks(validator->io_context, &response);
        }
        return JETKVM_OIDC_NO_MEMORY;
    }
    memset(fresh_keys, 0, fresh_keys_bytes);
    size_t fresh_count = 0U;
    status = jetkvm_oidc_parse_jwks(response.data, response.length,
                                    fresh_keys, JETKVM_OIDC_MAX_KEYS,
                                    &fresh_count);
    uint32_t ttl_s = response.max_age_s;
    if (ttl_s == JETKVM_OIDC_JWKS_TTL_UNSPECIFIED) {
        ttl_s = validator->config.default_jwks_ttl_s;
    }
    if (ttl_s > validator->config.max_jwks_ttl_s) {
        ttl_s = validator->config.max_jwks_ttl_s;
    }
    if (validator->release_jwks) {
        validator->release_jwks(validator->io_context, &response);
    }
    if (status != JETKVM_OIDC_OK) {
        secure_clear(fresh_keys, fresh_keys_bytes);
        cJSON_free(fresh_keys);
        return status;
    }

    secure_clear(validator->keys, sizeof(validator->keys));
    memcpy(validator->keys, fresh_keys,
           fresh_count * sizeof(*fresh_keys));
    validator->key_count = fresh_count;
    validator->cache_installed = true;
    uint64_t ttl_ms = (uint64_t)ttl_s * 1000U;
    validator->cache_expires_at_ms =
        now_ms > UINT64_MAX - ttl_ms ? UINT64_MAX : now_ms + ttl_ms;
    secure_clear(fresh_keys, fresh_keys_bytes);
    cJSON_free(fresh_keys);
    return JETKVM_OIDC_OK;
}

jetkvm_oidc_status_t jetkvm_oidc_core_validate_at(
    jetkvm_oidc_core_validator_t *validator,
    jetkvm_oidc_validation_mode_t mode,
    const char *compact_token, size_t token_length,
    const char *registration_client_id,
    int64_t wall_clock_now_s, uint64_t monotonic_now_ms,
    char *identity, size_t identity_capacity)
{
    if (!validator || !identity || identity_capacity == 0U) {
        return JETKVM_OIDC_INVALID_ARGUMENT;
    }
    identity[0] = '\0';
    jetkvm_oidc_token_t *token = cJSON_malloc(sizeof(*token));
    if (!token) {
        return JETKVM_OIDC_NO_MEMORY;
    }
    memset(token, 0, sizeof(*token));
    jetkvm_oidc_status_t status =
        jetkvm_oidc_parse_token(compact_token, token_length, token);
    char candidate_identity[JETKVM_OIDC_IDENTITY_CAPACITY] = { 0 };
    if (status == JETKVM_OIDC_OK) {
        status = jetkvm_oidc_validate_claims(
            token, mode, registration_client_id, wall_clock_now_s,
            validator->config.clock_skew_s, candidate_identity,
            sizeof(candidate_identity));
    }
    if (status != JETKVM_OIDC_OK) {
        secure_clear(token, sizeof(*token));
        cJSON_free(token);
        secure_clear(candidate_identity, sizeof(candidate_identity));
        return status;
    }

    bool refreshed_this_call = false;
    bool expired = !validator->cache_installed ||
                   monotonic_now_ms >= validator->cache_expires_at_ms;
    if (expired) {
        if (!refresh_allowed(validator, monotonic_now_ms)) {
            status = JETKVM_OIDC_JWKS_UNAVAILABLE;
            goto cleanup;
        }
        status = refresh_keys(validator, monotonic_now_ms);
        refreshed_this_call = true;
        if (status != JETKVM_OIDC_OK) {
            goto cleanup;
        }
    }

    const jetkvm_oidc_jwk_t *key =
        jetkvm_oidc_find_key(validator->keys, validator->key_count,
                             token->kid);
    if (!key && !refreshed_this_call) {
        if (!refresh_allowed(validator, monotonic_now_ms)) {
            status = JETKVM_OIDC_KEY_NOT_FOUND;
            goto cleanup;
        }
        status = refresh_keys(validator, monotonic_now_ms);
        if (status != JETKVM_OIDC_OK) {
            goto cleanup;
        }
        key = jetkvm_oidc_find_key(validator->keys,
                                   validator->key_count, token->kid);
    }
    if (!key) {
        status = JETKVM_OIDC_KEY_NOT_FOUND;
        goto cleanup;
    }
    if (token->signature_length != key->modulus_length) {
        status = JETKVM_OIDC_INVALID_SIGNATURE;
        goto cleanup;
    }
    status = validator->verify_rs256(
        validator->crypto_context, key, token->signing_input,
        token->signing_input_length, token->signature,
        token->signature_length);
    if (status != JETKVM_OIDC_OK) {
        goto cleanup;
    }
    if (strlen(candidate_identity) + 1U > identity_capacity) {
        status = JETKVM_OIDC_IDENTITY_TOO_LARGE;
        goto cleanup;
    }
    memcpy(identity, candidate_identity, strlen(candidate_identity) + 1U);

cleanup:
    if (status != JETKVM_OIDC_OK) {
        identity[0] = '\0';
    }
    secure_clear(token, sizeof(*token));
    cJSON_free(token);
    secure_clear(candidate_identity, sizeof(candidate_identity));
    return status;
}

const char *jetkvm_oidc_status_name(jetkvm_oidc_status_t status)
{
    switch (status) {
    case JETKVM_OIDC_OK: return "ok";
    case JETKVM_OIDC_INVALID_ARGUMENT: return "invalid_argument";
    case JETKVM_OIDC_INPUT_TOO_LARGE: return "input_too_large";
    case JETKVM_OIDC_MALFORMED_TOKEN: return "malformed_token";
    case JETKVM_OIDC_UNSUPPORTED_ALGORITHM: return "unsupported_algorithm";
    case JETKVM_OIDC_MISSING_CLAIM: return "missing_claim";
    case JETKVM_OIDC_INVALID_ISSUER: return "invalid_issuer";
    case JETKVM_OIDC_INVALID_AUDIENCE: return "invalid_audience";
    case JETKVM_OIDC_INVALID_TIME: return "invalid_time";
    case JETKVM_OIDC_CLOCK_NOT_READY: return "clock_not_ready";
    case JETKVM_OIDC_TOKEN_EXPIRED: return "token_expired";
    case JETKVM_OIDC_TOKEN_NOT_YET_VALID: return "token_not_yet_valid";
    case JETKVM_OIDC_IDENTITY_TOO_LARGE: return "identity_too_large";
    case JETKVM_OIDC_INVALID_JWKS: return "invalid_jwks";
    case JETKVM_OIDC_KEY_NOT_FOUND: return "key_not_found";
    case JETKVM_OIDC_INVALID_SIGNATURE: return "invalid_signature";
    case JETKVM_OIDC_JWKS_UNAVAILABLE: return "jwks_unavailable";
    case JETKVM_OIDC_BUSY: return "busy";
    case JETKVM_OIDC_NO_MEMORY: return "no_memory";
    case JETKVM_OIDC_CRYPTO_ERROR: return "crypto_error";
    default: return "unknown";
    }
}

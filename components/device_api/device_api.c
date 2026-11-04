#include "jetkvm_access.h"
/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "device_api.h"
#include "jetkvm_board_config.h"
#include "jetkvm_factory_reset.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_contract.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "jetkvm_cloud_registration.h"
#include "jetkvm_cloud_version.h"
#include "jetkvm_device_state.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_oidc.h"
#include "jetkvm_time_sync.h"

#define CLOUD_REGISTER_JSON_MAX_DEPTH 8U
#define CLOUD_REGISTER_RECV_CALL_LIMIT 16U

static jetkvm_oidc_validator_t *s_oidc_validator;

static void *control_calloc(size_t count, size_t size)
{
    if (count == 0U || size == 0U || count > SIZE_MAX / size) {
        return NULL;
    }
    const size_t bytes = count * size;
    void *memory = cJSON_malloc(bytes);
    if (memory != NULL) {
        memset(memory, 0, bytes);
    }
    return memory;
}

typedef struct {
    jetkvm_cloud_registration_request_t request;
    char token[JETKVM_CLOUD_REGISTRATION_TEMP_TOKEN_MAX_LENGTH + 1U];
    char cloud_api[JETKVM_CLOUD_URL_CAPACITY];
    char oidc_google[JETKVM_CLOUD_REGISTRATION_OIDC_TOKEN_MAX_LENGTH + 1U];
    char client_id[JETKVM_CLOUD_REGISTRATION_CLIENT_ID_MAX_LENGTH + 1U];
} cloud_register_request_storage_t;

typedef struct {
    uint32_t expected_generation;
    char expected_cloud_url[JETKVM_CLOUD_URL_CAPACITY];
} cloud_registration_cas_context_t;

typedef enum {
    BODY_READ_OK = 0,
    BODY_READ_INVALID,
    BODY_READ_NO_MEMORY,
} body_read_status_t;

static void secure_zero(void *value, size_t size)
{
    volatile unsigned char *bytes = value;
    while (size-- > 0U) {
        *bytes++ = 0U;
    }
}

static void set_api_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    /* no-cache, not no-store: Chromium only drains an unread fetch body into
     * its cache, and the shared UI never reads the /device 401 body it
     * redirects on.  With no-store that fetch stays in flight and pages
     * waiting for network idle never settle.  Upstream sends no directive. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, max-age=0");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}

static esp_err_t send_contract(httpd_req_t *req, const char *json)
{
    set_api_headers(req);
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_json_status(httpd_req_t *req, int status,
                                  const char *json)
{
    char status_line[32];
    const char *reason = status == 200 ? "OK" :
                         status == 400 ? "Bad Request" :
                         status == 500 ? "Internal Server Error" :
                                         "Upstream Response";
    int written = snprintf(status_line, sizeof(status_line), "%d %s",
                           status, reason);
    if (written < 0 || (size_t)written >= sizeof(status_line)) {
        return ESP_ERR_INVALID_SIZE;
    }
    set_api_headers(req);
    esp_err_t err = httpd_resp_set_status(req, status_line);
    return err == ESP_OK ? httpd_resp_sendstr(req, json) : err;
}

static bool valid_utf8_sequence(const unsigned char *text, size_t length,
                                size_t *position)
{
    size_t i = *position;
    unsigned char first = text[i];
    size_t count = 0U;
    uint32_t codepoint = 0U;
    if (first >= 0xc2U && first <= 0xdfU) {
        count = 2U;
        codepoint = first & 0x1fU;
    } else if (first >= 0xe0U && first <= 0xefU) {
        count = 3U;
        codepoint = first & 0x0fU;
    } else if (first >= 0xf0U && first <= 0xf4U) {
        count = 4U;
        codepoint = first & 0x07U;
    } else {
        return false;
    }
    if (count > length - i) {
        return false;
    }
    for (size_t offset = 1U; offset < count; ++offset) {
        unsigned char byte = text[i + offset];
        if ((byte & 0xc0U) != 0x80U) {
            return false;
        }
        codepoint = (codepoint << 6U) | (byte & 0x3fU);
    }
    if ((count == 3U && codepoint < 0x800U) ||
        (count == 4U && codepoint < 0x10000U) ||
        codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
        return false;
    }
    *position = i + count;
    return true;
}

static bool parse_hex_quad(const unsigned char *text, size_t length,
                           size_t position, uint16_t *value)
{
    if (position > length || length - position < 4U || !value) {
        return false;
    }
    uint16_t parsed = 0U;
    for (size_t i = 0U; i < 4U; ++i) {
        unsigned char byte = text[position + i];
        uint16_t digit;
        if (byte >= '0' && byte <= '9') {
            digit = (uint16_t)(byte - '0');
        } else if (byte >= 'a' && byte <= 'f') {
            digit = (uint16_t)(byte - 'a' + 10U);
        } else if (byte >= 'A' && byte <= 'F') {
            digit = (uint16_t)(byte - 'A' + 10U);
        } else {
            return false;
        }
        parsed = (uint16_t)((parsed << 4U) | digit);
    }
    *value = parsed;
    return true;
}

/* A small lexical gate keeps hostile input away from cJSON's deep parser. */
static bool safe_json_text(const char *body, size_t length)
{
    if (!body || length == 0U ||
        memchr(body, '\0', length) != NULL) {
        return false;
    }
    const unsigned char *text = (const unsigned char *)body;
    char containers[CLOUD_REGISTER_JSON_MAX_DEPTH];
    size_t depth = 0U;
    bool in_string = false;
    size_t i = 0U;
    while (i < length) {
        unsigned char byte = text[i];
        if (in_string) {
            if (byte == '"') {
                in_string = false;
                ++i;
                continue;
            }
            if (byte == '\\') {
                if (++i >= length) {
                    return false;
                }
                byte = text[i];
                if (byte == 'u') {
                    uint16_t codepoint;
                    if (!parse_hex_quad(text, length, i + 1U,
                                        &codepoint) || codepoint == 0U) {
                        return false;
                    }
                    i += 5U;
                    if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                        uint16_t low;
                        if (i > length || length - i < 6U ||
                            text[i] != '\\' || text[i + 1U] != 'u' ||
                            !parse_hex_quad(text, length, i + 2U, &low) ||
                            low < 0xdc00U || low > 0xdfffU) {
                            return false;
                        }
                        i += 6U;
                    } else if (codepoint >= 0xdc00U &&
                               codepoint <= 0xdfffU) {
                        return false;
                    }
                    continue;
                }
                if (byte != '"' && byte != '\\' && byte != '/' &&
                    byte != 'b' && byte != 'f' && byte != 'n' &&
                    byte != 'r' && byte != 't') {
                    return false;
                }
                ++i;
                continue;
            }
            if (byte < 0x20U) {
                return false;
            }
            if (byte >= 0x80U) {
                if (!valid_utf8_sequence(text, length, &i)) {
                    return false;
                }
                continue;
            }
            ++i;
            continue;
        }
        if (byte == '"') {
            in_string = true;
            ++i;
        } else if (byte == '{' || byte == '[') {
            if (depth >= CLOUD_REGISTER_JSON_MAX_DEPTH) {
                return false;
            }
            containers[depth++] = (char)byte;
            ++i;
        } else if (byte == '}' || byte == ']') {
            if (depth == 0U ||
                (byte == '}' && containers[depth - 1U] != '{') ||
                (byte == ']' && containers[depth - 1U] != '[')) {
                return false;
            }
            --depth;
            ++i;
        } else if (byte >= 0x80U || byte == 0U) {
            return false;
        } else {
            ++i;
        }
    }
    return !in_string && depth == 0U;
}

static void wipe_json_strings(cJSON *item)
{
    for (cJSON *cursor = item; cursor; cursor = cursor->next) {
        if (cursor->valuestring) {
            secure_zero(cursor->valuestring, strlen(cursor->valuestring));
        }
        if (cursor->child) {
            wipe_json_strings(cursor->child);
        }
    }
}

static cJSON *unique_object_member(cJSON *root, const char *name,
                                   size_t *count)
{
    cJSON *found = NULL;
    *count = 0U;
    for (cJSON *item = root ? root->child : NULL; item; item = item->next) {
        if (item->string && strcmp(item->string, name) == 0) {
            found = item;
            ++*count;
        }
    }
    return found;
}

static size_t bounded_string_length(const char *value, size_t capacity)
{
    size_t length = 0U;
    while (length < capacity && value[length] != '\0') {
        ++length;
    }
    return length;
}

static bool copy_required_member(cJSON *root, const char *name,
                                 char *output, size_t capacity)
{
    size_t count;
    cJSON *item = unique_object_member(root, name, &count);
    if (count != 1U || !cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    size_t length = bounded_string_length(item->valuestring, capacity);
    if (length == 0U || length >= capacity) {
        return false;
    }
    memcpy(output, item->valuestring, length + 1U);
    return true;
}

static bool copy_optional_cloud_api(cJSON *root, char *output,
                                    size_t capacity)
{
    size_t count;
    cJSON *item = unique_object_member(root, "cloudApi", &count);
    output[0] = '\0';
    if (count == 0U || cJSON_IsNull(item)) {
        return count <= 1U;
    }
    if (count != 1U || !cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    size_t length = bounded_string_length(item->valuestring, capacity);
    if (length >= capacity) {
        return false;
    }
    memcpy(output, item->valuestring, length + 1U);
    return true;
}

static bool parse_cloud_register_request(
    char *body, size_t length, cloud_register_request_storage_t *storage)
{
    if (!storage || !safe_json_text(body, length)) {
        return false;
    }
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(
        body, length + 1U, &parse_end, true);
    bool valid = root && cJSON_IsObject(root) &&
                 parse_end == body + length &&
                 copy_required_member(root, "token", storage->token,
                                      sizeof(storage->token)) &&
                 copy_optional_cloud_api(root, storage->cloud_api,
                                         sizeof(storage->cloud_api)) &&
                 copy_required_member(root, "oidcGoogle",
                                      storage->oidc_google,
                                      sizeof(storage->oidc_google)) &&
                 copy_required_member(root, "clientId", storage->client_id,
                                      sizeof(storage->client_id));
    if (root) {
        wipe_json_strings(root);
        cJSON_Delete(root);
    }
    if (!valid) {
        return false;
    }
    storage->request = (jetkvm_cloud_registration_request_t) {
        .token = storage->token,
        .cloud_api = storage->cloud_api,
        .oidc_google = storage->oidc_google,
        .client_id = storage->client_id,
    };
    return true;
}

static body_read_status_t read_request_body(httpd_req_t *req,
                                            char **body_out,
                                            size_t *length_out)
{
    *body_out = NULL;
    *length_out = 0U;
    if (!req || req->content_len == 0U ||
        req->content_len > P4_DEVICE_API_CLOUD_REGISTER_BODY_MAX_LENGTH) {
        return BODY_READ_INVALID;
    }
    char *body = cJSON_malloc(req->content_len + 1U);
    if (!body) {
        return BODY_READ_NO_MEMORY;
    }
    size_t received = 0U;
    unsigned calls = 0U;
    while (received < req->content_len &&
           calls++ < CLOUD_REGISTER_RECV_CALL_LIMIT) {
        int chunk = httpd_req_recv(req, body + received,
                                   req->content_len - received);
        if (chunk <= 0 || (size_t)chunk > req->content_len - received) {
            secure_zero(body, received);
            cJSON_free(body);
            return BODY_READ_INVALID;
        }
        received += (size_t)chunk;
    }
    if (received != req->content_len) {
        secure_zero(body, received);
        cJSON_free(body);
        return BODY_READ_INVALID;
    }
    body[received] = '\0';
    *body_out = body;
    *length_out = received;
    return BODY_READ_OK;
}

static jetkvm_cloud_registration_oidc_status_t validate_registration_oidc(
    void *context, const char *oidc_google, const char *client_id,
    char *identity, size_t identity_capacity)
{
    jetkvm_oidc_status_t status = jetkvm_oidc_validate_registration(
        context, oidc_google, client_id, identity, identity_capacity);
    switch (status) {
    case JETKVM_OIDC_OK:
        return JETKVM_CLOUD_REGISTRATION_OIDC_OK;
    case JETKVM_OIDC_INPUT_TOO_LARGE:
    case JETKVM_OIDC_MALFORMED_TOKEN:
    case JETKVM_OIDC_UNSUPPORTED_ALGORITHM:
    case JETKVM_OIDC_MISSING_CLAIM:
    case JETKVM_OIDC_INVALID_ISSUER:
    case JETKVM_OIDC_INVALID_AUDIENCE:
    case JETKVM_OIDC_INVALID_TIME:
    case JETKVM_OIDC_TOKEN_EXPIRED:
    case JETKVM_OIDC_TOKEN_NOT_YET_VALID:
    case JETKVM_OIDC_IDENTITY_TOO_LARGE:
    case JETKVM_OIDC_KEY_NOT_FOUND:
    case JETKVM_OIDC_INVALID_SIGNATURE:
        return JETKVM_CLOUD_REGISTRATION_OIDC_INVALID;
    default:
        return JETKVM_CLOUD_REGISTRATION_OIDC_INTERNAL_ERROR;
    }
}

static esp_err_t save_registration_if_current(
    void *context, const jetkvm_cloud_config_t *config)
{
    const cloud_registration_cas_context_t *cas = context;
    if (!cas || !config) {
        return ESP_ERR_INVALID_ARG;
    }
    return jetkvm_device_state_cloud_set_registration_if_current(
        cas->expected_generation, cas->expected_cloud_url,
        config->cloud_token, config->google_identity);
}

static const char *registration_error_json(
    jetkvm_cloud_registration_status_t status)
{
    switch (status) {
    case JETKVM_CLOUD_REGISTRATION_INVALID_ARGUMENT:
        return "{\"error\":\"Invalid request body\"}";
    case JETKVM_CLOUD_REGISTRATION_UPSTREAM_HTTP_ERROR:
        return "{\"error\":\"Failed to exchange token\"}";
    case JETKVM_CLOUD_REGISTRATION_INVALID_TOKEN_RESPONSE:
        return "{\"error\":\"Failed to parse token response\"}";
    case JETKVM_CLOUD_REGISTRATION_OIDC_REJECTED:
        return "{\"error\":\"Invalid OIDC token\"}";
    case JETKVM_CLOUD_REGISTRATION_PERSIST_FAILED:
        return "{\"error\":\"Failed to save configuration\"}";
    default:
        return "{\"error\":\"Cloud registration failed\"}";
    }
}

static int registration_http_status(
    jetkvm_cloud_registration_status_t status,
    const jetkvm_cloud_registration_result_t *result)
{
    if (status == JETKVM_CLOUD_REGISTRATION_OK) {
        return 200;
    }
    if (status == JETKVM_CLOUD_REGISTRATION_INVALID_ARGUMENT ||
        status == JETKVM_CLOUD_REGISTRATION_OIDC_REJECTED) {
        return 400;
    }
    if (status == JETKVM_CLOUD_REGISTRATION_UPSTREAM_HTTP_ERROR && result &&
        result->response_http_status >= 100 &&
        result->response_http_status <= 599) {
        return result->response_http_status;
    }
    return 500;
}

static bool registration_success_json(char *json, size_t capacity)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char version[sizeof(app->version) +
                 sizeof(JETKVM_CLOUD_CORE_APP_VERSION_SUFFIX) - 1U];
    snprintf(version, sizeof(version), "%s%s", app->version,
             JETKVM_CLOUD_CORE_APP_VERSION_SUFFIX);
    cJSON *body = cJSON_CreateObject();
    bool rendered = body &&
        cJSON_AddStringToObject(body, "message", "Cloud registration successful") &&
        cJSON_AddStringToObject(body, "cloudUiVersion", version) &&
        cJSON_PrintPreallocated(body, json, (int)capacity, false);
    cJSON_Delete(body);
    return rendered;
}

static esp_err_t cloud_register_handler(httpd_req_t *req)
{
    jetkvm_oidc_validator_t *validator = req ? req->user_ctx : NULL;
    if (!validator) {
        return send_json_status(
            req, 500, "{\"error\":\"Cloud registration unavailable\"}");
    }

    char *body = NULL;
    size_t body_length = 0U;
    body_read_status_t read_status = read_request_body(
        req, &body, &body_length);
    if (read_status != BODY_READ_OK) {
        return send_json_status(
            req,
            read_status == BODY_READ_NO_MEMORY ? 500 : 400,
            read_status == BODY_READ_NO_MEMORY
                ? "{\"error\":\"Cloud registration unavailable\"}"
                : "{\"error\":\"Invalid request body\"}");
    }

    cloud_register_request_storage_t *parsed =
        control_calloc(1U, sizeof(*parsed));
    bool request_valid = parsed &&
                         parse_cloud_register_request(
                             body, body_length, parsed);
    secure_zero(body, body_length + 1U);
    cJSON_free(body);
    if (!request_valid) {
        if (parsed) {
            secure_zero(parsed, sizeof(*parsed));
            cJSON_free(parsed);
        }
        return send_json_status(
            req, parsed ? 400 : 500,
            parsed ? "{\"error\":\"Invalid request body\"}"
                   : "{\"error\":\"Cloud registration unavailable\"}");
    }

    jetkvm_cloud_config_t *current = control_calloc(1U, sizeof(*current));
    jetkvm_cloud_config_t *registered =
        control_calloc(1U, sizeof(*registered));
    uint32_t generation = 0U;
    esp_err_t snapshot_error = current && registered
        ? jetkvm_device_state_cloud_snapshot(current, &generation)
        : ESP_ERR_NO_MEM;
    if (snapshot_error != ESP_OK) {
        if (current) {
            secure_zero(current, sizeof(*current));
            cJSON_free(current);
        }
        if (registered) {
            secure_zero(registered, sizeof(*registered));
            cJSON_free(registered);
        }
        secure_zero(parsed, sizeof(*parsed));
        cJSON_free(parsed);
        return send_json_status(
            req, 500,
            "{\"error\":\"Cloud registration unavailable\"}");
    }

    esp_err_t time_error = jetkvm_time_sync_start();
    if (time_error == ESP_OK) {
        time_error = jetkvm_time_sync_wait(pdMS_TO_TICKS(
            P4_DEVICE_API_CLOUD_REGISTER_TIME_WAIT_MS));
    }
    if (time_error != ESP_OK) {
        secure_zero(current, sizeof(*current));
        secure_zero(registered, sizeof(*registered));
        secure_zero(parsed, sizeof(*parsed));
        cJSON_free(current);
        cJSON_free(registered);
        cJSON_free(parsed);
        return send_json_status(
            req, 500,
            "{\"error\":\"System time unavailable\"}");
    }

    cloud_registration_cas_context_t cas = {
        .expected_generation = generation,
    };
    memcpy(cas.expected_cloud_url, current->cloud_url,
           sizeof(cas.expected_cloud_url));

    /* The token request and the key set fetch are two TLS connections, one
     * after the other, so one connection's reservation covers both. */
    jetkvm_memory_policy_reservation_t memory_reservation = {0};
    if (!jetkvm_memory_policy_wait_reserve_tls(
            "cloud-registration-tls", &memory_reservation)) {
        secure_zero(current, sizeof(*current));
        secure_zero(registered, sizeof(*registered));
        secure_zero(parsed, sizeof(*parsed));
        secure_zero(&cas, sizeof(cas));
        cJSON_free(current);
        cJSON_free(registered);
        cJSON_free(parsed);
        return send_json_status(
            req, 503,
            "{\"error\":\"Insufficient memory for cloud registration\"}");
    }
    jetkvm_cloud_registration_result_t result = { 0 };
    /* Prepare the response before registration can commit. Allocation failure
     * must not leave a registered device behind an unsuccessful HTTP reply. */
    char success_json[192];
    jetkvm_cloud_registration_status_t status =
        registration_success_json(success_json, sizeof(success_json)) ?
            jetkvm_cloud_registration_register_snapshot(
                current, &parsed->request,
                validate_registration_oidc, validator,
                save_registration_if_current, &cas,
                registered, &result) : JETKVM_CLOUD_REGISTRATION_OUT_OF_MEMORY;
    /* The HTTPS/OIDC transaction is complete. Do not keep the reservation
     * while a slow HTTP client receives the local response. */
    jetkvm_memory_policy_release(&memory_reservation);

    secure_zero(current, sizeof(*current));
    secure_zero(registered, sizeof(*registered));
    secure_zero(parsed, sizeof(*parsed));
    secure_zero(&cas, sizeof(cas));
    cJSON_free(current);
    cJSON_free(registered);
    cJSON_free(parsed);

    if (status == JETKVM_CLOUD_REGISTRATION_OK) {
        return send_json_status(req, 200, success_json);
    }
    return send_json_status(
        req, registration_http_status(status, &result),
        registration_error_json(status));
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char json[512];
    if (!p4_device_status_json(jetkvm_access_is_setup(), jetkvm_factory_reset_pending(),
                               jetkvm_factory_reset_error(), JETKVM_DEVICE_SKU,
                               json, sizeof(json))) {
        set_api_headers(req);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Setup status unavailable");
    }
    return send_contract(req, json);
}

static esp_err_t identity_handler(httpd_req_t *req)
{
    char device_id[JETKVM_DEVICE_ID_CAPACITY];
    char json[96];
    esp_err_t err = jetkvm_device_state_get_id(device_id, sizeof(device_id));
    if (err != ESP_OK ||
        !p4_device_identity_json(device_id, jetkvm_access_password_enabled(), json, sizeof(json))) {
        set_api_headers(req);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "device identity unavailable");
    }
    return send_contract(req, json);
}

static esp_err_t capabilities_handler(httpd_req_t *req)
{
    char json[P4_DEVICE_CAPABILITIES_JSON_CAPACITY];
    if (!p4_device_capabilities_json_copy(json, sizeof(json))) {
        set_api_headers(req);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "capabilities unavailable");
    }
    return send_contract(req, json);
}

static esp_err_t cloud_state_handler(httpd_req_t *req)
{
    const size_t json_capacity = JETKVM_CLOUD_URL_CAPACITY +
                                 JETKVM_CLOUD_APP_URL_CAPACITY + 80U;
    jetkvm_device_cloud_public_state_t *state =
        control_calloc(1U, sizeof(*state));
    char *json = cJSON_malloc(json_capacity);
    if (!state || !json) {
        cJSON_free(json);
        cJSON_free(state);
        set_api_headers(req);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "cloud state unavailable");
    }
    esp_err_t err = jetkvm_device_state_cloud_public_snapshot(state);
    bool rendered = err == ESP_OK && p4_cloud_state_json(
        state->registered, state->cloud_url,
        state->cloud_app_url, json, json_capacity);
    if (!rendered) {
        cJSON_free(json);
        cJSON_free(state);
        set_api_headers(req);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "cloud state unavailable");
    }
    err = send_contract(req, json);
    cJSON_free(json);
    cJSON_free(state);
    return err;
}

static esp_err_t register_handler(httpd_handle_t server, const char *uri,
                                  httpd_method_t method,
                                  esp_err_t (*handler)(httpd_req_t *),
                                  void *user_ctx)
{
    const httpd_uri_t config = {
        .uri = uri,
        .method = method,
        .handler = handler,
        .user_ctx = user_ctx,
    };
    return jetkvm_access_register(server, &config);
}

esp_err_t p4_device_api_set_oidc_validator(
    jetkvm_oidc_validator_t *validator)
{
    if (!validator) {
        return ESP_ERR_INVALID_ARG;
    }
    s_oidc_validator = validator;
    return ESP_OK;
}

esp_err_t p4_device_api_register_handlers(httpd_handle_t server)
{
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = register_handler(server, "/device/status", HTTP_GET,
                                     status_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = register_handler(server, "/device", HTTP_GET,
                           identity_handler, NULL);
    if (err != ESP_OK) {
        httpd_unregister_uri_handler(server, "/device/status", HTTP_GET);
        return err;
    }
    err = register_handler(server, "/device/capabilities", HTTP_GET,
                           capabilities_handler, NULL);
    if (err != ESP_OK) {
        httpd_unregister_uri_handler(server, "/device", HTTP_GET);
        httpd_unregister_uri_handler(server, "/device/status", HTTP_GET);
        return err;
    }
    err = register_handler(server, "/cloud/state", HTTP_GET,
                           cloud_state_handler, NULL);
    if (err != ESP_OK) {
        httpd_unregister_uri_handler(server, "/device/capabilities", HTTP_GET);
        httpd_unregister_uri_handler(server, "/device", HTTP_GET);
        httpd_unregister_uri_handler(server, "/device/status", HTTP_GET);
        return err;
    }
    err = register_handler(server, "/cloud/register", HTTP_POST,
                           cloud_register_handler, s_oidc_validator);
    if (err != ESP_OK) {
        httpd_unregister_uri_handler(server, "/cloud/state", HTTP_GET);
        httpd_unregister_uri_handler(server, "/device/capabilities", HTTP_GET);
        httpd_unregister_uri_handler(server, "/device", HTTP_GET);
        httpd_unregister_uri_handler(server, "/device/status", HTTP_GET);
        return err;
    }
    return ESP_OK;
}

esp_err_t p4_device_api_unregister_handlers(httpd_handle_t server)
{
    if (!server) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t first = httpd_unregister_uri_handler(
        server, "/cloud/register", HTTP_POST);
    esp_err_t second = httpd_unregister_uri_handler(
        server, "/cloud/state", HTTP_GET);
    esp_err_t third = httpd_unregister_uri_handler(
        server, "/device/capabilities", HTTP_GET);
    esp_err_t fourth = httpd_unregister_uri_handler(
        server, "/device", HTTP_GET);
    esp_err_t fifth = httpd_unregister_uri_handler(
        server, "/device/status", HTTP_GET);
    if (first != ESP_OK) {
        return first;
    }
    if (second != ESP_OK) {
        return second;
    }
    if (third != ESP_OK) {
        return third;
    }
    return fourth != ESP_OK ? fourth : fifth;
}

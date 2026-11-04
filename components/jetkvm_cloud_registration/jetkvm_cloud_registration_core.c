/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_cloud_registration.h"
#include "jetkvm_cloud_registration_url.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define TOKEN_ENDPOINT_SUFFIX "/devices/token"

bool jetkvm_cloud_public_ip_parse(const char *trace, char *ip,
                                  size_t ip_capacity, int64_t *seen_at)
{
    ip[0] = '\0';
    *seen_at = 0;
    for (const char *line = trace; line != NULL && *line != '\0';) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        if (length > 3U && strncmp(line, "ip=", 3U) == 0 &&
            length - 3U < ip_capacity &&
            strspn(line + 3, "0123456789abcdefABCDEF.:") == length - 3U) {
            memcpy(ip, line + 3, length - 3U);
            ip[length - 3U] = '\0';
        } else if (strncmp(line, "ts=", 3U) == 0) {
            *seen_at = strtoll(line + 3, NULL, 10);
        }
        line = end ? end + 1 : NULL;
    }
    return ip[0] != '\0';
}

static size_t bounded_length(const char *value, size_t maximum)
{
    if (!value) {
        return maximum + 1U;
    }
    size_t length = 0U;
    while (length <= maximum && value[length] != '\0') {
        ++length;
    }
    return length;
}

static bool required_bounded_string(const char *value, size_t maximum)
{
    size_t length = bounded_length(value, maximum);
    return length > 0U && length <= maximum;
}

static void secure_zero(void *value, size_t size)
{
    volatile unsigned char *bytes = (volatile unsigned char *)value;
    while (size-- > 0U) {
        *bytes++ = 0U;
    }
}

typedef struct {
    jetkvm_cloud_config_t staged;
    char secret_token[JETKVM_CLOUD_TOKEN_CAPACITY];
    char identity[JETKVM_CLOUD_IDENTITY_CAPACITY];
} registration_workspace_t;

static void release_workspace(registration_workspace_t *workspace)
{
    if (workspace) {
        secure_zero(workspace, sizeof(*workspace));
        cJSON_free(workspace);
    }
}

static void set_result(jetkvm_cloud_registration_result_t *result,
                       jetkvm_cloud_registration_status_t status,
                       int response_http_status,
                       int upstream_http_status,
                       esp_err_t cause)
{
    if (!result) {
        return;
    }
    result->status = status;
    result->response_http_status = response_http_status;
    result->upstream_http_status = upstream_http_status;
    result->cause = cause;
}

static jetkvm_cloud_registration_status_t fail(
    jetkvm_cloud_registration_result_t *result,
    jetkvm_cloud_registration_status_t status,
    int response_http_status, int upstream_http_status, esp_err_t cause)
{
    set_result(result, status, response_http_status,
               upstream_http_status, cause);
    return status;
}

static char *make_token_url(const char *cloud_url)
{
    size_t base_length = strlen(cloud_url);
    size_t suffix_length = sizeof(TOKEN_ENDPOINT_SUFFIX) - 1U;
    if (base_length > SIZE_MAX - suffix_length - 1U) {
        return NULL;
    }
    char *url = cJSON_malloc(base_length + suffix_length + 1U);
    if (!url) {
        return NULL;
    }
    memcpy(url, cloud_url, base_length);
    memcpy(url + base_length, TOKEN_ENDPOINT_SUFFIX, suffix_length + 1U);
    return url;
}

static char *make_token_body(const char *temporary_token)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON *token = cJSON_AddStringToObject(
        root, "tempToken", temporary_token);
    if (!token) {
        cJSON_Delete(root);
        return NULL;
    }
    char *body = cJSON_PrintUnformatted(root);
    if (token->valuestring) {
        secure_zero(token->valuestring, strlen(token->valuestring));
    }
    cJSON_Delete(root);
    return body;
}

static bool parse_secret_token(const char *response, size_t response_length,
                               char token[JETKVM_CLOUD_TOKEN_CAPACITY])
{
    if (!response || memchr(response, '\0', response_length) != NULL) {
        return false;
    }
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(
        response, response_length + 1U, &parse_end, true);
    if (!root || !cJSON_IsObject(root) ||
        parse_end != response + response_length) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *secret = cJSON_GetObjectItemCaseSensitive(
        root, "secretToken");
    bool valid = cJSON_IsString(secret) && secret->valuestring != NULL;
    size_t length = valid ? bounded_length(
                                secret->valuestring,
                                JETKVM_CLOUD_TOKEN_CAPACITY - 1U)
                          : 0U;
    valid = valid && length > 0U &&
            length < JETKVM_CLOUD_TOKEN_CAPACITY;
    if (valid) {
        memcpy(token, secret->valuestring, length + 1U);
    }
    if (cJSON_IsString(secret) && secret->valuestring) {
        secure_zero(secret->valuestring, strlen(secret->valuestring));
    }
    cJSON_Delete(root);
    return valid;
}

jetkvm_cloud_registration_status_t
jetkvm_cloud_registration_run_transaction(
    const jetkvm_cloud_config_t *current_config,
    const jetkvm_cloud_registration_request_t *request,
    const jetkvm_cloud_registration_operations_t *operations,
    jetkvm_cloud_config_t *registered_config,
    jetkvm_cloud_registration_result_t *result)
{
    set_result(result, JETKVM_CLOUD_REGISTRATION_INVALID_ARGUMENT,
               400, 0, ESP_ERR_INVALID_ARG);
    if (current_config && registered_config) {
        *registered_config = *current_config;
    }
    if (!current_config || !request || !operations || !registered_config ||
        !operations->exchange || !operations->validate_oidc ||
        !operations->save ||
        !required_bounded_string(
            request->token,
            JETKVM_CLOUD_REGISTRATION_TEMP_TOKEN_MAX_LENGTH) ||
        !required_bounded_string(
            request->oidc_google,
            JETKVM_CLOUD_REGISTRATION_OIDC_TOKEN_MAX_LENGTH) ||
        !required_bounded_string(
            request->client_id,
            JETKVM_CLOUD_REGISTRATION_CLIENT_ID_MAX_LENGTH)) {
        return JETKVM_CLOUD_REGISTRATION_INVALID_ARGUMENT;
    }

    if (!jetkvm_cloud_config_validate(current_config) ||
        !cloud_registration_url_allowed(current_config->cloud_url)) {
        return fail(result, JETKVM_CLOUD_REGISTRATION_INVALID_CONFIG,
                    500, 0, ESP_ERR_INVALID_STATE);
    }

    char *url = make_token_url(current_config->cloud_url);
    char *body = make_token_body(request->token);
    char *response = cJSON_malloc(
        JETKVM_CLOUD_REGISTRATION_RESPONSE_MAX_LENGTH + 1U);
    if (response != NULL) {
        memset(response, 0,
               JETKVM_CLOUD_REGISTRATION_RESPONSE_MAX_LENGTH + 1U);
    }
    if (!url || !body || !response) {
        cJSON_free(url);
        cJSON_free(body);
        cJSON_free(response);
        return fail(result, JETKVM_CLOUD_REGISTRATION_OUT_OF_MEMORY,
                    500, 0, ESP_ERR_NO_MEM);
    }

    size_t response_length = 0U;
    int upstream_status = 0;
    esp_err_t exchange_result = operations->exchange(
        operations->exchange_context, url, body, response,
        JETKVM_CLOUD_REGISTRATION_RESPONSE_MAX_LENGTH + 1U,
        &response_length, &upstream_status);
    secure_zero(body, strlen(body));
    cJSON_free(body);
    cJSON_free(url);

    if (exchange_result != ESP_OK) {
        secure_zero(response,
                    JETKVM_CLOUD_REGISTRATION_RESPONSE_MAX_LENGTH + 1U);
        cJSON_free(response);
        jetkvm_cloud_registration_status_t status =
            exchange_result == ESP_ERR_INVALID_SIZE
                ? JETKVM_CLOUD_REGISTRATION_RESPONSE_TOO_LARGE
                : JETKVM_CLOUD_REGISTRATION_EXCHANGE_FAILED;
        return fail(result, status, 500, upstream_status, exchange_result);
    }
    if (upstream_status < 100 || upstream_status > 599) {
        secure_zero(response,
                    JETKVM_CLOUD_REGISTRATION_RESPONSE_MAX_LENGTH + 1U);
        cJSON_free(response);
        return fail(result, JETKVM_CLOUD_REGISTRATION_EXCHANGE_FAILED,
                    500, upstream_status, ESP_ERR_INVALID_RESPONSE);
    }
    if (upstream_status != 200) {
        secure_zero(response,
                    JETKVM_CLOUD_REGISTRATION_RESPONSE_MAX_LENGTH + 1U);
        cJSON_free(response);
        return fail(result,
                    JETKVM_CLOUD_REGISTRATION_UPSTREAM_HTTP_ERROR,
                    upstream_status, upstream_status, ESP_OK);
    }
    if (response_length > JETKVM_CLOUD_REGISTRATION_RESPONSE_MAX_LENGTH) {
        secure_zero(response,
                    JETKVM_CLOUD_REGISTRATION_RESPONSE_MAX_LENGTH + 1U);
        cJSON_free(response);
        return fail(result,
                    JETKVM_CLOUD_REGISTRATION_RESPONSE_TOO_LARGE,
                    500, upstream_status, ESP_ERR_INVALID_SIZE);
    }
    response[response_length] = '\0';

    registration_workspace_t *workspace = cJSON_malloc(sizeof(*workspace));
    if (!workspace) {
        secure_zero(response,
                    JETKVM_CLOUD_REGISTRATION_RESPONSE_MAX_LENGTH + 1U);
        cJSON_free(response);
        return fail(result, JETKVM_CLOUD_REGISTRATION_OUT_OF_MEMORY,
                    500, upstream_status, ESP_ERR_NO_MEM);
    }
    memset(workspace, 0, sizeof(*workspace));
    bool token_valid = parse_secret_token(
        response, response_length, workspace->secret_token);
    secure_zero(response,
                JETKVM_CLOUD_REGISTRATION_RESPONSE_MAX_LENGTH + 1U);
    cJSON_free(response);
    if (!token_valid) {
        release_workspace(workspace);
        return fail(result,
                    JETKVM_CLOUD_REGISTRATION_INVALID_TOKEN_RESPONSE,
                    500, upstream_status, ESP_ERR_INVALID_RESPONSE);
    }

    jetkvm_cloud_registration_oidc_status_t oidc_result =
        operations->validate_oidc(
            operations->oidc_context, request->oidc_google,
            request->client_id, workspace->identity,
            sizeof(workspace->identity));
    if (oidc_result != JETKVM_CLOUD_REGISTRATION_OIDC_OK) {
        release_workspace(workspace);
        if (oidc_result == JETKVM_CLOUD_REGISTRATION_OIDC_INVALID) {
            return fail(result,
                        JETKVM_CLOUD_REGISTRATION_OIDC_REJECTED,
                        400, upstream_status, ESP_ERR_INVALID_RESPONSE);
        }
        return fail(result, JETKVM_CLOUD_REGISTRATION_OIDC_FAILED,
                    500, upstream_status, ESP_FAIL);
    }

    workspace->staged = *current_config;
    esp_err_t update_result = jetkvm_cloud_config_set_registration(
        &workspace->staged, workspace->secret_token, workspace->identity);
    secure_zero(workspace->secret_token, sizeof(workspace->secret_token));
    secure_zero(workspace->identity, sizeof(workspace->identity));
    if (update_result != ESP_OK ||
        !jetkvm_cloud_config_validate(&workspace->staged)) {
        release_workspace(workspace);
        return fail(result,
                    JETKVM_CLOUD_REGISTRATION_CONFIG_UPDATE_FAILED,
                    500, upstream_status,
                    update_result == ESP_OK ? ESP_ERR_INVALID_STATE
                                            : update_result);
    }

    esp_err_t save_result = operations->save(
        operations->save_context, &workspace->staged);
    if (save_result != ESP_OK) {
        release_workspace(workspace);
        return fail(result, JETKVM_CLOUD_REGISTRATION_PERSIST_FAILED,
                    500, upstream_status, save_result);
    }

    *registered_config = workspace->staged;
    release_workspace(workspace);
    set_result(result, JETKVM_CLOUD_REGISTRATION_OK,
               200, upstream_status, ESP_OK);
    return JETKVM_CLOUD_REGISTRATION_OK;
}

const char *jetkvm_cloud_registration_status_name(
    jetkvm_cloud_registration_status_t status)
{
    switch (status) {
    case JETKVM_CLOUD_REGISTRATION_OK:
        return "ok";
    case JETKVM_CLOUD_REGISTRATION_INVALID_ARGUMENT:
        return "invalid_argument";
    case JETKVM_CLOUD_REGISTRATION_CONFIG_LOAD_FAILED:
        return "config_load_failed";
    case JETKVM_CLOUD_REGISTRATION_INVALID_CONFIG:
        return "invalid_config";
    case JETKVM_CLOUD_REGISTRATION_OUT_OF_MEMORY:
        return "out_of_memory";
    case JETKVM_CLOUD_REGISTRATION_EXCHANGE_FAILED:
        return "exchange_failed";
    case JETKVM_CLOUD_REGISTRATION_RESPONSE_TOO_LARGE:
        return "response_too_large";
    case JETKVM_CLOUD_REGISTRATION_UPSTREAM_HTTP_ERROR:
        return "upstream_http_error";
    case JETKVM_CLOUD_REGISTRATION_INVALID_TOKEN_RESPONSE:
        return "invalid_token_response";
    case JETKVM_CLOUD_REGISTRATION_OIDC_REJECTED:
        return "oidc_rejected";
    case JETKVM_CLOUD_REGISTRATION_OIDC_FAILED:
        return "oidc_failed";
    case JETKVM_CLOUD_REGISTRATION_CONFIG_UPDATE_FAILED:
        return "config_update_failed";
    case JETKVM_CLOUD_REGISTRATION_PERSIST_FAILED:
        return "persist_failed";
    default:
        return "unknown";
    }
}

/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_oidc.h"
#include "jetkvm_oidc_private.h"

#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "jetkvm_memory_policy.h"

#define OIDC_DEFAULT_HTTP_TIMEOUT_MS 10000U
#define OIDC_DEFAULT_LOCK_TIMEOUT_MS 12000U
#define OIDC_MIN_HTTP_TIMEOUT_MS 1000U
#define OIDC_MAX_HTTP_TIMEOUT_MS 30000U
#define OIDC_MAX_LOCK_TIMEOUT_MS 60000U
#define OIDC_HEADER_CAPACITY 256U

/* CMake defines JETKVM_MOCK_CLOUD_JWKS_URL only for bench builds that test
 * against the e2e mock cloud. */
#ifdef JETKVM_MOCK_CLOUD_JWKS_URL
#define OIDC_JWKS_URL JETKVM_MOCK_CLOUD_JWKS_URL
#else
#define OIDC_JWKS_URL JETKVM_OIDC_GOOGLE_JWKS_URL
#endif

typedef struct {
    char *body;
    size_t body_length;
    bool overflow;
    char cache_control[OIDC_HEADER_CAPACITY];
    char content_type[OIDC_HEADER_CAPACITY];
    char age[32];
} oidc_http_response_t;

struct jetkvm_oidc_validator {
    SemaphoreHandle_t mutex;
    uint32_t http_timeout_ms;
    uint32_t lock_timeout_ms;
    jetkvm_oidc_core_validator_t core;
};

static size_t bounded_length(const char *value, size_t maximum)
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

static void copy_header(char *destination, size_t capacity,
                        const char *source)
{
    if (!destination || capacity == 0U || !source) {
        return;
    }
    size_t length = bounded_length(source, capacity);
    if (length >= capacity) {
        destination[0] = '\0';
        return;
    }
    memcpy(destination, source, length + 1U);
}

static esp_err_t oidc_http_event(esp_http_client_event_t *event)
{
    if (!event || !event->user_data) {
        return ESP_OK;
    }
    oidc_http_response_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER &&
        event->header_key && event->header_value) {
        if (strcasecmp(event->header_key, "Cache-Control") == 0) {
            copy_header(response->cache_control,
                        sizeof(response->cache_control),
                        event->header_value);
        } else if (strcasecmp(event->header_key, "Content-Type") == 0) {
            copy_header(response->content_type,
                        sizeof(response->content_type),
                        event->header_value);
        } else if (strcasecmp(event->header_key, "Age") == 0) {
            copy_header(response->age, sizeof(response->age),
                        event->header_value);
        }
    } else if (event->event_id == HTTP_EVENT_ON_DATA &&
               event->data && event->data_len > 0) {
        size_t chunk_length = (size_t)event->data_len;
        if (response->body_length > JETKVM_OIDC_MAX_JWKS_BYTES ||
            chunk_length >
                JETKVM_OIDC_MAX_JWKS_BYTES - response->body_length) {
            response->overflow = true;
            return ESP_FAIL;
        }
        memcpy(response->body + response->body_length,
               event->data, chunk_length);
        response->body_length += chunk_length;
    }
    return ESP_OK;
}

static bool content_type_is_json(const char *content_type)
{
    static const char expected[] = "application/json";
    size_t length = bounded_length(content_type, OIDC_HEADER_CAPACITY);
    if (length < sizeof(expected) - 1U ||
        strncasecmp(content_type, expected, sizeof(expected) - 1U) != 0) {
        return false;
    }
    if (length == sizeof(expected) - 1U) {
        return true;
    }
    return content_type[sizeof(expected) - 1U] == ';' ||
           content_type[sizeof(expected) - 1U] == ' ' ||
           content_type[sizeof(expected) - 1U] == '\t';
}

static bool parse_decimal_u32(const char *value, uint32_t *parsed)
{
    if (!value || !parsed || value[0] == '\0') {
        return false;
    }
    uint64_t result = 0U;
    for (size_t index = 0U; value[index] != '\0'; ++index) {
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
        result = result * 10U + (uint64_t)(value[index] - '0');
        if (result > UINT32_MAX) {
            return false;
        }
    }
    *parsed = (uint32_t)result;
    return true;
}

static bool has_cache_directive(const char *header, const char *wanted)
{
    if (!header || !wanted) {
        return false;
    }
    size_t header_length = bounded_length(header, OIDC_HEADER_CAPACITY);
    size_t cursor = 0U;
    while (cursor < header_length) {
        while (cursor < header_length &&
               (header[cursor] == ' ' || header[cursor] == '\t' ||
                header[cursor] == ',')) {
            ++cursor;
        }
        size_t start = cursor;
        while (cursor < header_length && header[cursor] != ',' &&
               header[cursor] != '=') {
            ++cursor;
        }
        size_t end = cursor;
        while (end > start &&
               (header[end - 1U] == ' ' || header[end - 1U] == '\t')) {
            --end;
        }
        size_t wanted_length = strlen(wanted);
        if (end - start == wanted_length &&
            strncasecmp(header + start, wanted, wanted_length) == 0) {
            return true;
        }
        while (cursor < header_length && header[cursor] != ',') {
            ++cursor;
        }
    }
    return false;
}

static jetkvm_oidc_status_t esp_fetch_jwks(
    void *context, jetkvm_oidc_jwks_response_t *result)
{
    jetkvm_oidc_validator_t *validator = context;
    if (!validator || !result) {
        return JETKVM_OIDC_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    result->max_age_s = JETKVM_OIDC_JWKS_TTL_UNSPECIFIED;

    oidc_http_response_t response = { 0 };
    response.body = cJSON_malloc(JETKVM_OIDC_MAX_JWKS_BYTES + 1U);
    if (!response.body) {
        return JETKVM_OIDC_NO_MEMORY;
    }

    esp_http_client_config_t http_config = {
        .url = OIDC_JWKS_URL,
        .user_agent = "JetKVM-ESP32P4-OIDC/1",
        .method = HTTP_METHOD_GET,
        .timeout_ms = (int)validator->http_timeout_ms,
        .disable_auto_redirect = true,
        .event_handler = oidc_http_event,
        .user_data = &response,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .skip_cert_common_name_check = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        cJSON_free(response.body);
        return JETKVM_OIDC_NO_MEMORY;
    }
    (void)esp_http_client_set_header(client, "Accept", "application/json");
    (void)esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_err_t error = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    bool complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);

    if (error != ESP_OK || response.overflow || !complete ||
        status_code != 200 || response.body_length == 0U ||
        !content_type_is_json(response.content_type)) {
        cJSON_free(response.body);
        return JETKVM_OIDC_JWKS_UNAVAILABLE;
    }
    response.body[response.body_length] = '\0';
    uint32_t max_age_s = 0U;
    if (has_cache_directive(response.cache_control, "no-store") ||
        has_cache_directive(response.cache_control, "no-cache")) {
        result->max_age_s = 0U;
    } else if (jetkvm_oidc_parse_cache_max_age(
            response.cache_control, strlen(response.cache_control),
            &max_age_s)) {
        uint32_t age_s = 0U;
        if (parse_decimal_u32(response.age, &age_s)) {
            max_age_s = age_s >= max_age_s ? 0U : max_age_s - age_s;
        }
        result->max_age_s = max_age_s;
    }
    result->data = response.body;
    result->length = response.body_length;
    result->handle = response.body;
    return JETKVM_OIDC_OK;
}

static void esp_release_jwks(void *context,
                             jetkvm_oidc_jwks_response_t *response)
{
    (void)context;
    if (!response) {
        return;
    }
    cJSON_free(response->handle);
    memset(response, 0, sizeof(*response));
}

void jetkvm_oidc_default_config(jetkvm_oidc_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->http_timeout_ms = OIDC_DEFAULT_HTTP_TIMEOUT_MS;
    config->lock_timeout_ms = OIDC_DEFAULT_LOCK_TIMEOUT_MS;
    jetkvm_oidc_core_default_config(&config->core);
}

jetkvm_oidc_status_t jetkvm_oidc_validator_create(
    const jetkvm_oidc_config_t *configuration,
    jetkvm_oidc_validator_t **validator_out)
{
    if (!validator_out) {
        return JETKVM_OIDC_INVALID_ARGUMENT;
    }
    *validator_out = NULL;
    jetkvm_oidc_config_t defaults;
    if (!configuration) {
        jetkvm_oidc_default_config(&defaults);
        configuration = &defaults;
    }
    if (configuration->http_timeout_ms < OIDC_MIN_HTTP_TIMEOUT_MS ||
        configuration->http_timeout_ms > OIDC_MAX_HTTP_TIMEOUT_MS ||
        configuration->lock_timeout_ms == 0U ||
        configuration->lock_timeout_ms > OIDC_MAX_LOCK_TIMEOUT_MS) {
        return JETKVM_OIDC_INVALID_ARGUMENT;
    }

    jetkvm_oidc_validator_t *validator =
        jetkvm_memory_policy_control_alloc(sizeof(*validator));
    if (!validator) {
        return JETKVM_OIDC_NO_MEMORY;
    }
    memset(validator, 0, sizeof(*validator));
    validator->http_timeout_ms = configuration->http_timeout_ms;
    validator->lock_timeout_ms = configuration->lock_timeout_ms;
    validator->mutex = xSemaphoreCreateMutex();
    if (!validator->mutex) {
        jetkvm_memory_policy_control_free(validator);
        return JETKVM_OIDC_NO_MEMORY;
    }
    jetkvm_oidc_status_t status = jetkvm_oidc_core_validator_init(
        &validator->core, &configuration->core,
        esp_fetch_jwks, esp_release_jwks, validator,
        jetkvm_oidc_mbedtls_verify_rs256, NULL);
    if (status != JETKVM_OIDC_OK) {
        vSemaphoreDelete(validator->mutex);
        jetkvm_memory_policy_control_free(validator);
        return status;
    }
    *validator_out = validator;
    return JETKVM_OIDC_OK;
}

void jetkvm_oidc_validator_destroy(jetkvm_oidc_validator_t *validator)
{
    if (!validator) {
        return;
    }
    jetkvm_oidc_core_validator_invalidate(&validator->core);
    vSemaphoreDelete(validator->mutex);
    memset(validator, 0, sizeof(*validator));
    jetkvm_memory_policy_control_free(validator);
}

static jetkvm_oidc_status_t validate(
    jetkvm_oidc_validator_t *validator,
    jetkvm_oidc_validation_mode_t mode,
    const char *id_token, const char *client_id,
    char *identity, size_t identity_capacity)
{
    if (!validator || !id_token || !identity || identity_capacity == 0U) {
        return JETKVM_OIDC_INVALID_ARGUMENT;
    }
    identity[0] = '\0';
    size_t token_length = bounded_length(
        id_token, JETKVM_OIDC_MAX_TOKEN_BYTES + 1U);
    if (token_length == 0U) {
        return JETKVM_OIDC_MALFORMED_TOKEN;
    }
    if (token_length > JETKVM_OIDC_MAX_TOKEN_BYTES) {
        return JETKVM_OIDC_INPUT_TOO_LARGE;
    }
    TickType_t lock_ticks = pdMS_TO_TICKS(validator->lock_timeout_ms);
    if (lock_ticks == 0U) {
        lock_ticks = 1U;
    }
    if (xSemaphoreTake(validator->mutex, lock_ticks) != pdTRUE) {
        return JETKVM_OIDC_BUSY;
    }
    time_t now = time(NULL);
    int64_t monotonic_us = esp_timer_get_time();
    jetkvm_oidc_status_t status = jetkvm_oidc_core_validate_at(
        &validator->core, mode, id_token, token_length, client_id,
        (int64_t)now,
        monotonic_us < 0 ? 0U : (uint64_t)monotonic_us / 1000U,
        identity, identity_capacity);
    xSemaphoreGive(validator->mutex);
    if (status != JETKVM_OIDC_OK) {
        ESP_LOGW("oidc", "token rejected: %s", jetkvm_oidc_status_name(status));
    }
    return status;
}

jetkvm_oidc_status_t jetkvm_oidc_validate_registration(
    jetkvm_oidc_validator_t *validator,
    const char *id_token, const char *client_id,
    char *identity, size_t identity_capacity)
{
    if (!client_id) {
        if (identity && identity_capacity > 0U) {
            identity[0] = '\0';
        }
        return JETKVM_OIDC_INVALID_ARGUMENT;
    }
    return validate(validator, JETKVM_OIDC_REGISTRATION,
                    id_token, client_id, identity, identity_capacity);
}

jetkvm_oidc_status_t jetkvm_oidc_validate_session(
    jetkvm_oidc_validator_t *validator,
    const char *id_token,
    char *identity, size_t identity_capacity)
{
    return validate(validator, JETKVM_OIDC_SESSION,
                    id_token, NULL, identity, identity_capacity);
}

void jetkvm_oidc_validator_invalidate_keys(
    jetkvm_oidc_validator_t *validator)
{
    if (!validator) {
        return;
    }
    TickType_t lock_ticks = pdMS_TO_TICKS(validator->lock_timeout_ms);
    if (lock_ticks == 0U) {
        lock_ticks = 1U;
    }
    if (xSemaphoreTake(validator->mutex, lock_ticks) == pdTRUE) {
        jetkvm_oidc_core_validator_invalidate(&validator->core);
        xSemaphoreGive(validator->mutex);
    }
}

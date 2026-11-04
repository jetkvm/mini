/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_cloud_registration.h"
#include "jetkvm_cloud_registration_url.h"

#include <stdbool.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "sdkconfig.h"

typedef struct {
    char *response;
    size_t capacity;
    size_t length;
    bool overflow;
} http_response_buffer_t;

static void secure_zero(void *value, size_t size)
{
    volatile unsigned char *bytes = value;
    while (size-- > 0U) {
        *bytes++ = 0U;
    }
}

static esp_err_t registration_http_event(esp_http_client_event_t *event)
{
    if (!event || event->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    http_response_buffer_t *buffer = event->user_data;
    if (!buffer || event->data_len < 0 ||
        (event->data_len > 0 && !event->data)) {
        return ESP_FAIL;
    }
    size_t chunk_length = (size_t)event->data_len;
    if (buffer->overflow || buffer->capacity == 0U ||
        chunk_length > buffer->capacity - 1U - buffer->length) {
        buffer->overflow = true;
        return ESP_OK;
    }
    memcpy(buffer->response + buffer->length, event->data, chunk_length);
    buffer->length += chunk_length;
    buffer->response[buffer->length] = '\0';
    return ESP_OK;
}

static esp_err_t exchange_token_https(
    void *context, const char *url, const char *json_body,
    char *response, size_t response_capacity, size_t *response_length,
    int *http_status)
{
    (void)context;
    if (!url || !json_body || !response || response_capacity == 0U ||
        !response_length || !http_status ||
        !cloud_registration_url_allowed(url)) {
        return ESP_ERR_INVALID_ARG;
    }
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    http_response_buffer_t response_buffer = {
        .response = response,
        .capacity = response_capacity,
    };
    response[0] = '\0';
    *response_length = 0U;
    *http_status = 0;

    const esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = (int)JETKVM_CLOUD_REGISTRATION_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .event_handler = registration_http_event,
        .user_data = &response_buffer,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = false,
        .common_name = NULL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_set_header(
        client, "Content-Type", "application/json");
    if (err == ESP_OK) {
        size_t body_length = strlen(json_body);
        if (body_length > (size_t)INT_MAX) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = esp_http_client_set_post_field(
                client, json_body, (int)body_length);
        }
    }
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }
    if (err == ESP_OK) {
        *http_status = esp_http_client_get_status_code(client);
        *response_length = response_buffer.length;
        if (*http_status == 200 && response_buffer.overflow) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    esp_http_client_cleanup(client);
    return err;
#endif
}

static esp_err_t save_registration(
    void *context, const jetkvm_cloud_config_t *config)
{
    (void)context;
    return jetkvm_cloud_config_save(config);
}

static void initialize_invalid_result(
    jetkvm_cloud_registration_result_t *result)
{
    if (result) {
        *result = (jetkvm_cloud_registration_result_t) {
            .status = JETKVM_CLOUD_REGISTRATION_INVALID_ARGUMENT,
            .response_http_status = 400,
            .cause = ESP_ERR_INVALID_ARG,
        };
    }
}

jetkvm_cloud_registration_status_t
jetkvm_cloud_registration_register_snapshot(
    const jetkvm_cloud_config_t *current_config,
    const jetkvm_cloud_registration_request_t *request,
    jetkvm_cloud_registration_validate_oidc_fn validate_oidc,
    void *oidc_context,
    jetkvm_cloud_registration_save_fn save,
    void *save_context,
    jetkvm_cloud_config_t *registered_config,
    jetkvm_cloud_registration_result_t *result)
{
    initialize_invalid_result(result);
    if (!current_config || !request || !validate_oidc || !save ||
        !registered_config) {
        return JETKVM_CLOUD_REGISTRATION_INVALID_ARGUMENT;
    }

    const jetkvm_cloud_registration_operations_t operations = {
        .exchange = exchange_token_https,
        .validate_oidc = validate_oidc,
        .oidc_context = oidc_context,
        .save = save,
        .save_context = save_context,
    };
    jetkvm_cloud_registration_status_t status =
        jetkvm_cloud_registration_run_transaction(
            current_config, request, &operations,
            registered_config, result);
    if (status != JETKVM_CLOUD_REGISTRATION_OK && result) {
        ESP_LOGW("cloud_register", "registration failed: %s (%s, HTTP %d)",
                 jetkvm_cloud_registration_status_name(status),
                 esp_err_to_name(result->cause),
                 result->response_http_status);
    }
    return status;
}

jetkvm_cloud_registration_status_t jetkvm_cloud_registration_register(
    const jetkvm_cloud_registration_request_t *request,
    jetkvm_cloud_registration_validate_oidc_fn validate_oidc,
    void *oidc_context,
    jetkvm_cloud_config_t *registered_config,
    jetkvm_cloud_registration_result_t *result)
{
    initialize_invalid_result(result);
    if (!request || !validate_oidc || !registered_config) {
        return JETKVM_CLOUD_REGISTRATION_INVALID_ARGUMENT;
    }

    jetkvm_cloud_config_t *current_config =
        cJSON_malloc(sizeof(*current_config));
    if (!current_config) {
        if (result) {
            *result = (jetkvm_cloud_registration_result_t) {
                .status = JETKVM_CLOUD_REGISTRATION_OUT_OF_MEMORY,
                .response_http_status = 500,
                .cause = ESP_ERR_NO_MEM,
            };
        }
        return JETKVM_CLOUD_REGISTRATION_OUT_OF_MEMORY;
    }
    memset(current_config, 0, sizeof(*current_config));
    esp_err_t load_result = jetkvm_cloud_config_load(current_config);
    if (load_result != ESP_OK) {
        if (result) {
            *result = (jetkvm_cloud_registration_result_t) {
                .status = JETKVM_CLOUD_REGISTRATION_CONFIG_LOAD_FAILED,
                .response_http_status = 500,
                .cause = load_result,
            };
        }
        secure_zero(current_config, sizeof(*current_config));
        cJSON_free(current_config);
        return JETKVM_CLOUD_REGISTRATION_CONFIG_LOAD_FAILED;
    }

    jetkvm_cloud_registration_status_t status =
        jetkvm_cloud_registration_register_snapshot(
        current_config, request, validate_oidc, oidc_context,
        save_registration, NULL, registered_config, result);
    secure_zero(current_config, sizeof(*current_config));
    cJSON_free(current_config);
    return status;
}

esp_err_t jetkvm_cloud_registration_delete_remote(
    const jetkvm_cloud_config_t *config, const char *device_id)
{
    if (!config || !jetkvm_cloud_config_is_registered(config) ||
        !cloud_registration_url_allowed(config->cloud_url) ||
        !device_id || strlen(device_id) != 12U ||
        strspn(device_id, "0123456789abcdef") != 12U) {
        return ESP_ERR_INVALID_ARG;
    }
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    char url[JETKVM_CLOUD_URL_CAPACITY + 32U];
    char authorization[JETKVM_CLOUD_TOKEN_CAPACITY + 8U];
    snprintf(url, sizeof(url), "%s/devices/%s", config->cloud_url, device_id);
    snprintf(authorization, sizeof(authorization), "Bearer %s", config->cloud_token);
    const esp_http_client_config_t http_config = {
        .url = url,
        .method = HTTP_METHOD_DELETE,
        .timeout_ms = JETKVM_CLOUD_REGISTRATION_HTTP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    esp_err_t err = client ? esp_http_client_set_header(
        client, "Authorization", authorization) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status != 404 && (status < 200 || status >= 300)) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (client) {
        esp_http_client_cleanup(client);
    }
    secure_zero(authorization, sizeof(authorization));
    return err;
#endif
}

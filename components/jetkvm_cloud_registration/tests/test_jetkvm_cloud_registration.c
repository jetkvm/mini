/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "jetkvm_cloud_registration.h"
#include "mock_nvs.h"

typedef struct {
    int exchange_calls;
    int oidc_calls;
    int save_calls;
    esp_err_t exchange_result;
    int upstream_status;
    const char *response;
    size_t response_length;
    jetkvm_cloud_registration_oidc_status_t oidc_result;
    const char *identity;
    esp_err_t save_result;
    jetkvm_cloud_config_t persisted;
    char observed_url[600];
    char observed_body[1400];
} mock_context_t;

static esp_err_t mock_exchange(
    void *opaque, const char *url, const char *json_body,
    char *response, size_t response_capacity, size_t *response_length,
    int *http_status)
{
    mock_context_t *context = opaque;
    ++context->exchange_calls;
    snprintf(context->observed_url, sizeof(context->observed_url), "%s", url);
    snprintf(context->observed_body, sizeof(context->observed_body), "%s",
             json_body);
    *http_status = context->upstream_status;
    if (context->exchange_result != ESP_OK) {
        return context->exchange_result;
    }
    size_t length = context->response_length != 0U
                        ? context->response_length
                        : strlen(context->response ? context->response : "");
    if (length >= response_capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(response, context->response ? context->response : "", length + 1U);
    *response_length = length;
    return ESP_OK;
}

static jetkvm_cloud_registration_oidc_status_t mock_oidc(
    void *opaque, const char *oidc_google, const char *client_id,
    char *identity, size_t identity_capacity)
{
    mock_context_t *context = opaque;
    ++context->oidc_calls;
    assert(strcmp(oidc_google, "header.payload.signature") == 0);
    assert(strcmp(client_id, "jetkvm-client") == 0);
    if (context->oidc_result == JETKVM_CLOUD_REGISTRATION_OIDC_OK) {
        size_t length = strlen(context->identity);
        assert(length < identity_capacity);
        memcpy(identity, context->identity, length + 1U);
    }
    return context->oidc_result;
}

static esp_err_t mock_save(
    void *opaque, const jetkvm_cloud_config_t *config)
{
    mock_context_t *context = opaque;
    ++context->save_calls;
    if (context->save_result == ESP_OK) {
        context->persisted = *config;
    }
    return context->save_result;
}

static esp_err_t config_nvs_save(
    void *opaque, const jetkvm_cloud_config_t *config)
{
    mock_context_t *context = opaque;
    ++context->save_calls;
    return jetkvm_cloud_config_save(config);
}

static void make_fixture(
    jetkvm_cloud_config_t *current,
    jetkvm_cloud_registration_request_t *request,
    mock_context_t *context,
    jetkvm_cloud_registration_operations_t *operations)
{
    jetkvm_cloud_config_defaults(current);
    *request = (jetkvm_cloud_registration_request_t) {
        .token = "temporary-token",
        .cloud_api = "https://attacker.invalid/ignored",
        .oidc_google = "header.payload.signature",
        .client_id = "jetkvm-client",
    };
    memset(context, 0, sizeof(*context));
    context->upstream_status = 200;
    context->response = "{\"secretToken\":\"opaque-permanent-token\"}";
    context->oidc_result = JETKVM_CLOUD_REGISTRATION_OIDC_OK;
    context->identity = "jetkvm-client:user-123";
    context->save_result = ESP_OK;
    context->persisted = *current;
    *operations = (jetkvm_cloud_registration_operations_t) {
        .exchange = mock_exchange,
        .exchange_context = context,
        .validate_oidc = mock_oidc,
        .oidc_context = context,
        .save = mock_save,
        .save_context = context,
    };
}

static void test_success_exact_request_and_commit(void)
{
    jetkvm_cloud_config_t current;
    jetkvm_cloud_registration_request_t request;
    mock_context_t context;
    jetkvm_cloud_registration_operations_t operations;
    make_fixture(&current, &request, &context, &operations);
    jetkvm_cloud_config_t output;
    jetkvm_cloud_registration_result_t result;

    assert(jetkvm_cloud_registration_run_transaction(
               &current, &request, &operations, &output, &result) ==
           JETKVM_CLOUD_REGISTRATION_OK);
    assert(result.response_http_status == 200);
    assert(result.upstream_http_status == 200);
    assert(context.exchange_calls == 1);
    assert(context.oidc_calls == 1);
    assert(context.save_calls == 1);
    assert(strcmp(context.observed_url,
                  "https://api.jetkvm.com/devices/token") == 0);
    assert(strcmp(context.observed_body,
                  "{\"tempToken\":\"temporary-token\"}") == 0);
    assert(strstr(context.observed_url, "attacker.invalid") == NULL);
    assert(strcmp(output.cloud_token, "opaque-permanent-token") == 0);
    assert(strcmp(output.google_identity, "jetkvm-client:user-123") == 0);
    assert(memcmp(&output, &context.persisted, sizeof(output)) == 0);
    assert(!jetkvm_cloud_config_is_registered(&current));
}

static void test_json_escapes_temporary_token(void)
{
    jetkvm_cloud_config_t current;
    jetkvm_cloud_registration_request_t request;
    mock_context_t context;
    jetkvm_cloud_registration_operations_t operations;
    make_fixture(&current, &request, &context, &operations);
    request.token = "quote\\\"and\\\\slash";
    jetkvm_cloud_config_t output;
    assert(jetkvm_cloud_registration_run_transaction(
               &current, &request, &operations, &output, NULL) ==
           JETKVM_CLOUD_REGISTRATION_OK);
    assert(strcmp(context.observed_body,
                  "{\"tempToken\":\"quote\\\\\\\"and\\\\\\\\slash\"}") == 0);
}

static void test_upstream_status_is_passed_through(void)
{
    jetkvm_cloud_config_t current;
    jetkvm_cloud_registration_request_t request;
    mock_context_t context;
    jetkvm_cloud_registration_operations_t operations;
    make_fixture(&current, &request, &context, &operations);
    context.upstream_status = 429;
    context.response = "arbitrary upstream error";
    jetkvm_cloud_config_t output;
    jetkvm_cloud_registration_result_t result;

    assert(jetkvm_cloud_registration_run_transaction(
               &current, &request, &operations, &output, &result) ==
           JETKVM_CLOUD_REGISTRATION_UPSTREAM_HTTP_ERROR);
    assert(result.response_http_status == 429);
    assert(result.upstream_http_status == 429);
    assert(context.oidc_calls == 0);
    assert(context.save_calls == 0);
    assert(memcmp(&output, &current, sizeof(output)) == 0);
    assert(memcmp(&context.persisted, &current, sizeof(current)) == 0);
}

static void test_empty_input_token_stops_before_exchange(void)
{
    jetkvm_cloud_config_t current;
    jetkvm_cloud_registration_request_t request;
    mock_context_t context;
    jetkvm_cloud_registration_operations_t operations;
    make_fixture(&current, &request, &context, &operations);
    request.token = "";
    jetkvm_cloud_config_t output;
    memset(&output, 0xa5, sizeof(output));
    jetkvm_cloud_registration_result_t result;

    assert(jetkvm_cloud_registration_run_transaction(
               &current, &request, &operations, &output, &result) ==
           JETKVM_CLOUD_REGISTRATION_INVALID_ARGUMENT);
    assert(result.response_http_status == 400);
    assert(context.exchange_calls == 0);
    assert(context.oidc_calls == 0);
    assert(context.save_calls == 0);
    assert(memcmp(&output, &current, sizeof(output)) == 0);
    assert(memcmp(&context.persisted, &current, sizeof(current)) == 0);
}

static void test_empty_secret_token_is_internal_failure(void)
{
    jetkvm_cloud_config_t current;
    jetkvm_cloud_registration_request_t request;
    mock_context_t context;
    jetkvm_cloud_registration_operations_t operations;
    make_fixture(&current, &request, &context, &operations);
    context.response = "{\"secretToken\":\"\"}";
    jetkvm_cloud_config_t output;
    jetkvm_cloud_registration_result_t result;

    assert(jetkvm_cloud_registration_run_transaction(
               &current, &request, &operations, &output, &result) ==
           JETKVM_CLOUD_REGISTRATION_INVALID_TOKEN_RESPONSE);
    assert(result.response_http_status == 500);
    assert(context.oidc_calls == 0);
    assert(context.save_calls == 0);
    assert(memcmp(&output, &current, sizeof(output)) == 0);
}

static void test_embedded_nul_cannot_hide_trailing_response_bytes(void)
{
    static const char response[] =
        "{\"secretToken\":\"apparently-valid\"}\0trailing";
    jetkvm_cloud_config_t current;
    jetkvm_cloud_registration_request_t request;
    mock_context_t context;
    jetkvm_cloud_registration_operations_t operations;
    make_fixture(&current, &request, &context, &operations);
    context.response = response;
    context.response_length = sizeof(response) - 1U;
    jetkvm_cloud_config_t output;

    assert(jetkvm_cloud_registration_run_transaction(
               &current, &request, &operations, &output, NULL) ==
           JETKVM_CLOUD_REGISTRATION_INVALID_TOKEN_RESPONSE);
    assert(context.oidc_calls == 0);
    assert(context.save_calls == 0);
    assert(memcmp(&output, &current, sizeof(output)) == 0);
}

static void test_oidc_rejection_does_not_persist_exchanged_token(void)
{
    jetkvm_cloud_config_t current;
    jetkvm_cloud_registration_request_t request;
    mock_context_t context;
    jetkvm_cloud_registration_operations_t operations;
    make_fixture(&current, &request, &context, &operations);
    context.oidc_result = JETKVM_CLOUD_REGISTRATION_OIDC_INVALID;
    jetkvm_cloud_config_t output;
    jetkvm_cloud_registration_result_t result;

    assert(jetkvm_cloud_registration_run_transaction(
               &current, &request, &operations, &output, &result) ==
           JETKVM_CLOUD_REGISTRATION_OIDC_REJECTED);
    assert(result.response_http_status == 400);
    assert(context.exchange_calls == 1);
    assert(context.oidc_calls == 1);
    assert(context.save_calls == 0);
    assert(memcmp(&output, &current, sizeof(output)) == 0);
    assert(memcmp(&context.persisted, &current, sizeof(current)) == 0);
}

static void test_save_failure_has_no_half_registration(void)
{
    jetkvm_cloud_config_t current;
    jetkvm_cloud_registration_request_t request;
    mock_context_t context;
    jetkvm_cloud_registration_operations_t operations;
    make_fixture(&current, &request, &context, &operations);
    context.save_result = ESP_FAIL;
    jetkvm_cloud_config_t output;
    jetkvm_cloud_registration_result_t result;

    assert(jetkvm_cloud_registration_run_transaction(
               &current, &request, &operations, &output, &result) ==
           JETKVM_CLOUD_REGISTRATION_PERSIST_FAILED);
    assert(result.response_http_status == 500);
    assert(context.exchange_calls == 1);
    assert(context.oidc_calls == 1);
    assert(context.save_calls == 1);
    assert(memcmp(&output, &current, sizeof(output)) == 0);
    assert(memcmp(&context.persisted, &current, sizeof(current)) == 0);
}

static void test_nvs_commit_failure_keeps_previous_single_blob(void)
{
    mock_nvs_reset();
    jetkvm_cloud_config_t current;
    jetkvm_cloud_registration_request_t request;
    mock_context_t context;
    jetkvm_cloud_registration_operations_t operations;
    make_fixture(&current, &request, &context, &operations);
    assert(jetkvm_cloud_config_save(&current) == ESP_OK);
    size_t original_blob_size = mock_nvs_committed_size();
    assert(original_blob_size > 0U);

    operations.save = config_nvs_save;
    mock_nvs_fail_next_commit();
    jetkvm_cloud_config_t output;
    jetkvm_cloud_registration_result_t result;
    assert(jetkvm_cloud_registration_run_transaction(
               &current, &request, &operations, &output, &result) ==
           JETKVM_CLOUD_REGISTRATION_PERSIST_FAILED);
    assert(context.save_calls == 1);
    assert(mock_nvs_committed_size() == original_blob_size);
    assert(memcmp(&output, &current, sizeof(output)) == 0);

    jetkvm_cloud_config_t loaded;
    assert(jetkvm_cloud_config_load(&loaded) == ESP_OK);
    assert(memcmp(&loaded, &current, sizeof(loaded)) == 0);
    assert(!jetkvm_cloud_config_is_registered(&loaded));
}

static void test_oversized_response_is_rejected(void)
{
    jetkvm_cloud_config_t current;
    jetkvm_cloud_registration_request_t request;
    mock_context_t context;
    jetkvm_cloud_registration_operations_t operations;
    make_fixture(&current, &request, &context, &operations);
    context.exchange_result = ESP_ERR_INVALID_SIZE;
    jetkvm_cloud_config_t output;
    jetkvm_cloud_registration_result_t result;

    assert(jetkvm_cloud_registration_run_transaction(
               &current, &request, &operations, &output, &result) ==
           JETKVM_CLOUD_REGISTRATION_RESPONSE_TOO_LARGE);
    assert(result.response_http_status == 500);
    assert(context.oidc_calls == 0);
    assert(context.save_calls == 0);
}

/* Only a mock cloud build (-DJETKVM_MOCK_CLOUD=1) registers over http://. */
static void test_http_cloud_url_only_in_mock_cloud_builds(void)
{
    jetkvm_cloud_config_t current;
    jetkvm_cloud_registration_request_t request;
    mock_context_t context;
    jetkvm_cloud_registration_operations_t operations;
    make_fixture(&current, &request, &context, &operations);
    assert(jetkvm_cloud_config_set_urls(&current, "http://192.0.2.1:8790",
                                        "http://192.0.2.1:8790") == ESP_OK);
    jetkvm_cloud_config_t output;
    jetkvm_cloud_registration_status_t status =
        jetkvm_cloud_registration_run_transaction(
            &current, &request, &operations, &output, NULL);
#if JETKVM_MOCK_CLOUD
    assert(status == JETKVM_CLOUD_REGISTRATION_OK);
    assert(strcmp(context.observed_url,
                  "http://192.0.2.1:8790/devices/token") == 0);
#else
    assert(status == JETKVM_CLOUD_REGISTRATION_INVALID_CONFIG);
    assert(context.exchange_calls == 0);
#endif
}

static void test_public_ip_trace_parse(void)
{
    char ip[46];
    int64_t seen_at = 0;
    assert(jetkvm_cloud_public_ip_parse(
        "fl=12f\nh=api.jetkvm.com\nip=203.0.113.7\nts=1790000000.123\nloc=DE\n",
        ip, sizeof(ip), &seen_at));
    assert(strcmp(ip, "203.0.113.7") == 0 && seen_at == 1790000000);
    assert(jetkvm_cloud_public_ip_parse("ip=2001:db8::1", ip, sizeof(ip), &seen_at));
    assert(strcmp(ip, "2001:db8::1") == 0 && seen_at == 0);
    assert(!jetkvm_cloud_public_ip_parse("ip=<script>\nts=1\n", ip, sizeof(ip), &seen_at));
    assert(!jetkvm_cloud_public_ip_parse("<html>not a trace</html>", ip, sizeof(ip), &seen_at));
}

int main(void)
{
    test_public_ip_trace_parse();
    test_success_exact_request_and_commit();
    test_json_escapes_temporary_token();
    test_upstream_status_is_passed_through();
    test_empty_input_token_stops_before_exchange();
    test_empty_secret_token_is_internal_failure();
    test_embedded_nul_cannot_hide_trailing_response_bytes();
    test_oidc_rejection_does_not_persist_exchanged_token();
    test_save_failure_has_no_half_registration();
    test_nvs_commit_failure_keeps_previous_single_blob();
    test_oversized_response_is_rejected();
    test_http_cloud_url_only_in_mock_cloud_builds();
    puts("jetkvm cloud registration tests passed");
    return 0;
}

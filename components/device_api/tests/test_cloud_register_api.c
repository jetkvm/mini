#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_api.h"
#include "esp_app_desc.h"
#include "jetkvm_access.h"
#include "jetkvm_factory_reset.h"
#include "jetkvm_cloud_registration.h"
#include "jetkvm_device_state.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_oidc.h"
#include "jetkvm_time_sync.h"

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t app = { .version = "0.5.9-dev202610021009" };
    return &app;
}

typedef struct {
    const char *input;
    size_t input_length;
    size_t input_offset;
    size_t receive_chunk;
    unsigned receive_calls;
    char status[64];
    char type[64];
    char body[256];
    unsigned sends;
} request_io_t;

static httpd_uri_t s_handlers[8];
static size_t s_handler_count;
static int s_register_fail_at;
static unsigned s_unregister_calls;

static esp_err_t s_snapshot_result;
static uint32_t s_snapshot_generation;
static char s_snapshot_url[JETKVM_CLOUD_URL_CAPACITY];
static unsigned s_snapshot_calls;

static esp_err_t s_time_start_result;
static esp_err_t s_time_wait_result;
static unsigned s_time_start_calls;
static unsigned s_time_wait_calls;
static TickType_t s_time_wait_ticks;

static jetkvm_oidc_status_t s_oidc_result;
static unsigned s_oidc_calls;
static char s_oidc_identity[JETKVM_CLOUD_IDENTITY_CAPACITY];

static unsigned s_registration_calls;
static int s_forced_upstream_status;
static unsigned s_cas_calls;
static esp_err_t s_cas_result;
static uint32_t s_cas_generation;
static char s_cas_url[JETKVM_CLOUD_URL_CAPACITY];
static char s_cas_token[JETKVM_CLOUD_TOKEN_CAPACITY];
static char s_cas_identity[JETKVM_CLOUD_IDENTITY_CAPACITY];
static char s_seen_token[128];
static char s_seen_cloud_api[128];
static char s_seen_oidc[128];
static char s_seen_client_id[128];

static unsigned char s_validator_storage;
static bool s_memory_reservation_allowed;
static unsigned s_memory_reserve_calls;
static unsigned s_memory_release_calls;
static unsigned s_memory_reservations_active;

static void copy_checked(char *output, size_t output_capacity,
                         const char *input)
{
    size_t length = strlen(input);
    assert(length < output_capacity);
    memcpy(output, input, length + 1U);
}

static void reset_mocks(void)
{
    assert(s_memory_reservations_active == 0U);
    s_memory_reservation_allowed = true;
    s_memory_reserve_calls = 0U;
    s_memory_release_calls = 0U;
    s_snapshot_result = ESP_OK;
    s_snapshot_generation = 77U;
    copy_checked(s_snapshot_url, sizeof(s_snapshot_url),
                 "https://trusted.example");
    s_snapshot_calls = 0U;
    s_time_start_result = ESP_OK;
    s_time_wait_result = ESP_OK;
    s_time_start_calls = 0U;
    s_time_wait_calls = 0U;
    s_time_wait_ticks = 0U;
    s_oidc_result = JETKVM_OIDC_OK;
    s_oidc_calls = 0U;
    copy_checked(s_oidc_identity, sizeof(s_oidc_identity),
                 "client-id:google-subject");
    s_registration_calls = 0U;
    s_forced_upstream_status = 0;
    s_cas_calls = 0U;
    s_cas_result = ESP_OK;
    s_cas_generation = 0U;
    memset(s_cas_url, 0, sizeof(s_cas_url));
    memset(s_cas_token, 0, sizeof(s_cas_token));
    memset(s_cas_identity, 0, sizeof(s_cas_identity));
    memset(s_seen_token, 0, sizeof(s_seen_token));
    memset(s_seen_cloud_api, 0, sizeof(s_seen_cloud_api));
    memset(s_seen_oidc, 0, sizeof(s_seen_oidc));
    memset(s_seen_client_id, 0, sizeof(s_seen_client_id));
}

bool jetkvm_memory_policy_wait_reserve_tls(
    const char *owner, jetkvm_memory_policy_reservation_t *reservation)
{
    ++s_memory_reserve_calls;
    assert(strcmp(owner, "cloud-registration-tls") == 0);
    assert(reservation != NULL && !reservation->active);
    if (!s_memory_reservation_allowed) {
        return false;
    }
    reservation->generation = 1U;
    reservation->active = true;
    ++s_memory_reservations_active;
    return true;
}

void jetkvm_memory_policy_release(
    jetkvm_memory_policy_reservation_t *reservation)
{
    if (reservation != NULL && reservation->active) {
        assert(s_memory_reservations_active > 0U);
        --s_memory_reservations_active;
        ++s_memory_release_calls;
        memset(reservation, 0, sizeof(*reservation));
    }
}

esp_err_t httpd_register_uri_handler(httpd_handle_t server,
                                     const httpd_uri_t *uri)
{
    assert(server != NULL);
    assert(uri != NULL);
    if (s_register_fail_at > 0 &&
        (int)(s_handler_count + 1U) == s_register_fail_at) {
        return ESP_FAIL;
    }
    assert(s_handler_count < ARRAY_SIZE(s_handlers));
    s_handlers[s_handler_count++] = *uri;
    return ESP_OK;
}

esp_err_t httpd_unregister_uri_handler(httpd_handle_t server,
                                       const char *uri,
                                       httpd_method_t method)
{
    assert(server != NULL);
    assert(uri != NULL);
    (void)method;
    ++s_unregister_calls;
    return ESP_OK;
}

int httpd_req_recv(httpd_req_t *request, char *buffer, size_t length)
{
    request_io_t *io = request->aux;
    ++io->receive_calls;
    if (io->input_offset >= io->input_length) {
        return 0;
    }
    size_t available = io->input_length - io->input_offset;
    size_t chunk = io->receive_chunk && io->receive_chunk < length
        ? io->receive_chunk : length;
    if (chunk > available) {
        chunk = available;
    }
    memcpy(buffer, io->input + io->input_offset, chunk);
    io->input_offset += chunk;
    return (int)chunk;
}

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    request_io_t *io = request->aux;
    copy_checked(io->type, sizeof(io->type), type);
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *name,
                             const char *value)
{
    (void)request;
    assert(name != NULL);
    assert(value != NULL);
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    request_io_t *io = request->aux;
    copy_checked(io->status, sizeof(io->status), status);
    return ESP_OK;
}

esp_err_t httpd_resp_sendstr(httpd_req_t *request, const char *body)
{
    /* Response latency is outside the TLS/OIDC transaction window. */
    assert(s_memory_reservations_active == 0U);
    request_io_t *io = request->aux;
    copy_checked(io->body, sizeof(io->body), body);
    ++io->sends;
    return ESP_OK;
}

esp_err_t httpd_resp_send_err(httpd_req_t *request, int code,
                              const char *message)
{
    char status[32];
    snprintf(status, sizeof(status), "%d Error", code);
    assert(httpd_resp_set_status(request, status) == ESP_OK);
    return httpd_resp_sendstr(request, message);
}

esp_err_t jetkvm_device_state_get_id(char *output, size_t output_capacity)
{
    if (output_capacity < JETKVM_DEVICE_ID_CAPACITY) {
        return ESP_ERR_INVALID_ARG;
    }
    copy_checked(output, output_capacity, "001122334455");
    return ESP_OK;
}

esp_err_t jetkvm_device_state_cloud_public_snapshot(
    jetkvm_device_cloud_public_state_t *state)
{
    memset(state, 0, sizeof(*state));
    copy_checked(state->cloud_url, sizeof(state->cloud_url), s_snapshot_url);
    copy_checked(state->cloud_app_url, sizeof(state->cloud_app_url),
                 "https://app.example");
    state->generation = s_snapshot_generation;
    return ESP_OK;
}

esp_err_t jetkvm_device_state_cloud_snapshot(
    jetkvm_cloud_config_t *config, uint32_t *generation)
{
    ++s_snapshot_calls;
    if (s_snapshot_result != ESP_OK) {
        return s_snapshot_result;
    }
    memset(config, 0, sizeof(*config));
    config->schema_version = JETKVM_CLOUD_CONFIG_SCHEMA_VERSION;
    copy_checked(config->cloud_url, sizeof(config->cloud_url),
                 s_snapshot_url);
    copy_checked(config->cloud_app_url, sizeof(config->cloud_app_url),
                 "https://app.example");
    *generation = s_snapshot_generation;
    return ESP_OK;
}

esp_err_t jetkvm_device_state_cloud_set_registration_if_current(
    uint32_t expected_generation, const char *expected_cloud_url,
    const char *cloud_token, const char *google_identity)
{
    ++s_cas_calls;
    s_cas_generation = expected_generation;
    copy_checked(s_cas_url, sizeof(s_cas_url), expected_cloud_url);
    copy_checked(s_cas_token, sizeof(s_cas_token), cloud_token);
    copy_checked(s_cas_identity, sizeof(s_cas_identity), google_identity);
    return s_cas_result;
}

esp_err_t jetkvm_time_sync_start(void)
{
    ++s_time_start_calls;
    return s_time_start_result;
}

esp_err_t jetkvm_time_sync_wait(TickType_t timeout)
{
    ++s_time_wait_calls;
    s_time_wait_ticks = timeout;
    return s_time_wait_result;
}

jetkvm_oidc_status_t jetkvm_oidc_validate_registration(
    jetkvm_oidc_validator_t *validator,
    const char *id_token, const char *client_id,
    char *identity, size_t identity_capacity)
{
    assert(validator == (jetkvm_oidc_validator_t *)&s_validator_storage);
    assert(strcmp(id_token, "header.payload.signature") == 0);
    assert(strcmp(client_id, "client-id") == 0);
    ++s_oidc_calls;
    if (s_oidc_result == JETKVM_OIDC_OK) {
        copy_checked(identity, identity_capacity, s_oidc_identity);
    }
    return s_oidc_result;
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
    ++s_registration_calls;
    copy_checked(s_seen_token, sizeof(s_seen_token), request->token);
    copy_checked(s_seen_cloud_api, sizeof(s_seen_cloud_api),
                 request->cloud_api);
    copy_checked(s_seen_oidc, sizeof(s_seen_oidc), request->oidc_google);
    copy_checked(s_seen_client_id, sizeof(s_seen_client_id),
                 request->client_id);
    assert(strcmp(current_config->cloud_url, s_snapshot_url) == 0);
    *registered_config = *current_config;
    if (s_forced_upstream_status != 0) {
        *result = (jetkvm_cloud_registration_result_t) {
            .status = JETKVM_CLOUD_REGISTRATION_UPSTREAM_HTTP_ERROR,
            .response_http_status = s_forced_upstream_status,
            .upstream_http_status = s_forced_upstream_status,
        };
        return JETKVM_CLOUD_REGISTRATION_UPSTREAM_HTTP_ERROR;
    }
    char identity[JETKVM_CLOUD_IDENTITY_CAPACITY] = { 0 };
    jetkvm_cloud_registration_oidc_status_t oidc = validate_oidc(
        oidc_context, request->oidc_google, request->client_id,
        identity, sizeof(identity));
    if (oidc == JETKVM_CLOUD_REGISTRATION_OIDC_INVALID) {
        result->response_http_status = 400;
        return result->status = JETKVM_CLOUD_REGISTRATION_OIDC_REJECTED;
    }
    if (oidc != JETKVM_CLOUD_REGISTRATION_OIDC_OK) {
        result->response_http_status = 500;
        return result->status = JETKVM_CLOUD_REGISTRATION_OIDC_FAILED;
    }
    copy_checked(registered_config->cloud_token,
                 sizeof(registered_config->cloud_token),
                 "permanent-secret");
    copy_checked(registered_config->google_identity,
                 sizeof(registered_config->google_identity), identity);
    esp_err_t saved = save(save_context, registered_config);
    if (saved != ESP_OK) {
        result->response_http_status = 500;
        result->cause = saved;
        return result->status = JETKVM_CLOUD_REGISTRATION_PERSIST_FAILED;
    }
    result->response_http_status = 200;
    return result->status = JETKVM_CLOUD_REGISTRATION_OK;
}

static httpd_uri_t *find_handler(const char *uri, httpd_method_t method)
{
    for (size_t i = 0U; i < s_handler_count; ++i) {
        if (s_handlers[i].method == method &&
            strcmp(s_handlers[i].uri, uri) == 0) {
            return &s_handlers[i];
        }
    }
    return NULL;
}

static request_io_t invoke(httpd_uri_t *handler, const char *body,
                           size_t body_length, size_t receive_chunk)
{
    request_io_t io = {
        .input = body,
        .input_length = body_length,
        .receive_chunk = receive_chunk,
    };
    httpd_req_t request = {
        .method = HTTP_POST,
        .content_len = body_length,
        .user_ctx = handler->user_ctx,
        .aux = &io,
    };
    assert(handler->handler(&request) == ESP_OK);
    assert(s_memory_reservations_active == 0U);
    assert(io.sends == 1U);
    assert(strcmp(io.type, "application/json; charset=utf-8") == 0);
    return io;
}

static void expect_status(const request_io_t *io, int status,
                          const char *body)
{
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "%d ", status);
    if (strncmp(io->status, prefix, strlen(prefix)) != 0 ||
        strcmp(io->body, body) != 0) {
        fprintf(stderr, "expected %s / %s, got %s / %s\n",
                prefix, body, io->status, io->body);
    }
    assert(strncmp(io->status, prefix, strlen(prefix)) == 0);
    assert(strcmp(io->body, body) == 0);
}

static const char s_valid_request[] =
    "{\"token\":\"temporary-token\","
    "\"cloudApi\":\"https://ignored.example\","
    "\"oidcGoogle\":\"header.payload.signature\","
    "\"clientId\":\"client-id\"}";

static void test_memory_admission_failure(httpd_uri_t *handler)
{
    reset_mocks();
    s_memory_reservation_allowed = false;
    request_io_t io = invoke(handler, s_valid_request,
                             strlen(s_valid_request), 0U);
    expect_status(&io, 503,
                  "{\"error\":\"Insufficient memory for cloud registration\"}");
    assert(io.receive_calls > 0U);
    assert(s_memory_reserve_calls == 1U);
    assert(s_memory_release_calls == 0U);
    assert(s_registration_calls == 0U);
}

static void test_registration_success(httpd_uri_t *handler)
{
    reset_mocks();
    request_io_t io = invoke(handler, s_valid_request,
                             strlen(s_valid_request), 16U);
    expect_status(&io, 200,
                  "{\"message\":\"Cloud registration successful\","
                  "\"cloudUiVersion\":\"0.5.9-dev202610021009-mini\"}");
    assert(io.receive_calls > 1U);
    assert(s_snapshot_calls == 1U);
    assert(s_time_start_calls == 1U && s_time_wait_calls == 1U);
    assert(s_time_wait_ticks == P4_DEVICE_API_CLOUD_REGISTER_TIME_WAIT_MS);
    assert(s_registration_calls == 1U && s_oidc_calls == 1U);
    assert(strcmp(s_seen_token, "temporary-token") == 0);
    assert(strcmp(s_seen_cloud_api, "https://ignored.example") == 0);
    assert(strcmp(s_seen_oidc, "header.payload.signature") == 0);
    assert(strcmp(s_seen_client_id, "client-id") == 0);
    assert(s_cas_calls == 1U);
    assert(s_cas_generation == 77U);
    assert(strcmp(s_cas_url, "https://trusted.example") == 0);
    assert(strcmp(s_cas_token, "permanent-secret") == 0);
    assert(strcmp(s_cas_identity, "client-id:google-subject") == 0);
}

static void *fail_response_allocation(size_t bytes)
{
    return s_memory_reservations_active ? NULL : malloc(bytes);
}

static void test_response_allocation_failure(httpd_uri_t *handler)
{
    reset_mocks();
    cJSON_Hooks hooks = {
        .malloc_fn = fail_response_allocation,
        .free_fn = free,
    };
    cJSON_InitHooks(&hooks);
    request_io_t io = invoke(handler, s_valid_request,
                             strlen(s_valid_request), 0U);
    cJSON_InitHooks(NULL);
    expect_status(&io, 500, "{\"error\":\"Cloud registration failed\"}");
    assert(s_memory_reserve_calls == 1U);
    assert(s_memory_release_calls == 1U);
    assert(s_registration_calls == 0U);
    assert(s_cas_calls == 0U);
}

static void test_invalid_requests(httpd_uri_t *handler)
{
    const char *invalid[] = {
        "{}",
        "{\"token\":\"a\",\"token\":\"b\","
            "\"oidcGoogle\":\"header.payload.signature\","
            "\"clientId\":\"client-id\"}",
        "{\"token\":\"a\",\"oidcGoogle\":"
            "\"header.payload.signature\",\"clientId\":12}",
        "{\"token\":\"\\u0000\",\"oidcGoogle\":"
            "\"header.payload.signature\",\"clientId\":\"client-id\"}",
        "{\"token\":\"a\",\"oidcGoogle\":"
            "\"header.payload.signature\",\"clientId\":\"client-id\","
            "\"x\":[[[[[[[[[]]]]]]]]]}",
    };
    for (size_t i = 0U; i < ARRAY_SIZE(invalid); ++i) {
        reset_mocks();
        request_io_t io = invoke(handler, invalid[i], strlen(invalid[i]), 0U);
        expect_status(&io, 400, "{\"error\":\"Invalid request body\"}");
        assert(s_registration_calls == 0U);
        assert(s_time_start_calls == 0U);
    }

    char embedded_nul[] = {
        '{', '"', 't', 'o', 'k', 'e', 'n', '"', ':', '"', 'a', '"', ',',
        '"', 'x', '"', ':', '"', '\0', '"', '}',
    };
    reset_mocks();
    request_io_t io = invoke(handler, embedded_nul,
                             sizeof(embedded_nul), 0U);
    expect_status(&io, 400, "{\"error\":\"Invalid request body\"}");
    assert(s_registration_calls == 0U);

    const char invalid_utf8[] =
        "{\"token\":\"\xc0\x80\",\"oidcGoogle\":\"x\","
        "\"clientId\":\"y\"}";
    reset_mocks();
    io = invoke(handler, invalid_utf8, sizeof(invalid_utf8) - 1U, 0U);
    expect_status(&io, 400, "{\"error\":\"Invalid request body\"}");

    reset_mocks();
    io = invoke(handler, s_valid_request,
                P4_DEVICE_API_CLOUD_REGISTER_BODY_MAX_LENGTH + 1U, 0U);
    expect_status(&io, 400, "{\"error\":\"Invalid request body\"}");
    assert(io.receive_calls == 0U);

    reset_mocks();
    io = invoke(handler, s_valid_request, strlen(s_valid_request), 1U);
    expect_status(&io, 400, "{\"error\":\"Invalid request body\"}");
    assert(io.receive_calls == 16U);
}

static void test_time_failures(httpd_uri_t *handler)
{
    reset_mocks();
    s_time_start_result = ESP_FAIL;
    request_io_t io = invoke(handler, s_valid_request,
                             strlen(s_valid_request), 0U);
    expect_status(&io, 500, "{\"error\":\"System time unavailable\"}");
    assert(s_time_wait_calls == 0U && s_registration_calls == 0U);

    reset_mocks();
    s_time_wait_result = ESP_ERR_INVALID_STATE;
    io = invoke(handler, s_valid_request, strlen(s_valid_request), 0U);
    expect_status(&io, 500, "{\"error\":\"System time unavailable\"}");
    assert(s_time_wait_calls == 1U && s_registration_calls == 0U);
}

static void test_status_mapping_and_cas(httpd_uri_t *handler)
{
    reset_mocks();
    s_oidc_result = JETKVM_OIDC_INVALID_SIGNATURE;
    request_io_t io = invoke(handler, s_valid_request,
                             strlen(s_valid_request), 0U);
    expect_status(&io, 400, "{\"error\":\"Invalid OIDC token\"}");
    assert(s_cas_calls == 0U);

    reset_mocks();
    s_oidc_result = JETKVM_OIDC_JWKS_UNAVAILABLE;
    io = invoke(handler, s_valid_request, strlen(s_valid_request), 0U);
    expect_status(&io, 500, "{\"error\":\"Cloud registration failed\"}");
    assert(s_cas_calls == 0U);

    reset_mocks();
    s_forced_upstream_status = 429;
    io = invoke(handler, s_valid_request, strlen(s_valid_request), 0U);
    expect_status(&io, 429, "{\"error\":\"Failed to exchange token\"}");
    assert(s_oidc_calls == 0U && s_cas_calls == 0U);

    reset_mocks();
    s_cas_result = ESP_ERR_INVALID_STATE;
    io = invoke(handler, s_valid_request, strlen(s_valid_request), 0U);
    expect_status(&io, 500,
                  "{\"error\":\"Failed to save configuration\"}");
    assert(s_cas_calls == 1U);
    assert(s_cas_generation == 77U);
    assert(strcmp(s_cas_url, "https://trusted.example") == 0);

    reset_mocks();
    s_snapshot_result = ESP_FAIL;
    io = invoke(handler, s_valid_request, strlen(s_valid_request), 0U);
    expect_status(&io, 500,
                  "{\"error\":\"Cloud registration unavailable\"}");
    assert(s_time_start_calls == 0U && s_registration_calls == 0U);
}

int main(void)
{
    httpd_handle_t server = (httpd_handle_t)(uintptr_t)1U;

    /* Without injection the route stays present but cannot expose secrets. */
    assert(p4_device_api_register_handlers(server) == ESP_OK);
    httpd_uri_t *handler = find_handler("/cloud/register", HTTP_POST);
    assert(handler != NULL && handler->user_ctx == NULL);
    reset_mocks();
    request_io_t unavailable = invoke(handler, s_valid_request,
                                      strlen(s_valid_request), 0U);
    expect_status(&unavailable, 500,
                  "{\"error\":\"Cloud registration unavailable\"}");
    assert(unavailable.receive_calls == 0U);
    assert(p4_device_api_unregister_handlers(server) == ESP_OK);
    assert(s_unregister_calls == 5U);

    assert(p4_device_api_set_oidc_validator(NULL) == ESP_ERR_INVALID_ARG);
    assert(p4_device_api_set_oidc_validator(
        (jetkvm_oidc_validator_t *)&s_validator_storage) == ESP_OK);

    s_handler_count = 0U;
    s_unregister_calls = 0U;
    s_register_fail_at = 5;
    assert(p4_device_api_register_handlers(server) == ESP_FAIL);
    assert(s_handler_count == 4U);
    assert(s_unregister_calls == 4U);

    s_handler_count = 0U;
    s_unregister_calls = 0U;
    s_register_fail_at = 0;
    assert(p4_device_api_register_handlers(server) == ESP_OK);
    assert(s_handler_count == 5U);
    handler = find_handler("/cloud/register", HTTP_POST);
    assert(handler != NULL);
    assert(handler->user_ctx ==
           (jetkvm_oidc_validator_t *)&s_validator_storage);

    test_registration_success(handler);
    test_memory_admission_failure(handler);
    test_response_allocation_failure(handler);
    test_invalid_requests(handler);
    test_time_failures(handler);
    test_status_mapping_and_cas(handler);

    assert(p4_device_api_unregister_handlers(server) == ESP_OK);
    assert(s_unregister_calls == 5U);
    puts("P4 /cloud/register API host tests: OK");
    return 0;
}

/* The cloud handler suite models an already authorized HTTP request. */
bool jetkvm_access_is_setup(void) { return true; }
bool jetkvm_factory_reset_pending(void) { return false; }
const char *jetkvm_factory_reset_error(void) { return NULL; }
bool jetkvm_access_password_enabled(void) { return false; }
esp_err_t jetkvm_access_register(httpd_handle_t server, const httpd_uri_t *uri)
{ return httpd_register_uri_handler(server, uri); }

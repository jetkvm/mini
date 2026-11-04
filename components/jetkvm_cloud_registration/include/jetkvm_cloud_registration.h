/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "jetkvm_cloud_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JETKVM_CLOUD_REGISTRATION_HTTP_TIMEOUT_MS 10000U
#define JETKVM_CLOUD_REGISTRATION_TEMP_TOKEN_MAX_LENGTH 1024U
#define JETKVM_CLOUD_REGISTRATION_OIDC_TOKEN_MAX_LENGTH 8192U
#define JETKVM_CLOUD_REGISTRATION_CLIENT_ID_MAX_LENGTH 512U
#define JETKVM_CLOUD_REGISTRATION_RESPONSE_MAX_LENGTH 4096U

typedef enum {
    JETKVM_CLOUD_REGISTRATION_OK = 0,
    JETKVM_CLOUD_REGISTRATION_INVALID_ARGUMENT,
    JETKVM_CLOUD_REGISTRATION_CONFIG_LOAD_FAILED,
    JETKVM_CLOUD_REGISTRATION_INVALID_CONFIG,
    JETKVM_CLOUD_REGISTRATION_OUT_OF_MEMORY,
    JETKVM_CLOUD_REGISTRATION_EXCHANGE_FAILED,
    JETKVM_CLOUD_REGISTRATION_RESPONSE_TOO_LARGE,
    JETKVM_CLOUD_REGISTRATION_UPSTREAM_HTTP_ERROR,
    JETKVM_CLOUD_REGISTRATION_INVALID_TOKEN_RESPONSE,
    JETKVM_CLOUD_REGISTRATION_OIDC_REJECTED,
    JETKVM_CLOUD_REGISTRATION_OIDC_FAILED,
    JETKVM_CLOUD_REGISTRATION_CONFIG_UPDATE_FAILED,
    JETKVM_CLOUD_REGISTRATION_PERSIST_FAILED,
} jetkvm_cloud_registration_status_t;

typedef enum {
    JETKVM_CLOUD_REGISTRATION_OIDC_OK = 0,
    JETKVM_CLOUD_REGISTRATION_OIDC_INVALID,
    JETKVM_CLOUD_REGISTRATION_OIDC_INTERNAL_ERROR,
} jetkvm_cloud_registration_oidc_status_t;

/** Fields accepted by JetKVM's /cloud/register contract. */
typedef struct {
    const char *token;
    /* Kept for wire compatibility. The configured CloudURL is authoritative. */
    const char *cloud_api;
    const char *oidc_google;
    const char *client_id;
} jetkvm_cloud_registration_request_t;

/**
 * Registration outcome; the POST /cloud/register route in device_api.c
 * answers with response_http_status.
 *
 * response_http_status preserves a non-200 token endpoint status verbatim.
 * Other values match upstream JetKVM: bad input/OIDC rejection are 400,
 * internal failures are 500, and success is 200.
 */
typedef struct {
    jetkvm_cloud_registration_status_t status;
    int response_http_status;
    int upstream_http_status;
    esp_err_t cause;
} jetkvm_cloud_registration_result_t;

typedef esp_err_t (*jetkvm_cloud_registration_exchange_fn)(
    void *context, const char *url, const char *json_body,
    char *response, size_t response_capacity, size_t *response_length,
    int *http_status);

typedef jetkvm_cloud_registration_oidc_status_t
(*jetkvm_cloud_registration_validate_oidc_fn)(
    void *context, const char *oidc_google, const char *client_id,
    char *identity, size_t identity_capacity);

typedef esp_err_t (*jetkvm_cloud_registration_save_fn)(
    void *context, const jetkvm_cloud_config_t *config);

/** Injectable operations used by the deterministic transaction core. */
typedef struct {
    jetkvm_cloud_registration_exchange_fn exchange;
    void *exchange_context;
    jetkvm_cloud_registration_validate_oidc_fn validate_oidc;
    void *oidc_context;
    jetkvm_cloud_registration_save_fn save;
    void *save_context;
} jetkvm_cloud_registration_operations_t;

/**
 * Run one registration transaction from an immutable current snapshot.
 *
 * On every failure registered_config receives the unchanged current snapshot.
 * The save operation is invoked exactly once, and only after token exchange,
 * OIDC validation and staging all succeed. A shared device-state owner should
 * implement save as a generation-and-CloudURL compare-and-swap.
 */
jetkvm_cloud_registration_status_t
jetkvm_cloud_registration_run_transaction(
    const jetkvm_cloud_config_t *current_config,
    const jetkvm_cloud_registration_request_t *request,
    const jetkvm_cloud_registration_operations_t *operations,
    jetkvm_cloud_config_t *registered_config,
    jetkvm_cloud_registration_result_t *result);

/**
 * Run a transaction against an explicit snapshot using the verified ESP HTTPS
 * exchange adapter. The caller supplies the OIDC validator and commit/CAS
 * callback, making this the preferred device-state integration API.
 */
jetkvm_cloud_registration_status_t
jetkvm_cloud_registration_register_snapshot(
    const jetkvm_cloud_config_t *current_config,
    const jetkvm_cloud_registration_request_t *request,
    jetkvm_cloud_registration_validate_oidc_fn validate_oidc,
    void *oidc_context,
    jetkvm_cloud_registration_save_fn save,
    void *save_context,
    jetkvm_cloud_config_t *registered_config,
    jetkvm_cloud_registration_result_t *result);

/**
 * Load the current NVS snapshot, exchange the token over verified HTTPS, run
 * the injected OIDC validator, and atomically commit the resulting config.
 * This convenience API must be externally serialized against all other cloud
 * configuration writes; device-state integrations should use run_transaction.
 */
jetkvm_cloud_registration_status_t jetkvm_cloud_registration_register(
    const jetkvm_cloud_registration_request_t *request,
    jetkvm_cloud_registration_validate_oidc_fn validate_oidc,
    void *oidc_context,
    jetkvm_cloud_config_t *registered_config,
    jetkvm_cloud_registration_result_t *result);

/** Verified HTTPS DELETE; accepts 2xx/404 and never follows redirects. */
esp_err_t jetkvm_cloud_registration_delete_remote(
    const jetkvm_cloud_config_t *config, const char *device_id);

const char *jetkvm_cloud_registration_status_name(
    jetkvm_cloud_registration_status_t status);

/** Reads `ip=` and `ts=` (Unix seconds) from a Cloudflare /cdn-cgi/trace
 *  body. False when it holds no plausible IP address. */
bool jetkvm_cloud_public_ip_parse(const char *trace, char *ip,
                                  size_t ip_capacity, int64_t *seen_at);

/** The device's public IPv4 address as the cloud's host sees it, as Go's
 *  pkg/myip: GET <scheme>://<host of cloud_url>/cdn-cgi/trace when `refresh`,
 *  then a copy of the last address found (empty before the first). Returns an
 *  error message, or NULL. Matches jetkvm_control_context_t.get_public_ip. */
const char *jetkvm_cloud_public_ip_get(bool refresh, const char *cloud_url,
                                       char *ip, size_t ip_capacity,
                                       int64_t *last_updated, void *ctx);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jetkvm_cloud_client *jetkvm_cloud_client_handle_t;

/**
 * Validate the Google OIDC token carried by a cloud offer and bind it to the
 * identity stored by the registration owner. The token is untrusted,
 * NUL-terminated for convenience, and valid only for the duration of the call;
 * token_length is authoritative. The callback must not retain the pointer.
 */
typedef enum {
    JETKVM_CLOUD_IDENTITY_INVALID = 0,
    JETKVM_CLOUD_IDENTITY_VALID,
    JETKVM_CLOUD_IDENTITY_RETRYABLE,
} jetkvm_cloud_identity_validation_t;

typedef jetkvm_cloud_identity_validation_t
(*jetkvm_cloud_identity_validator_t)(
    const char *oidc_google, size_t token_length, void *ctx);

typedef enum {
    JETKVM_CLOUD_CLIENT_STOPPED = 0,
    JETKVM_CLOUD_CLIENT_CONNECTING,
    JETKVM_CLOUD_CLIENT_CONNECTED,
    JETKVM_CLOUD_CLIENT_RECONNECTING,
} jetkvm_cloud_client_state_t;

typedef struct {
    /** HTTP(S) cloud base URL. Only the scheme is converted to WS(S). */
    const char *url;
    /** Stable, nonempty device identity sent in X-Device-ID. */
    const char *device_id;
    /** Firmware version, sent in X-App-Version with
     * JETKVM_CLOUD_CORE_APP_VERSION_SUFFIX appended. */
    const char *app_version;
    /** Hardware SKU sent in X-Device-SKU. */
    const char *sku;
    /** Permanent cloud secret sent only as an Authorization bearer token. */
    const char *token;
    /** Required verifier for every offer's OidcGoogle token. */
    jetkvm_cloud_identity_validator_t validate_identity;
    void *identity_ctx;
    /** Defaults to 30 seconds; must be greater than the 15-second ping period. */
    uint32_t pong_timeout_sec;
    /** Wait after a failed or ended connection before the next attempt;
     * defaults to 5000 ms, as upstream. Attempts also wait for a network
     * address. */
    uint32_t reconnect_timeout_ms;
} jetkvm_cloud_client_config_t;

typedef struct {
    jetkvm_cloud_client_state_t state;
    bool started;
    bool connected;
    bool session_active;
    uint32_t connection_epoch;
    uint32_t session_id;
    uint32_t generation;

    uint64_t connections;
    uint64_t disconnections;
    uint64_t reconnect_requests;
    uint64_t websocket_errors;
    uint64_t pongs_received;
    uint64_t rx_messages;
    uint64_t rx_bytes;
    uint64_t rx_dropped;
    uint64_t rx_protocol_errors;
    uint64_t rx_oversize;
    uint64_t rx_queue_budget_dropped;
    uint32_t rx_queued_bytes;
    uint32_t rx_queued_bytes_high_watermark;
    uint64_t offers_received;
    uint64_t offers_accepted;
    uint64_t offers_rejected_identity;
    uint64_t offers_deferred_identity;
    uint64_t offers_retry_exhausted_identity;
    uint64_t candidates_received;
    uint64_t candidates_accepted;
    uint64_t signaling_errors;
    uint64_t tx_messages;
    uint64_t tx_bytes;
    uint64_t tx_dropped;
    uint64_t tx_send_errors;
    uint64_t tx_queue_budget_dropped;
    uint32_t tx_queued_bytes;
    uint32_t tx_queued_bytes_high_watermark;
} jetkvm_cloud_client_stats_t;

/** Deep-copies all string configuration; it never reads NVS or registration. */
esp_err_t jetkvm_cloud_client_init(
    const jetkvm_cloud_client_config_t *config,
    jetkvm_cloud_client_handle_t *client);

/** Starts the worker, which makes every connection attempt, the first one as
 * soon as the network has an address. ESP_OK means the worker runs. */
esp_err_t jetkvm_cloud_client_start(jetkvm_cloud_client_handle_t client);
esp_err_t jetkvm_cloud_client_stop(jetkvm_cloud_client_handle_t client);
esp_err_t jetkvm_cloud_client_destroy(jetkvm_cloud_client_handle_t client);
esp_err_t jetkvm_cloud_client_get_stats(
    jetkvm_cloud_client_handle_t client,
    jetkvm_cloud_client_stats_t *stats);

#ifdef __cplusplus
}
#endif

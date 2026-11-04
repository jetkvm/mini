/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "jetkvm_control.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JETKVM_MQTT_BROKER_CAPACITY 256U
#define JETKVM_MQTT_USERNAME_CAPACITY 128U
#define JETKVM_MQTT_PASSWORD_CAPACITY 128U
#define JETKVM_MQTT_BASE_TOPIC_CAPACITY 128U
#define JETKVM_MQTT_ERROR_CAPACITY 192U
#define JETKVM_MQTT_EFFECTIVE_TOPIC_CAPACITY 192U
#define JETKVM_MQTT_PASSWORD_MASK "********"
#define JETKVM_MQTT_DEFAULT_BASE_TOPIC "jetkvm"
#define JETKVM_MQTT_DEFAULT_PORT 1883
#define JETKVM_MQTT_DEFAULT_DEBOUNCE_MS 500

/** Persistent MQTT settings matching the Go JSON-RPC contract. */
typedef struct {
    bool enabled;
    char broker[JETKVM_MQTT_BROKER_CAPACITY];
    int32_t port;
    char username[JETKVM_MQTT_USERNAME_CAPACITY];
    char password[JETKVM_MQTT_PASSWORD_CAPACITY];
    char base_topic[JETKVM_MQTT_BASE_TOPIC_CAPACITY];
    bool use_tls;
    bool tls_insecure;
    bool enable_ha_discovery;
    bool enable_actions;
    int32_t debounce_ms;
} jetkvm_mqtt_config_t;

typedef struct {
    bool initialized;
    bool service_started;
    bool connected;
    bool connecting;
    bool recovered_defaults;
    esp_err_t load_error;
    esp_err_t persistence_error;
    uint32_t generation;
    uint64_t connections;
    uint64_t disconnections;
    uint64_t reconnect_attempts;
    uint64_t publish_failures;
    uint64_t commands_received;
    uint64_t commands_rejected;
    char error[JETKVM_MQTT_ERROR_CAPACITY];
} jetkvm_mqtt_status_t;

typedef struct {
    bool success;
    char error[JETKVM_MQTT_ERROR_CAPACITY];
} jetkvm_mqtt_test_result_t;

/** Populate the upstream-compatible disabled/default configuration. */
void jetkvm_mqtt_config_defaults(jetkvm_mqtt_config_t *config);

/** Validate a canonical configuration and optionally explain the first error. */
bool jetkvm_mqtt_config_validate(
    const jetkvm_mqtt_config_t *config,
    char *error, size_t error_capacity);

/**
 * Canonicalize a candidate in place.
 *
 * Empty base_topic becomes "jetkvm", negative debounce becomes zero, and a
 * masked password preserves the existing secret. Validation runs afterwards.
 */
bool jetkvm_mqtt_config_normalize(
    jetkvm_mqtt_config_t *candidate,
    const jetkvm_mqtt_config_t *existing,
    char *error, size_t error_capacity);

/** Build Go's effective base topic, appending device_id when not contained. */
bool jetkvm_mqtt_build_base_topic(
    const jetkvm_mqtt_config_t *config, const char *device_id,
    char *output, size_t output_capacity);

/** Load the CRC-protected NVS record and create the reconnect/status worker. */
esp_err_t jetkvm_mqtt_init(void);
bool jetkvm_mqtt_is_initialized(void);

/** Allow the persisted client to connect after Ethernet/services are ready. */
esp_err_t jetkvm_mqtt_start(void);

/** Copy settings, replacing a nonempty password with the eight-star mask. */
esp_err_t jetkvm_mqtt_get_config(jetkvm_mqtt_config_t *config,
                                 bool mask_password);

/** Persist first, then atomically replace/restart the active client. */
esp_err_t jetkvm_mqtt_set_config(
    const jetkvm_mqtt_config_t *config,
    char *error, size_t error_capacity);

/** Copy connection and bounded diagnostic state; never returns credentials. */
esp_err_t jetkvm_mqtt_get_status(jetkvm_mqtt_status_t *status);

/** Run one non-reconnecting five-second connection probe. */
esp_err_t jetkvm_mqtt_test_connection(
    const jetkvm_mqtt_config_t *config,
    jetkvm_mqtt_test_result_t *result);

/** Publish all currently applicable P4 states immediately when connected. */
esp_err_t jetkvm_mqtt_publish_now(void);

/** Fill the JSON-RPC control-protocol context with this service's handlers. */
void jetkvm_mqtt_fill_control_context(jetkvm_mqtt_context_t *context);

#ifdef __cplusplus
}
#endif

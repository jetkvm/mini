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
#include "jetkvm_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    JETKVM_DEVICE_CLOUD_CHANGE_API_URL = 1U << 0,
    JETKVM_DEVICE_CLOUD_CHANGE_APP_URL = 1U << 1,
    JETKVM_DEVICE_CLOUD_CHANGE_REGISTRATION = 1U << 2,
};

/** Callback runs after the state lock is released and must not block. */
typedef void (*jetkvm_device_cloud_change_cb_t)(
    uint32_t changes, uint32_t generation, void *ctx);

typedef struct {
    bool registered;
    uint32_t generation;
    char cloud_url[JETKVM_CLOUD_URL_CAPACITY];
    char cloud_app_url[JETKVM_CLOUD_APP_URL_CAPACITY];
} jetkvm_device_cloud_public_state_t;

/** Load the stable identity and cloud NVS snapshot before starting services. */
esp_err_t jetkvm_device_state_init(void);

esp_err_t jetkvm_device_state_get_id(char *output, size_t output_capacity);
esp_err_t jetkvm_device_state_cloud_snapshot(
    jetkvm_cloud_config_t *config, uint32_t *generation);
esp_err_t jetkvm_device_state_cloud_public_snapshot(
    jetkvm_device_cloud_public_state_t *state);

/** Persist first, then publish both URL values as one runtime generation. */
esp_err_t jetkvm_device_state_cloud_set_urls(
    const char *cloud_url, const char *cloud_app_url,
    uint32_t *changes);

/** Persist and publish a verified permanent token + aud:sub pair. */
esp_err_t jetkvm_device_state_cloud_set_registration(
    const char *cloud_token, const char *google_identity);

/**
 * Commit registration only if the CloudURL snapshot used for token exchange
 * is still current. This prevents a concurrent setCloudUrl from binding a
 * token issued by one cloud to another cloud configuration.
 */
esp_err_t jetkvm_device_state_cloud_set_registration_if_current(
    uint32_t expected_generation, const char *expected_cloud_url,
    const char *cloud_token, const char *google_identity);

/** Persist and publish an unregistered state without changing cloud URLs. */
esp_err_t jetkvm_device_state_cloud_clear_registration(void);
/** Do not remove a newer registration after an in-flight cloud DELETE. */
esp_err_t jetkvm_device_state_cloud_clear_registration_if_current(
    uint32_t expected_generation, const char *expected_cloud_url);

/** Reload a transaction committed by another component, then notify changes. */
esp_err_t jetkvm_device_state_cloud_reload(void);

/** One cloud service may subscribe; secrets are never included in callbacks. */
esp_err_t jetkvm_device_state_cloud_subscribe(
    jetkvm_device_cloud_change_cb_t callback, void *ctx);
void jetkvm_device_state_cloud_unsubscribe(
    jetkvm_device_cloud_change_cb_t callback, void *ctx);

/** Last NVS load/save error, retained for diagnostics without exposing data. */
esp_err_t jetkvm_device_state_persistence_error(void);

/** Persist deregistration but keep the current session identity until its reply.
 * Cloud writes return INVALID_STATE while publication is pending. */
esp_err_t jetkvm_device_state_cloud_prepare_deregistration(
    uint32_t expected_generation, const char *expected_cloud_url);
/** Publish the saved state after the reply attempt, including failure/disconnect. */
void jetkvm_device_state_cloud_finish_deregistration(void);

#ifdef __cplusplus
}
#endif

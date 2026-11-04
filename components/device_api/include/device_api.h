/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jetkvm_oidc_validator jetkvm_oidc_validator_t;

#define P4_DEVICE_API_CLOUD_REGISTER_BODY_MAX_LENGTH 10368U
#define P4_DEVICE_API_CLOUD_REGISTER_TIME_WAIT_MS 10000U

/**
 * Supply the OIDC validator shared by local adoption and cloud sessions.
 *
 * Call this before p4_device_api_register_handlers(). The HTTP server stores
 * the pointer in the /cloud/register handler context, so the validator must
 * remain alive until those handlers have been unregistered.
 */
esp_err_t p4_device_api_set_oidc_validator(
    jetkvm_oidc_validator_t *validator);

/** Register the local JetKVM bootstrap and cloud-adoption API. */
esp_err_t p4_device_api_register_handlers(httpd_handle_t server);

/** Unregister all handlers installed by p4_device_api_register_handlers(). */
esp_err_t p4_device_api_unregister_handlers(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

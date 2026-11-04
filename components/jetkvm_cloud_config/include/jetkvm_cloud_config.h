/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JETKVM_CLOUD_CONFIG_SCHEMA_VERSION 1U
#define JETKVM_CLOUD_URL_CAPACITY 513U
#define JETKVM_CLOUD_APP_URL_CAPACITY 513U
#define JETKVM_CLOUD_TOKEN_CAPACITY 1025U
#define JETKVM_CLOUD_IDENTITY_CAPACITY 384U

#define JETKVM_DEFAULT_CLOUD_URL "https://api.jetkvm.com"
#define JETKVM_DEFAULT_CLOUD_APP_URL "https://app.jetkvm.com"

typedef struct {
    uint32_t schema_version;
    char cloud_url[JETKVM_CLOUD_URL_CAPACITY];
    char cloud_app_url[JETKVM_CLOUD_APP_URL_CAPACITY];
    char cloud_token[JETKVM_CLOUD_TOKEN_CAPACITY];
    char google_identity[JETKVM_CLOUD_IDENTITY_CAPACITY];
} jetkvm_cloud_config_t;

/** Populate upstream-compatible JetKVM cloud defaults without registration. */
void jetkvm_cloud_config_defaults(jetkvm_cloud_config_t *config);

/** Validate the complete in-memory representation, including token pairing. */
bool jetkvm_cloud_config_validate(const jetkvm_cloud_config_t *config);

/** True only when URL, permanent token, and verified OIDC identity are present. */
bool jetkvm_cloud_config_is_registered(const jetkvm_cloud_config_t *config);

/** Replace API and app origins after strict header/URL-safe validation. */
esp_err_t jetkvm_cloud_config_set_urls(
    jetkvm_cloud_config_t *config,
    const char *cloud_url, const char *cloud_app_url);

/** Atomically update the permanent bearer token and its verified aud:sub owner. */
esp_err_t jetkvm_cloud_config_set_registration(
    jetkvm_cloud_config_t *config,
    const char *cloud_token, const char *google_identity);

/** Remove local registration secrets while retaining configured cloud URLs. */
void jetkvm_cloud_config_clear_registration(jetkvm_cloud_config_t *config);

/** Load from the dedicated NVS namespace; missing state yields safe defaults. */
esp_err_t jetkvm_cloud_config_load(jetkvm_cloud_config_t *config);

/** Commit a validated snapshot to NVS. Secrets are never logged by this API. */
esp_err_t jetkvm_cloud_config_save(const jetkvm_cloud_config_t *config);

/** Clear registration credentials and commit the resulting snapshot. */
esp_err_t jetkvm_cloud_config_clear_registration_persisted(void);

#ifdef __cplusplus
}
#endif

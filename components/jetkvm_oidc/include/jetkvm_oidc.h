/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "jetkvm_oidc_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jetkvm_oidc_validator jetkvm_oidc_validator_t;

typedef struct {
    uint32_t http_timeout_ms;
    uint32_t lock_timeout_ms;
    jetkvm_oidc_core_config_t core;
} jetkvm_oidc_config_t;

void jetkvm_oidc_default_config(jetkvm_oidc_config_t *config);

jetkvm_oidc_status_t jetkvm_oidc_validator_create(
    const jetkvm_oidc_config_t *config,
    jetkvm_oidc_validator_t **validator);

void jetkvm_oidc_validator_destroy(jetkvm_oidc_validator_t *validator);

jetkvm_oidc_status_t jetkvm_oidc_validate_registration(
    jetkvm_oidc_validator_t *validator,
    const char *id_token, const char *client_id,
    char *identity, size_t identity_capacity);

jetkvm_oidc_status_t jetkvm_oidc_validate_session(
    jetkvm_oidc_validator_t *validator,
    const char *id_token,
    char *identity, size_t identity_capacity);

void jetkvm_oidc_validator_invalidate_keys(
    jetkvm_oidc_validator_t *validator);

#ifdef __cplusplus
}
#endif


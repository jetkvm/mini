/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "jetkvm_oidc_core.h"

jetkvm_oidc_status_t jetkvm_oidc_mbedtls_verify_rs256(
    void *context, const jetkvm_oidc_jwk_t *key,
    const uint8_t *message, size_t message_length,
    const uint8_t *signature, size_t signature_length);


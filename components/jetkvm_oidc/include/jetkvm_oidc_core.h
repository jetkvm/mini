/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JETKVM_OIDC_GOOGLE_ISSUER "https://accounts.google.com"
#define JETKVM_OIDC_GOOGLE_ISSUER_LEGACY "accounts.google.com"
#define JETKVM_OIDC_GOOGLE_JWKS_URL \
    "https://www.googleapis.com/oauth2/v3/certs"

#define JETKVM_OIDC_MAX_TOKEN_BYTES 8192U
#define JETKVM_OIDC_MAX_JWKS_BYTES 32768U
#define JETKVM_OIDC_MAX_KEYS 8U
#define JETKVM_OIDC_MAX_KID_BYTES 128U
#define JETKVM_OIDC_MAX_AUDIENCES 8U
#define JETKVM_OIDC_MAX_AUDIENCE_BYTES 255U
#define JETKVM_OIDC_MAX_SUBJECT_BYTES 255U
#define JETKVM_OIDC_IDENTITY_CAPACITY 384U
#define JETKVM_OIDC_MAX_RSA_MODULUS_BYTES 512U
#define JETKVM_OIDC_MAX_RSA_EXPONENT_BYTES 8U
#define JETKVM_OIDC_MAX_SIGNATURE_BYTES 512U
#define JETKVM_OIDC_JWKS_TTL_UNSPECIFIED UINT32_MAX

typedef enum {
    JETKVM_OIDC_OK = 0,
    JETKVM_OIDC_INVALID_ARGUMENT,
    JETKVM_OIDC_INPUT_TOO_LARGE,
    JETKVM_OIDC_MALFORMED_TOKEN,
    JETKVM_OIDC_UNSUPPORTED_ALGORITHM,
    JETKVM_OIDC_MISSING_CLAIM,
    JETKVM_OIDC_INVALID_ISSUER,
    JETKVM_OIDC_INVALID_AUDIENCE,
    JETKVM_OIDC_INVALID_TIME,
    JETKVM_OIDC_CLOCK_NOT_READY,
    JETKVM_OIDC_TOKEN_EXPIRED,
    JETKVM_OIDC_TOKEN_NOT_YET_VALID,
    JETKVM_OIDC_IDENTITY_TOO_LARGE,
    JETKVM_OIDC_INVALID_JWKS,
    JETKVM_OIDC_KEY_NOT_FOUND,
    JETKVM_OIDC_INVALID_SIGNATURE,
    JETKVM_OIDC_JWKS_UNAVAILABLE,
    JETKVM_OIDC_BUSY,
    JETKVM_OIDC_NO_MEMORY,
    JETKVM_OIDC_CRYPTO_ERROR,
} jetkvm_oidc_status_t;

typedef enum {
    JETKVM_OIDC_REGISTRATION = 0,
    JETKVM_OIDC_SESSION,
} jetkvm_oidc_validation_mode_t;

typedef struct {
    const uint8_t *signing_input;
    size_t signing_input_length;
    uint8_t signature[JETKVM_OIDC_MAX_SIGNATURE_BYTES];
    size_t signature_length;
    char kid[JETKVM_OIDC_MAX_KID_BYTES + 1U];
    char issuer[sizeof(JETKVM_OIDC_GOOGLE_ISSUER)];
    char audiences[JETKVM_OIDC_MAX_AUDIENCES]
                  [JETKVM_OIDC_MAX_AUDIENCE_BYTES + 1U];
    size_t audience_count;
    char subject[JETKVM_OIDC_MAX_SUBJECT_BYTES + 1U];
    int64_t expires_at_s;
    int64_t issued_at_s;
    int64_t not_before_s;
    bool has_not_before;
} jetkvm_oidc_token_t;

typedef struct {
    char kid[JETKVM_OIDC_MAX_KID_BYTES + 1U];
    uint8_t modulus[JETKVM_OIDC_MAX_RSA_MODULUS_BYTES];
    size_t modulus_length;
    uint8_t exponent[JETKVM_OIDC_MAX_RSA_EXPONENT_BYTES];
    size_t exponent_length;
} jetkvm_oidc_jwk_t;

typedef struct {
    const char *data;
    size_t length;
    uint32_t max_age_s;
    void *handle;
} jetkvm_oidc_jwks_response_t;

typedef jetkvm_oidc_status_t (*jetkvm_oidc_fetch_jwks_fn)(
    void *context, jetkvm_oidc_jwks_response_t *response);
typedef void (*jetkvm_oidc_release_jwks_fn)(
    void *context, jetkvm_oidc_jwks_response_t *response);
typedef jetkvm_oidc_status_t (*jetkvm_oidc_verify_rs256_fn)(
    void *context, const jetkvm_oidc_jwk_t *key,
    const uint8_t *message, size_t message_length,
    const uint8_t *signature, size_t signature_length);

typedef struct {
    uint32_t clock_skew_s;
    uint32_t default_jwks_ttl_s;
    uint32_t max_jwks_ttl_s;
    uint32_t minimum_refresh_interval_s;
} jetkvm_oidc_core_config_t;

typedef struct {
    jetkvm_oidc_core_config_t config;
    jetkvm_oidc_fetch_jwks_fn fetch_jwks;
    jetkvm_oidc_release_jwks_fn release_jwks;
    jetkvm_oidc_verify_rs256_fn verify_rs256;
    void *io_context;
    void *crypto_context;
    jetkvm_oidc_jwk_t keys[JETKVM_OIDC_MAX_KEYS];
    size_t key_count;
    uint64_t cache_expires_at_ms;
    uint64_t last_refresh_attempt_ms;
    bool cache_installed;
    bool refresh_attempted;
} jetkvm_oidc_core_validator_t;

void jetkvm_oidc_core_default_config(jetkvm_oidc_core_config_t *config);

jetkvm_oidc_status_t jetkvm_oidc_parse_token(
    const char *compact_token, size_t token_length,
    jetkvm_oidc_token_t *parsed);

jetkvm_oidc_status_t jetkvm_oidc_parse_jwks(
    const char *jwks, size_t jwks_length,
    jetkvm_oidc_jwk_t *keys, size_t keys_capacity,
    size_t *key_count);

const jetkvm_oidc_jwk_t *jetkvm_oidc_find_key(
    const jetkvm_oidc_jwk_t *keys, size_t key_count,
    const char *kid);

jetkvm_oidc_status_t jetkvm_oidc_validate_claims(
    const jetkvm_oidc_token_t *token,
    jetkvm_oidc_validation_mode_t mode,
    const char *registration_client_id,
    int64_t now_s, uint32_t clock_skew_s,
    char *identity, size_t identity_capacity);

bool jetkvm_oidc_parse_cache_max_age(
    const char *cache_control, size_t cache_control_length,
    uint32_t *max_age_s);

jetkvm_oidc_status_t jetkvm_oidc_core_validator_init(
    jetkvm_oidc_core_validator_t *validator,
    const jetkvm_oidc_core_config_t *config,
    jetkvm_oidc_fetch_jwks_fn fetch_jwks,
    jetkvm_oidc_release_jwks_fn release_jwks,
    void *io_context,
    jetkvm_oidc_verify_rs256_fn verify_rs256,
    void *crypto_context);

void jetkvm_oidc_core_validator_invalidate(
    jetkvm_oidc_core_validator_t *validator);

jetkvm_oidc_status_t jetkvm_oidc_core_validate_at(
    jetkvm_oidc_core_validator_t *validator,
    jetkvm_oidc_validation_mode_t mode,
    const char *compact_token, size_t token_length,
    const char *registration_client_id,
    int64_t wall_clock_now_s, uint64_t monotonic_now_ms,
    char *identity, size_t identity_capacity);

const char *jetkvm_oidc_status_name(jetkvm_oidc_status_t status);

#ifdef __cplusplus
}
#endif

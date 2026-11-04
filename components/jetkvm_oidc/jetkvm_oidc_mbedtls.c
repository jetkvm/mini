/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_oidc_private.h"

#include <string.h>

#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "psa/crypto.h"

#define RSA_PUBLIC_DER_CAPACITY \
    (JETKVM_OIDC_MAX_RSA_MODULUS_BYTES + \
     JETKVM_OIDC_MAX_RSA_EXPONENT_BYTES + 24U)

static size_t der_length_size(size_t length)
{
    if (length < 128U) {
        return 1U;
    }
    if (length <= 0xffU) {
        return 2U;
    }
    return 3U;
}

static bool write_der_length(uint8_t *destination, size_t capacity,
                             size_t *position, size_t length)
{
    if (!destination || !position || *position >= capacity) {
        return false;
    }
    if (length < 128U) {
        destination[(*position)++] = (uint8_t)length;
        return true;
    }
    if (length <= 0xffU) {
        if (capacity - *position < 2U) {
            return false;
        }
        destination[(*position)++] = 0x81U;
        destination[(*position)++] = (uint8_t)length;
        return true;
    }
    if (length <= 0xffffU && capacity - *position >= 3U) {
        destination[(*position)++] = 0x82U;
        destination[(*position)++] = (uint8_t)(length >> 8U);
        destination[(*position)++] = (uint8_t)length;
        return true;
    }
    return false;
}

static size_t der_integer_total_size(const uint8_t *integer, size_t length)
{
    size_t content = length + ((integer[0] & 0x80U) != 0U ? 1U : 0U);
    return 1U + der_length_size(content) + content;
}

static bool write_der_integer(uint8_t *destination, size_t capacity,
                              size_t *position, const uint8_t *integer,
                              size_t length)
{
    if (!destination || !position || !integer || length == 0U ||
        *position >= capacity) {
        return false;
    }
    size_t content = length + ((integer[0] & 0x80U) != 0U ? 1U : 0U);
    destination[(*position)++] = 0x02U;
    if (!write_der_length(destination, capacity, position, content) ||
        capacity - *position < content) {
        return false;
    }
    if ((integer[0] & 0x80U) != 0U) {
        destination[(*position)++] = 0U;
    }
    memcpy(destination + *position, integer, length);
    *position += length;
    return true;
}

static bool encode_rsa_public_key(const jetkvm_oidc_jwk_t *key,
                                  uint8_t *der, size_t capacity,
                                  size_t *der_length)
{
    if (!key || !der || !der_length || key->modulus_length < 256U ||
        key->modulus_length > sizeof(key->modulus) ||
        key->exponent_length == 0U ||
        key->exponent_length > sizeof(key->exponent)) {
        return false;
    }
    size_t content_length =
        der_integer_total_size(key->modulus, key->modulus_length) +
        der_integer_total_size(key->exponent, key->exponent_length);
    size_t required = 1U + der_length_size(content_length) + content_length;
    if (required > capacity) {
        return false;
    }
    size_t position = 0U;
    der[position++] = 0x30U;
    if (!write_der_length(der, capacity, &position, content_length) ||
        !write_der_integer(der, capacity, &position, key->modulus,
                           key->modulus_length) ||
        !write_der_integer(der, capacity, &position, key->exponent,
                           key->exponent_length) ||
        position != required) {
        mbedtls_platform_zeroize(der, capacity);
        return false;
    }
    *der_length = position;
    return true;
}

jetkvm_oidc_status_t jetkvm_oidc_mbedtls_verify_rs256(
    void *context, const jetkvm_oidc_jwk_t *key,
    const uint8_t *message, size_t message_length,
    const uint8_t *signature, size_t signature_length)
{
    (void)context;
    if (!key || !message || message_length == 0U || !signature ||
        signature_length == 0U ||
        signature_length != key->modulus_length) {
        return JETKVM_OIDC_INVALID_ARGUMENT;
    }

    uint8_t der[RSA_PUBLIC_DER_CAPACITY] = { 0 };
    size_t der_length = 0U;
    if (!encode_rsa_public_key(key, der, sizeof(der), &der_length)) {
        return JETKVM_OIDC_CRYPTO_ERROR;
    }

    mbedtls_pk_context public_key;
    mbedtls_pk_init(&public_key);
    int result = mbedtls_pk_parse_public_key(&public_key, der, der_length);
    mbedtls_platform_zeroize(der, sizeof(der));
    if (result != 0) {
        mbedtls_pk_free(&public_key);
        return JETKVM_OIDC_CRYPTO_ERROR;
    }

    uint8_t digest[32] = { 0 };
    size_t digest_length = 0U;
    psa_status_t psa_result = psa_crypto_init();
    if (psa_result == PSA_SUCCESS) {
        psa_result = psa_hash_compute(PSA_ALG_SHA_256,
                                      message, message_length,
                                      digest, sizeof(digest),
                                      &digest_length);
    }
    if (psa_result != PSA_SUCCESS || digest_length != sizeof(digest)) {
        mbedtls_platform_zeroize(digest, sizeof(digest));
        mbedtls_pk_free(&public_key);
        return JETKVM_OIDC_CRYPTO_ERROR;
    }

    result = mbedtls_pk_verify(&public_key, MBEDTLS_MD_SHA256,
                               digest, sizeof(digest),
                               signature, signature_length);
    mbedtls_platform_zeroize(digest, sizeof(digest));
    mbedtls_pk_free(&public_key);
    return result == 0 ? JETKVM_OIDC_OK : JETKVM_OIDC_INVALID_SIGNATURE;
}


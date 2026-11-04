/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixtures.h"
#include "jetkvm_oidc_core.h"

#define TEST_NOW 1700000000LL
#define TOKEN_CAPACITY (JETKVM_OIDC_MAX_TOKEN_BYTES + 1U)

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                 \
                    __FILE__, __LINE__, #condition);                         \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

typedef struct {
    const char *jwks;
    uint32_t max_age_s;
    jetkvm_oidc_status_t fetch_status;
    unsigned fetch_count;
    unsigned release_count;
} fake_jwks_t;

static char *base64url_encode(const uint8_t *source, size_t length)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t output_length = (length / 3U) * 4U;
    if (length % 3U == 1U) {
        output_length += 2U;
    } else if (length % 3U == 2U) {
        output_length += 3U;
    }
    char *output = calloc(output_length + 1U, 1U);
    CHECK(output != NULL);
    size_t input = 0U;
    size_t cursor = 0U;
    while (input + 3U <= length) {
        uint32_t value = ((uint32_t)source[input] << 16U) |
                         ((uint32_t)source[input + 1U] << 8U) |
                         source[input + 2U];
        output[cursor++] = alphabet[(value >> 18U) & 63U];
        output[cursor++] = alphabet[(value >> 12U) & 63U];
        output[cursor++] = alphabet[(value >> 6U) & 63U];
        output[cursor++] = alphabet[value & 63U];
        input += 3U;
    }
    if (length - input == 1U) {
        uint32_t value = (uint32_t)source[input] << 16U;
        output[cursor++] = alphabet[(value >> 18U) & 63U];
        output[cursor++] = alphabet[(value >> 12U) & 63U];
    } else if (length - input == 2U) {
        uint32_t value = ((uint32_t)source[input] << 16U) |
                         ((uint32_t)source[input + 1U] << 8U);
        output[cursor++] = alphabet[(value >> 18U) & 63U];
        output[cursor++] = alphabet[(value >> 12U) & 63U];
        output[cursor++] = alphabet[(value >> 6U) & 63U];
    }
    CHECK(cursor == output_length);
    return output;
}

static EVP_PKEY *fixture_private_key(void)
{
    BIO *bio = BIO_new_mem_buf(OIDC_FIXTURE_PRIVATE_KEY, -1);
    CHECK(bio != NULL);
    EVP_PKEY *key = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    BIO_free(bio);
    CHECK(key != NULL);
    return key;
}

static char *make_token(const char *header_json, const char *claims_json)
{
    char *header = base64url_encode((const uint8_t *)header_json,
                                    strlen(header_json));
    char *claims = base64url_encode((const uint8_t *)claims_json,
                                    strlen(claims_json));
    size_t signing_length = strlen(header) + 1U + strlen(claims);
    char *signing_input = calloc(signing_length + 1U, 1U);
    CHECK(signing_input != NULL);
    CHECK((size_t)snprintf(signing_input, signing_length + 1U,
                           "%s.%s", header, claims) == signing_length);

    EVP_PKEY *private_key = fixture_private_key();
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    CHECK(digest != NULL);
    CHECK(EVP_DigestSignInit(digest, NULL, EVP_sha256(), NULL,
                             private_key) == 1);
    size_t signature_length = 0U;
    CHECK(EVP_DigestSign(digest, NULL, &signature_length,
                         (const uint8_t *)signing_input,
                         signing_length) == 1);
    uint8_t *signature = malloc(signature_length);
    CHECK(signature != NULL);
    CHECK(EVP_DigestSign(digest, signature, &signature_length,
                         (const uint8_t *)signing_input,
                         signing_length) == 1);
    char *encoded_signature = base64url_encode(signature, signature_length);

    size_t token_length = signing_length + 1U + strlen(encoded_signature);
    CHECK(token_length <= JETKVM_OIDC_MAX_TOKEN_BYTES);
    char *token = calloc(token_length + 1U, 1U);
    CHECK(token != NULL);
    CHECK((size_t)snprintf(token, token_length + 1U, "%s.%s",
                           signing_input, encoded_signature) == token_length);

    EVP_MD_CTX_free(digest);
    EVP_PKEY_free(private_key);
    free(signature);
    free(encoded_signature);
    free(signing_input);
    free(claims);
    free(header);
    return token;
}

static char *make_standard_token(const char *kid, const char *claims)
{
    char header[256];
    int written = snprintf(header, sizeof(header),
                           "{\"alg\":\"RS256\",\"typ\":\"JWT\",\"kid\":\"%s\"}",
                           kid);
    CHECK(written > 0 && (size_t)written < sizeof(header));
    return make_token(header, claims);
}

static jetkvm_oidc_status_t openssl_verify(
    void *context, const jetkvm_oidc_jwk_t *key,
    const uint8_t *message, size_t message_length,
    const uint8_t *signature, size_t signature_length)
{
    (void)context;
    BIGNUM *modulus = BN_bin2bn(key->modulus, (int)key->modulus_length, NULL);
    BIGNUM *exponent = BN_bin2bn(key->exponent,
                                 (int)key->exponent_length, NULL);
    OSSL_PARAM_BLD *builder = OSSL_PARAM_BLD_new();
    CHECK(modulus != NULL && exponent != NULL && builder != NULL);
    CHECK(OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N,
                                 modulus) == 1);
    CHECK(OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E,
                                 exponent) == 1);
    OSSL_PARAM *parameters = OSSL_PARAM_BLD_to_param(builder);
    CHECK(parameters != NULL);
    EVP_PKEY_CTX *key_context = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
    CHECK(key_context != NULL && EVP_PKEY_fromdata_init(key_context) == 1);
    EVP_PKEY *public_key = NULL;
    CHECK(EVP_PKEY_fromdata(key_context, &public_key,
                            EVP_PKEY_PUBLIC_KEY, parameters) == 1);
    EVP_MD_CTX *digest = EVP_MD_CTX_new();
    CHECK(digest != NULL);
    CHECK(EVP_DigestVerifyInit(digest, NULL, EVP_sha256(), NULL,
                               public_key) == 1);
    int verified = EVP_DigestVerify(digest, signature, signature_length,
                                    message, message_length);

    EVP_MD_CTX_free(digest);
    EVP_PKEY_free(public_key);
    EVP_PKEY_CTX_free(key_context);
    OSSL_PARAM_free(parameters);
    OSSL_PARAM_BLD_free(builder);
    BN_free(exponent);
    BN_free(modulus);
    return verified == 1 ? JETKVM_OIDC_OK
                         : JETKVM_OIDC_INVALID_SIGNATURE;
}

static jetkvm_oidc_status_t fake_fetch(
    void *context, jetkvm_oidc_jwks_response_t *response)
{
    fake_jwks_t *fake = context;
    ++fake->fetch_count;
    if (fake->fetch_status != JETKVM_OIDC_OK) {
        return fake->fetch_status;
    }
    response->data = fake->jwks;
    response->length = strlen(fake->jwks);
    response->max_age_s = fake->max_age_s;
    return JETKVM_OIDC_OK;
}

static void fake_release(void *context,
                         jetkvm_oidc_jwks_response_t *response)
{
    fake_jwks_t *fake = context;
    ++fake->release_count;
    memset(response, 0, sizeof(*response));
}

static void init_validator(jetkvm_oidc_core_validator_t *validator,
                           fake_jwks_t *fake)
{
    jetkvm_oidc_core_config_t config;
    jetkvm_oidc_core_default_config(&config);
    CHECK(jetkvm_oidc_core_validator_init(
              validator, &config, fake_fetch, fake_release, fake,
              openssl_verify, NULL) == JETKVM_OIDC_OK);
}

static const char valid_string_audience_claims[] =
    "{\"iss\":\"https://accounts.google.com\","
    "\"sub\":\"subject-123\",\"aud\":\"primary-client\","
    "\"iat\":1699999900,\"exp\":1700003600}";

static const char valid_array_audience_claims[] =
    "{\"iss\":\"https://accounts.google.com\","
    "\"sub\":\"subject-123\","
    "\"aud\":[\"primary-client\",\"registration-client\"],"
    "\"iat\":1699999900,\"exp\":1700003600}";

static void test_upstream_registration_and_session_semantics(void)
{
    fake_jwks_t fake = {
        .jwks = OIDC_FIXTURE_JWKS,
        .max_age_s = 3600U,
        .fetch_status = JETKVM_OIDC_OK,
    };
    jetkvm_oidc_core_validator_t validator;
    init_validator(&validator, &fake);
    char *token = make_standard_token(OIDC_FIXTURE_KID,
                                      valid_array_audience_claims);
    char identity[JETKVM_OIDC_IDENTITY_CAPACITY];
    CHECK(jetkvm_oidc_core_validate_at(
              &validator, JETKVM_OIDC_REGISTRATION,
              token, strlen(token), "registration-client",
              TEST_NOW, 1000U, identity, sizeof(identity)) ==
          JETKVM_OIDC_OK);
    CHECK(strcmp(identity, "primary-client:subject-123") == 0);
    CHECK(fake.fetch_count == 1U && fake.release_count == 1U);

    memset(identity, 0xa5, sizeof(identity));
    CHECK(jetkvm_oidc_core_validate_at(
              &validator, JETKVM_OIDC_SESSION,
              token, strlen(token), NULL,
              TEST_NOW, 2000U, identity, sizeof(identity)) ==
          JETKVM_OIDC_OK);
    CHECK(strcmp(identity, "primary-client:subject-123") == 0);
    CHECK(fake.fetch_count == 1U);

    memset(identity, 0xa5, sizeof(identity));
    CHECK(jetkvm_oidc_core_validate_at(
              &validator, JETKVM_OIDC_REGISTRATION,
              token, strlen(token), "wrong-client",
              TEST_NOW, 3000U, identity, sizeof(identity)) ==
          JETKVM_OIDC_INVALID_AUDIENCE);
    CHECK(identity[0] == '\0' && fake.fetch_count == 1U);
    free(token);
}

static jetkvm_oidc_status_t validate_claims_fixture(const char *claims,
                                                     int64_t now)
{
    fake_jwks_t fake = {
        .jwks = OIDC_FIXTURE_JWKS,
        .max_age_s = 3600U,
        .fetch_status = JETKVM_OIDC_OK,
    };
    jetkvm_oidc_core_validator_t validator;
    init_validator(&validator, &fake);
    char *token = make_standard_token(OIDC_FIXTURE_KID, claims);
    char identity[JETKVM_OIDC_IDENTITY_CAPACITY] = "must-clear";
    jetkvm_oidc_status_t status = jetkvm_oidc_core_validate_at(
        &validator, JETKVM_OIDC_SESSION, token, strlen(token), NULL,
        now, 0U, identity, sizeof(identity));
    if (status != JETKVM_OIDC_OK) {
        CHECK(identity[0] == '\0');
    }
    free(token);
    return status;
}

static void test_claim_validation(void)
{
    CHECK(validate_claims_fixture(
              "{\"iss\":\"https://evil.example\",\"sub\":\"s\","
              "\"aud\":\"a\",\"iat\":1699999900,\"exp\":1700003600}",
              TEST_NOW) == JETKVM_OIDC_INVALID_ISSUER);
    CHECK(validate_claims_fixture(
              "{\"iss\":\"accounts.google.com\",\"sub\":\"s\","
              "\"aud\":\"a\",\"iat\":1699999900,\"exp\":1700003600}",
              TEST_NOW) == JETKVM_OIDC_OK);
    CHECK(validate_claims_fixture(
              "{\"iss\":\"https://accounts.google.com\",\"sub\":\"s\","
              "\"aud\":\"a\",\"iat\":1699990000,\"exp\":1699999939}",
              TEST_NOW) == JETKVM_OIDC_TOKEN_EXPIRED);
    CHECK(validate_claims_fixture(
              "{\"iss\":\"https://accounts.google.com\",\"sub\":\"s\","
              "\"aud\":\"a\",\"iat\":1700000061,\"exp\":1700003600}",
              TEST_NOW) == JETKVM_OIDC_TOKEN_NOT_YET_VALID);
    CHECK(validate_claims_fixture(
              "{\"iss\":\"https://accounts.google.com\",\"sub\":\"s\","
              "\"aud\":\"a\",\"iat\":1700000100,\"exp\":1700000001}",
              TEST_NOW) == JETKVM_OIDC_INVALID_TIME);
    CHECK(validate_claims_fixture(
              "{\"iss\":\"https://accounts.google.com\",\"sub\":\"s\","
              "\"aud\":\"a\",\"exp\":1700003600}",
              TEST_NOW) == JETKVM_OIDC_MISSING_CLAIM);
    CHECK(validate_claims_fixture(valid_string_audience_claims, 1000) ==
          JETKVM_OIDC_CLOCK_NOT_READY);
    CHECK(validate_claims_fixture(
              "{\"iss\":\"https://accounts.google.com\",\"sub\":\"s\","
              "\"aud\":\"a\",\"iat\":1699999900,\"exp\":1700003600,"
              "\"nbf\":1700000301}",
              TEST_NOW) == JETKVM_OIDC_TOKEN_NOT_YET_VALID);
    CHECK(validate_claims_fixture(
              "{\"iss\":\"https://accounts.google.com\",\"sub\":\"s\","
              "\"aud\":\"a\",\"iat\":1699999900,\"exp\":1700003600,"
              "\"nbf\":1700000300}",
              TEST_NOW) == JETKVM_OIDC_OK);
}

static void test_json_prescan_rejects_hostile_inputs(void)
{
    static const char deep_claims[] =
        "{\"iss\":\"https://accounts.google.com\",\"sub\":\"s\","
        "\"aud\":\"a\",\"iat\":1699999900,\"exp\":1700003600,"
        "\"x\":{\"a\":{\"b\":{\"c\":{\"d\":{\"e\":{\"f\":{\"g\":{\"h\":1}}}}}}}}}";
    char *token = make_standard_token(OIDC_FIXTURE_KID, deep_claims);
    CHECK(jetkvm_oidc_parse_token(token, strlen(token),
                                  &(jetkvm_oidc_token_t){ 0 }) ==
          JETKVM_OIDC_MALFORMED_TOKEN);
    free(token);

    static const char nul_claims[] =
        "{\"iss\":\"https://accounts.google.com\","
        "\"sub\":\"safe\\u0000suffix\",\"aud\":\"a\","
        "\"iat\":1699999900,\"exp\":1700003600}";
    token = make_standard_token(OIDC_FIXTURE_KID, nul_claims);
    CHECK(jetkvm_oidc_parse_token(token, strlen(token),
                                  &(jetkvm_oidc_token_t){ 0 }) ==
          JETKVM_OIDC_MALFORMED_TOKEN);
    free(token);

    static const char invalid_utf8_claims[] =
        "{\"iss\":\"https://accounts.google.com\",\"sub\":\"\xc0\xaf\","
        "\"aud\":\"a\",\"iat\":1699999900,\"exp\":1700003600}";
    token = make_standard_token(OIDC_FIXTURE_KID, invalid_utf8_claims);
    CHECK(jetkvm_oidc_parse_token(token, strlen(token),
                                  &(jetkvm_oidc_token_t){ 0 }) ==
          JETKVM_OIDC_MALFORMED_TOKEN);
    free(token);

    static const char lone_surrogate_claims[] =
        "{\"iss\":\"https://accounts.google.com\","
        "\"sub\":\"bad\\ud800\",\"aud\":\"a\","
        "\"iat\":1699999900,\"exp\":1700003600}";
    token = make_standard_token(OIDC_FIXTURE_KID,
                                lone_surrogate_claims);
    CHECK(jetkvm_oidc_parse_token(token, strlen(token),
                                  &(jetkvm_oidc_token_t){ 0 }) ==
          JETKVM_OIDC_MALFORMED_TOKEN);
    free(token);
}

static void test_signature_and_header_rejection(void)
{
    fake_jwks_t fake = {
        .jwks = OIDC_FIXTURE_JWKS,
        .max_age_s = 3600U,
        .fetch_status = JETKVM_OIDC_OK,
    };
    jetkvm_oidc_core_validator_t validator;
    init_validator(&validator, &fake);
    char identity[JETKVM_OIDC_IDENTITY_CAPACITY] = { 0 };

    char *token = make_standard_token(OIDC_FIXTURE_KID,
                                      valid_string_audience_claims);
    char *signature = strrchr(token, '.');
    CHECK(signature != NULL && signature[1] != '\0');
    signature[1] = signature[1] == 'A' ? 'B' : 'A';
    CHECK(jetkvm_oidc_core_validate_at(
              &validator, JETKVM_OIDC_SESSION, token, strlen(token), NULL,
              TEST_NOW, 0U, identity, sizeof(identity)) ==
          JETKVM_OIDC_INVALID_SIGNATURE);
    CHECK(identity[0] == '\0');
    free(token);

    token = make_token(
        "{\"alg\":\"HS256\",\"typ\":\"JWT\",\"kid\":\"fixture-key-2026\"}",
        valid_string_audience_claims);
    CHECK(jetkvm_oidc_parse_token(token, strlen(token),
                                  &(jetkvm_oidc_token_t){ 0 }) ==
          JETKVM_OIDC_UNSUPPORTED_ALGORITHM);
    free(token);

    token = make_token(
        "{\"alg\":\"RS256\",\"alg\":\"RS256\",\"kid\":\"fixture-key-2026\"}",
        valid_string_audience_claims);
    CHECK(jetkvm_oidc_parse_token(token, strlen(token),
                                  &(jetkvm_oidc_token_t){ 0 }) ==
          JETKVM_OIDC_UNSUPPORTED_ALGORITHM);
    free(token);

    token = make_standard_token(
        OIDC_FIXTURE_KID,
        "{\"iss\":\"https://accounts.google.com\","
        "\"iss\":\"https://accounts.google.com\",\"sub\":\"s\","
        "\"aud\":\"a\",\"iat\":1699999900,\"exp\":1700003600}");
    CHECK(jetkvm_oidc_parse_token(token, strlen(token),
                                  &(jetkvm_oidc_token_t){ 0 }) ==
          JETKVM_OIDC_MISSING_CLAIM);
    free(token);
}

static void test_jwks_validation_and_cache_headers(void)
{
    jetkvm_oidc_jwk_t keys[JETKVM_OIDC_MAX_KEYS];
    size_t count = 0U;
    CHECK(jetkvm_oidc_parse_jwks(
              OIDC_FIXTURE_JWKS, strlen(OIDC_FIXTURE_JWKS),
              keys, JETKVM_OIDC_MAX_KEYS, &count) == JETKVM_OIDC_OK);
    CHECK(count == 1U);
    CHECK(strcmp(keys[0].kid, OIDC_FIXTURE_KID) == 0);
    CHECK(keys[0].modulus_length == 256U);
    CHECK(keys[0].exponent_length == 3U);
    CHECK(jetkvm_oidc_find_key(keys, count, OIDC_FIXTURE_KID) == &keys[0]);
    CHECK(jetkvm_oidc_find_key(keys, count, "missing") == NULL);

    /* Google serves the document indented and ending in a newline. */
    static const char formatted_jwks[] =
        "{\n  \"keys\": [\n    {\n      \"kty\": \"RSA\",\n"
        "      \"kid\": \"" OIDC_FIXTURE_KID "\",\n"
        "      \"n\": \"" OIDC_FIXTURE_MODULUS "\",\n"
        "      \"e\": \"AQAB\"\n    }\n  ]\n}\r\n";
    CHECK(jetkvm_oidc_parse_jwks(
              formatted_jwks, strlen(formatted_jwks),
              keys, JETKVM_OIDC_MAX_KEYS, &count) == JETKVM_OIDC_OK);
    CHECK(count == 1U);
    static const char trailing_garbage_jwks[] =
        "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"" OIDC_FIXTURE_KID
        "\",\"n\":\"" OIDC_FIXTURE_MODULUS "\",\"e\":\"AQAB\"}]}\nx";
    CHECK(jetkvm_oidc_parse_jwks(
              trailing_garbage_jwks, strlen(trailing_garbage_jwks),
              keys, JETKVM_OIDC_MAX_KEYS, &count) ==
          JETKVM_OIDC_INVALID_JWKS);

    static const char duplicate_jwks[] =
        "{\"keys\":["
        "{\"kty\":\"RSA\",\"kid\":\"same\",\"n\":\""
        OIDC_FIXTURE_MODULUS "\",\"e\":\"AQAB\"},"
        "{\"kty\":\"RSA\",\"kid\":\"same\",\"n\":\""
        OIDC_FIXTURE_MODULUS "\",\"e\":\"AQAB\"}]}";
    CHECK(jetkvm_oidc_parse_jwks(
              duplicate_jwks, strlen(duplicate_jwks),
              keys, JETKVM_OIDC_MAX_KEYS, &count) ==
          JETKVM_OIDC_INVALID_JWKS);
    CHECK(count == 0U);

    uint32_t max_age = 0U;
    static const char cache_control[] = "public, max-age=3600, must-revalidate";
    CHECK(jetkvm_oidc_parse_cache_max_age(
              cache_control, strlen(cache_control), &max_age));
    CHECK(max_age == 3600U);
    static const char quoted[] = "MAX-AGE=\"42\"";
    CHECK(jetkvm_oidc_parse_cache_max_age(
              quoted, strlen(quoted), &max_age));
    CHECK(max_age == 42U);
    static const char duplicate[] = "max-age=1,max-age=2";
    CHECK(!jetkvm_oidc_parse_cache_max_age(
              duplicate, strlen(duplicate), &max_age));
    static const char overflow[] = "max-age=4294967296";
    CHECK(!jetkvm_oidc_parse_cache_max_age(
              overflow, strlen(overflow), &max_age));
    static const char unknown_quoted[] =
        "private-extension=\"a,b\", max-age=7";
    CHECK(jetkvm_oidc_parse_cache_max_age(
              unknown_quoted, strlen(unknown_quoted), &max_age));
    CHECK(max_age == 7U);
}

static void test_cache_and_kid_rotation(void)
{
    fake_jwks_t fake = {
        .jwks = OIDC_FIXTURE_JWKS,
        .max_age_s = 3600U,
        .fetch_status = JETKVM_OIDC_OK,
    };
    jetkvm_oidc_core_validator_t validator;
    init_validator(&validator, &fake);
    char identity[JETKVM_OIDC_IDENTITY_CAPACITY];
    char *token = make_standard_token(OIDC_FIXTURE_KID,
                                      valid_string_audience_claims);
    CHECK(jetkvm_oidc_core_validate_at(
              &validator, JETKVM_OIDC_SESSION, token, strlen(token), NULL,
              TEST_NOW, 0U, identity, sizeof(identity)) == JETKVM_OIDC_OK);
    CHECK(jetkvm_oidc_core_validate_at(
              &validator, JETKVM_OIDC_SESSION, token, strlen(token), NULL,
              TEST_NOW, 1000U, identity, sizeof(identity)) == JETKVM_OIDC_OK);
    CHECK(fake.fetch_count == 1U);

    char *rotated = make_standard_token(OIDC_FIXTURE_NEW_KID,
                                        valid_string_audience_claims);
    CHECK(jetkvm_oidc_core_validate_at(
              &validator, JETKVM_OIDC_SESSION,
              rotated, strlen(rotated), NULL,
              TEST_NOW, 2000U, identity, sizeof(identity)) ==
          JETKVM_OIDC_KEY_NOT_FOUND);
    CHECK(fake.fetch_count == 1U);
    fake.jwks = OIDC_FIXTURE_ROTATED_JWKS;
    CHECK(jetkvm_oidc_core_validate_at(
              &validator, JETKVM_OIDC_SESSION,
              rotated, strlen(rotated), NULL,
              TEST_NOW, 30000U, identity, sizeof(identity)) ==
          JETKVM_OIDC_OK);
    CHECK(fake.fetch_count == 2U);
    free(rotated);
    free(token);
}

static void test_expired_cache_is_not_used_stale(void)
{
    fake_jwks_t fake = {
        .jwks = OIDC_FIXTURE_JWKS,
        .max_age_s = 1U,
        .fetch_status = JETKVM_OIDC_OK,
    };
    jetkvm_oidc_core_validator_t validator;
    init_validator(&validator, &fake);
    char identity[JETKVM_OIDC_IDENTITY_CAPACITY];
    char *token = make_standard_token(OIDC_FIXTURE_KID,
                                      valid_string_audience_claims);
    CHECK(jetkvm_oidc_core_validate_at(
              &validator, JETKVM_OIDC_SESSION, token, strlen(token), NULL,
              TEST_NOW, 0U, identity, sizeof(identity)) == JETKVM_OIDC_OK);
    CHECK(jetkvm_oidc_core_validate_at(
              &validator, JETKVM_OIDC_SESSION, token, strlen(token), NULL,
              TEST_NOW, 1001U, identity, sizeof(identity)) ==
          JETKVM_OIDC_JWKS_UNAVAILABLE);
    CHECK(fake.fetch_count == 1U);
    CHECK(jetkvm_oidc_core_validate_at(
              &validator, JETKVM_OIDC_SESSION, token, strlen(token), NULL,
              TEST_NOW, 30000U, identity, sizeof(identity)) ==
          JETKVM_OIDC_OK);
    CHECK(fake.fetch_count == 2U);
    free(token);
}

int main(void)
{
    test_upstream_registration_and_session_semantics();
    test_claim_validation();
    test_json_prescan_rejects_hostile_inputs();
    test_signature_and_header_rejection();
    test_jwks_validation_and_cache_headers();
    test_cache_and_kid_rotation();
    test_expired_cache_is_not_used_stale();
    puts("jetkvm_oidc host tests: PASS");
    return 0;
}

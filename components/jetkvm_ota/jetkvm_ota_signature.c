/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_ota_signature.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "jetkvm_ota_semver.h"

bool jetkvm_ota_signature_bypass(const char *version, bool custom_version,
                                 bool include_prerelease)
{
    if (custom_version) {
        return true;
    }
    jetkvm_ota_semver_t parsed;
    if (version == NULL || !jetkvm_ota_semver_parse(version, &parsed)) {
        return false;
    }
    return parsed.prerelease_len != 0U && include_prerelease;
}

const char *jetkvm_ota_signature_download(
    jetkvm_ota_http_get_fn get, void *ctx, const char *version,
    const char *signature_url, uint8_t *body, size_t capacity, size_t *length,
    char error[JETKVM_OTA_SIGNATURE_ERROR_MAX])
{
    *length = 0U;
    error[0] = '\0';
    if (signature_url == NULL || signature_url[0] == '\0') {
        (void)snprintf(error, JETKVM_OTA_SIGNATURE_ERROR_MAX,
                       "version %s requires GPG signature but API returned "
                       "no signature URL", version);
        return error;
    }
    const int status = get(ctx, signature_url, body, capacity, length);
    if (status < 0) {
        (void)snprintf(error, JETKVM_OTA_SIGNATURE_ERROR_MAX,
                       "error downloading signature");
    } else if (status != 200) {
        (void)snprintf(error, JETKVM_OTA_SIGNATURE_ERROR_MAX,
                       "signature download failed with status %d", status);
    } else if (*length == 0U) {
        (void)snprintf(error, JETKVM_OTA_SIGNATURE_ERROR_MAX,
                       "signature file is empty");
    }
    return error[0] != '\0' ? error : NULL;
}

static bool fingerprint_octets(const char *hex,
                               uint8_t out[OSVC_PGP_FINGERPRINT_LEN])
{
    if (strlen(hex) != 2U * OSVC_PGP_FINGERPRINT_LEN) {
        return false;
    }
    for (size_t i = 0U; i < 2U * OSVC_PGP_FINGERPRINT_LEN; ++i) {
        const char c = hex[i];
        const int nibble = c >= '0' && c <= '9' ? c - '0' :
                           c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (nibble < 0) {
            return false;
        }
        out[i / 2U] = (uint8_t)((i % 2U) ? (out[i / 2U] | nibble) : (nibble << 4));
    }
    return true;
}

bool jetkvm_ota_release_keys_fresh(const jetkvm_ota_release_keys_t *cache,
                                   const char *fingerprint,
                                   int64_t uptime_seconds)
{
    return cache->valid && strcmp(cache->fingerprint, fingerprint) == 0 &&
           uptime_seconds - cache->fetched_at_seconds < JETKVM_OTA_KEY_CACHE_SECONDS;
}

const char *jetkvm_ota_release_keys(
    jetkvm_ota_release_keys_t *cache, const char *fingerprint,
    const char *const *keyservers, size_t keyserver_count,
    jetkvm_ota_http_get_fn get, void *ctx, const osvc_hash_ops_t *ops,
    int64_t uptime_seconds, uint32_t now, uint8_t *scratch,
    char error[JETKVM_OTA_SIGNATURE_ERROR_MAX])
{
    error[0] = '\0';
    if (jetkvm_ota_release_keys_fresh(cache, fingerprint, uptime_seconds)) {
        return NULL;
    }
    cache->valid = false;
    uint8_t root[OSVC_PGP_FINGERPRINT_LEN];
    if (!fingerprint_octets(fingerprint, root)) {
        (void)snprintf(error, JETKVM_OTA_SIGNATURE_ERROR_MAX,
                       "root key fingerprint not configured");
        return error;
    }
    /* The last keyserver's reason: with one reason per server the text
     * would not fit the page's error line. */
    const char *reason = "no keyserver";
    char status_text[40];
    for (size_t i = 0U; i < keyserver_count; ++i) {
        char url[JETKVM_OTA_KEYSERVER_URL_MAX];
        const int written = snprintf(url, sizeof(url), keyservers[i], fingerprint);
        if (written < 0 || (size_t)written >= sizeof(url)) {
            reason = "keyserver URL is too long";
            continue;
        }
        size_t length = 0U;
        const int status = get(ctx, url, scratch, JETKVM_OTA_KEY_TEXT_MAX, &length);
        if (status < 0) {
            reason = "error fetching key";
            continue;
        }
        if (status != 200) {
            (void)snprintf(status_text, sizeof(status_text),
                           "keyserver returned status %d", status);
            reason = status_text;
            continue;
        }
        reason = osvc_pgp_read_keyring(scratch, length, root, ops, now,
                                       cache->keys, JETKVM_OTA_SIGNING_KEYS_MAX,
                                       &cache->count);
        if (reason == NULL) {
            cache->valid = true;
            cache->fetched_at_seconds = uptime_seconds;
            (void)snprintf(cache->fingerprint, sizeof(cache->fingerprint), "%s",
                           fingerprint);
            return NULL;
        }
        if (reason == osvc_pgp_error_key_revoked) {
            /* A keyserver that still has the key without its revocation
             * does not bring the key back. */
            break;
        }
    }
    (void)snprintf(error, JETKVM_OTA_SIGNATURE_ERROR_MAX,
                   "failed to fetch public key: all keyservers failed: %s",
                   reason);
    return error;
}

const char *jetkvm_ota_image_check_begin(
    jetkvm_ota_image_check_t *check, const osvc_hash_ops_t *ops,
    const uint8_t *signature, size_t signature_length,
    const osvc_pgp_key_t *keys, size_t key_count,
    char error[JETKVM_OTA_SIGNATURE_ERROR_MAX])
{
    memset(check, 0, sizeof(*check));
    check->ops = ops;
    error[0] = '\0';
    const char *reason = NULL;
    if (signature != NULL) {
        reason = osvc_pgp_find_signature(signature, signature_length, keys,
                                         key_count, &check->signature,
                                         &check->key);
        if (reason == NULL) {
            check->signed_hash =
                ops->begin(ops->ctx, check->signature.hash_algorithm);
            if (check->signed_hash == NULL) {
                reason = "the signature's hash is unavailable";
            }
        }
        if (reason != NULL) {
            (void)snprintf(error, JETKVM_OTA_SIGNATURE_ERROR_MAX,
                           "GPG signature verification failed: %s", reason);
            return error;
        }
    }
    check->sha256 = ops->begin(ops->ctx, OSVC_PGP_HASH_SHA256);
    if (check->sha256 == NULL) {
        jetkvm_ota_image_check_abort(check);
        (void)snprintf(error, JETKVM_OTA_SIGNATURE_ERROR_MAX,
                       "the image's hash is unavailable");
        return error;
    }
    return NULL;
}

bool jetkvm_ota_image_check_update(jetkvm_ota_image_check_t *check,
                                   const uint8_t *data, size_t length)
{
    const osvc_hash_ops_t *ops = check->ops;
    return ops->update(ops->ctx, check->sha256, data, length) &&
           (check->signed_hash == NULL ||
            ops->update(ops->ctx, check->signed_hash, data, length));
}

void jetkvm_ota_image_check_abort(jetkvm_ota_image_check_t *check)
{
    const osvc_hash_ops_t *ops = check->ops;
    if (check->sha256 != NULL) {
        ops->abort(ops->ctx, check->sha256);
        check->sha256 = NULL;
    }
    if (check->signed_hash != NULL) {
        ops->abort(ops->ctx, check->signed_hash);
        check->signed_hash = NULL;
    }
}

const char *jetkvm_ota_image_check_end(
    jetkvm_ota_image_check_t *check, const char *expected_sha256, uint32_t now,
    char error[JETKVM_OTA_SIGNATURE_ERROR_MAX])
{
    const osvc_hash_ops_t *ops = check->ops;
    error[0] = '\0';
    uint8_t digest[32] = {0};
    size_t digest_length = 0U;
    void *sha256 = check->sha256;
    check->sha256 = NULL;
    const bool hashed = ops->finish(ops->ctx, sha256, digest, sizeof(digest),
                                    &digest_length) &&
                        digest_length == sizeof(digest);
    char hex[2U * sizeof(digest) + 1U];
    for (size_t i = 0U; i < sizeof(digest); ++i) {
        (void)snprintf(hex + 2U * i, 3U, "%02x", digest[i]);
    }
    if (!hashed || strcasecmp(hex, expected_sha256) != 0) {
        jetkvm_ota_image_check_abort(check);
        /* The first 16 digits tell the two apart on the page's line. */
        (void)snprintf(error, JETKVM_OTA_SIGNATURE_ERROR_MAX,
                       "hash mismatch: %.16s != %.16s", hashed ? hex : "none",
                       expected_sha256);
        return error;
    }
    if (check->signed_hash != NULL) {
        void *signed_hash = check->signed_hash;
        check->signed_hash = NULL;
        const char *reason =
            osvc_pgp_verify_hash(&check->signature, OSVC_PGP_SIG_BINARY,
                                 check->key, ops, signed_hash, now);
        if (reason != NULL) {
            (void)snprintf(error, JETKVM_OTA_SIGNATURE_ERROR_MAX,
                           "GPG signature verification failed: %s", reason);
            return error;
        }
    }
    return NULL;
}

/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The GPG signature of a firmware update, as jetkvm/kvm's internal/ota has
 * it (gpg.go, utils.go): a release is signed with a subkey of the JetKVM
 * release key, the device holds that key's fingerprint and fetches the key
 * from keyservers, and an update without a valid signature is refused unless
 * the user asked for that version or the device takes prereleases.
 *
 * The image goes into flash while it downloads, so the signature and the key
 * are fetched before the download, and the image is hashed once, as it
 * streams: a detached OpenPGP signature signs the hash of the file followed
 * by a trailer from the signature itself. Pure C; HTTP and the hashes come
 * from the caller, so the host tests run this code.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os_services_pgp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The fingerprint of the JetKVM release key (rootKeyFingerprint in gpg.go). */
#define JETKVM_OTA_RELEASE_KEY_FINGERPRINT "AF5A36A993D828FEFE7C18C2D1B9856C26A79E95"
/* Where the key is fetched, in this order (keyservers in gpg.go). %s is the
 * fingerprint. */
#define JETKVM_OTA_KEYSERVER_OPENPGP "https://keys.openpgp.org/vks/v1/by-fingerprint/%s"
#define JETKVM_OTA_KEYSERVER_UBUNTU "https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x%s"
/* A fetched key is used this long (keyCacheTTL in gpg.go). */
#define JETKVM_OTA_KEY_CACHE_SECONDS (24 * 60 * 60)

/* A detached signature of one Ed25519 key has 119 octets. */
#define JETKVM_OTA_SIGNATURE_MAX 1024U
/* The release key with two subkeys is 1.3 KiB as a keyserver sends it. */
#define JETKVM_OTA_KEY_TEXT_MAX 8192U
#define JETKVM_OTA_SIGNING_KEYS_MAX 8U
#define JETKVM_OTA_KEYSERVER_URL_MAX 320U
#define JETKVM_OTA_SIGNATURE_ERROR_MAX 160U

/*
 * GET url into body. Returns the HTTP status, with the body's length in
 * *length, or a negative number when there is no answer or the body is
 * longer than capacity.
 */
typedef int (*jetkvm_ota_http_get_fn)(void *ctx, const char *url,
                                      uint8_t *body, size_t capacity,
                                      size_t *length);

/*
 * shouldBypassSignatureCheck: no signature is needed for a version that the
 * user asked for by its number (custom_version), and for a prerelease on a
 * device that takes prereleases. A version that does not parse needs one.
 */
bool jetkvm_ota_signature_bypass(const char *version, bool custom_version,
                                 bool include_prerelease);

/*
 * downloadComponentSignature: fetches the signature that the update server
 * named. Returns NULL with the signature in body, or the reason: the server
 * named none, there is no answer, the status is not 200, or the file is
 * empty. The reason is written into error.
 */
const char *jetkvm_ota_signature_download(
    jetkvm_ota_http_get_fn get, void *ctx, const char *version,
    const char *signature_url, uint8_t *body, size_t capacity, size_t *length,
    char error[JETKVM_OTA_SIGNATURE_ERROR_MAX]);

/* The signing keys of one release key, and when they were fetched. */
typedef struct {
    bool valid;
    char fingerprint[2U * OSVC_PGP_FINGERPRINT_LEN + 1U];
    int64_t fetched_at_seconds;
    size_t count;
    osvc_pgp_key_t keys[JETKVM_OTA_SIGNING_KEYS_MAX];
} jetkvm_ota_release_keys_t;

/*
 * FetchPublicKey: the signing keys of the release key with this fingerprint
 * (40 hex digits). Keys fetched less than 24 hours ago, by uptime_seconds,
 * are used again; a key among them that expires in that time is refused by
 * the check of the image. Otherwise each keyserver is asked in order until
 * one answers 200 with the key that has the fingerprint, or one answers with
 * the key and its revocation, which ends the search. now is seconds since
 * 1970 or 0 (osvc_pgp_read_keyring). scratch holds an answer and needs
 * JETKVM_OTA_KEY_TEXT_MAX octets. Returns NULL with cache filled, or the
 * reason, written into error.
 */
const char *jetkvm_ota_release_keys(
    jetkvm_ota_release_keys_t *cache, const char *fingerprint,
    const char *const *keyservers, size_t keyserver_count,
    jetkvm_ota_http_get_fn get, void *ctx, const osvc_hash_ops_t *ops,
    int64_t uptime_seconds, uint32_t now, uint8_t *scratch,
    char error[JETKVM_OTA_SIGNATURE_ERROR_MAX]);

/* True when cache holds the keys of this fingerprint, fetched less than 24
 * hours before uptime_seconds: jetkvm_ota_release_keys then fetches
 * nothing and needs no scratch. */
bool jetkvm_ota_release_keys_fresh(const jetkvm_ota_release_keys_t *cache,
                                   const char *fingerprint,
                                   int64_t uptime_seconds);

/*
 * On the device (jetkvm_ota.c): the signing keys of the JetKVM release key
 * for the OS Services releases, from the same copy that the firmware update
 * uses (jetkvm_ota_release_keys from both keyservers, at most once in 24
 * hours). Copies them into keys, with their number in *count. stopped, or
 * NULL, is asked before each request and each read: once it is true, no
 * further request is made, so a firmware install that stops the caller
 * waits for at most the one request in progress. Returns NULL, or the
 * reason, written into error: no keyserver served the key, or no signing
 * subkey of it is valid. Waits while another task fetches the key.
 */
const char *jetkvm_ota_release_signing_keys(
    bool (*stopped)(void), osvc_pgp_key_t keys[JETKVM_OTA_SIGNING_KEYS_MAX],
    size_t *count, char error[JETKVM_OTA_SIGNATURE_ERROR_MAX]);

/* The check of one image while it downloads: its SHA-256 against the update
 * server's, and its signature. */
typedef struct {
    const osvc_hash_ops_t *ops;
    void *sha256;
    /* NULL when no signature is checked. */
    void *signed_hash;
    osvc_pgp_signature_t signature;
    const osvc_pgp_key_t *key;
} jetkvm_ota_image_check_t;

/*
 * Starts the check. signature is the downloaded file, which must outlive the
 * check, or NULL when the update needs none; keys are the release key's
 * signing keys. Returns NULL, or why the check cannot start: the file holds
 * no signature that one of the keys made. The reason is written into error.
 */
const char *jetkvm_ota_image_check_begin(
    jetkvm_ota_image_check_t *check, const osvc_hash_ops_t *ops,
    const uint8_t *signature, size_t signature_length,
    const osvc_pgp_key_t *keys, size_t key_count,
    char error[JETKVM_OTA_SIGNATURE_ERROR_MAX]);

/* The next octets of the image. False when a hash fails. */
bool jetkvm_ota_image_check_update(jetkvm_ota_image_check_t *check,
                                   const uint8_t *data, size_t length);

/*
 * verifyFile: ends the check after the last octet. expected_sha256 is the
 * update server's hash, 64 hex digits. Returns NULL, or the reason: the hash
 * differs, or the signature does not verify. The reason is written into
 * error. Ends both hashes in every case.
 */
const char *jetkvm_ota_image_check_end(
    jetkvm_ota_image_check_t *check, const char *expected_sha256, uint32_t now,
    char error[JETKVM_OTA_SIGNATURE_ERROR_MAX]);

/* Ends the hashes of a check that does not reach its end. */
void jetkvm_ota_image_check_abort(jetkvm_ota_image_check_t *check);

#ifdef __cplusplus
}
#endif

/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The firmware update's signature check, case by case as jetkvm/kvm tests
 * its own (internal/ota: signature_enforcement_test.go,
 * signature_policy_test.go, gpg_test.go). The keys and signatures are
 * GnuPG's (fixtures/make_fixtures.sh); fixtures/real holds what the real
 * keyservers and the real update server serve. The hashes are OpenSSL's.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/sha.h>

#include "fixtures/root.h"
#include "host_hash.h"
#include "jetkvm_ota_signature.h"

typedef struct {
    uint8_t *data;
    size_t length;
} file_t;

static file_t load(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIXTURES_DIR, name);
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    file_t file = {malloc(1U << 16), 0U};
    assert(file.data != NULL);
    file.length = fread(file.data, 1U, 1U << 16, f);
    fclose(f);
    return file;
}

static void fingerprint(const char *hex, uint8_t out[OSVC_PGP_FINGERPRINT_LEN])
{
    for (size_t i = 0U; i < OSVC_PGP_FINGERPRINT_LEN; ++i) {
        unsigned value;
        assert(sscanf(hex + 2U * i, "%2x", &value) == 1);
        out[i] = (uint8_t)value;
    }
}

static bool has_key(const osvc_pgp_key_t *keys, size_t count, const char *hex)
{
    uint8_t wanted[OSVC_PGP_FINGERPRINT_LEN];
    fingerprint(hex, wanted);
    for (size_t i = 0U; i < count; ++i) {
        if (memcmp(keys[i].fingerprint, wanted, sizeof(wanted)) == 0) {
            return true;
        }
    }
    return false;
}

/* The signing keys of a key file, for the key with this fingerprint. */
static const char *read_keys(const char *name, const char *root_hex, uint32_t now,
                             osvc_pgp_key_t keys[JETKVM_OTA_SIGNING_KEYS_MAX],
                             size_t *count)
{
    file_t text = load(name);
    uint8_t root[OSVC_PGP_FINGERPRINT_LEN];
    fingerprint(root_hex, root);
    const char *error =
        osvc_pgp_read_keyring(text.data, text.length, root, &host_hash_ops, now,
                              keys, JETKVM_OTA_SIGNING_KEYS_MAX, count);
    free(text.data);
    assert(host_hashes_open == 0);
    return error;
}

/* --- shouldBypassSignatureCheck ------------------------------------------ */

static void test_bypass_rule(void)
{
    /* version, the user asked for it, the device takes prereleases */
    assert(!jetkvm_ota_signature_bypass("1.0.0", false, false));
    assert(!jetkvm_ota_signature_bypass("1.0.0", false, true));
    assert(jetkvm_ota_signature_bypass("1.0.0", true, false));
    assert(jetkvm_ota_signature_bypass("1.0.0", true, true));
    assert(!jetkvm_ota_signature_bypass("1.0.0-dev.1", false, false));
    assert(jetkvm_ota_signature_bypass("1.0.0-dev.1", false, true));
    assert(jetkvm_ota_signature_bypass("1.0.0-dev.1", true, false));
    assert(jetkvm_ota_signature_bypass("1.0.0-dev.1", true, true));
    assert(jetkvm_ota_signature_bypass("0.5.13-dev202610130100", false, true));
    /* Build metadata is no prerelease. */
    assert(!jetkvm_ota_signature_bypass("1.0.0+build.5", false, true));
    assert(!jetkvm_ota_signature_bypass("not-a-version", false, true));
    assert(!jetkvm_ota_signature_bypass("", false, true));
}

/* --- A stand-in for HTTP -------------------------------------------------- */

typedef struct {
    const char *url_part; /* answers a URL that contains this */
    int status;
    const char *file; /* a fixture, or NULL for an empty body */
} answer_t;

typedef struct {
    const answer_t *answers;
    size_t answer_count;
    int calls;
    char urls[4][JETKVM_OTA_KEYSERVER_URL_MAX];
} http_t;

static int http_get(void *ctx, const char *url, uint8_t *body, size_t capacity,
                    size_t *length)
{
    http_t *http = ctx;
    assert(http->calls < 4);
    snprintf(http->urls[http->calls++], JETKVM_OTA_KEYSERVER_URL_MAX, "%s", url);
    *length = 0U;
    for (size_t i = 0U; i < http->answer_count; ++i) {
        const answer_t *answer = &http->answers[i];
        if (strstr(url, answer->url_part) == NULL) {
            continue;
        }
        if (answer->file != NULL) {
            file_t file = load(answer->file);
            assert(file.length <= capacity);
            memcpy(body, file.data, file.length);
            *length = file.length;
            free(file.data);
        }
        return answer->status;
    }
    return -1;
}

/* --- downloadComponentSignature, downloadSignature ----------------------- */

static void test_signature_download(void)
{
    uint8_t body[JETKVM_OTA_SIGNATURE_MAX];
    size_t length = 99U;
    char error[JETKVM_OTA_SIGNATURE_ERROR_MAX];

    /* No signature URL: refused without a request. */
    const answer_t signed_image[] = {{"/image.sig", 200, "image.bin.sig"}};
    http_t http = {signed_image, 1U, 0, {{0}}};
    const char *reason = jetkvm_ota_signature_download(
        http_get, &http, "1.0.1", "", body, sizeof(body), &length, error);
    assert(reason != NULL);
    assert(strcmp(reason, "version 1.0.1 requires GPG signature but API "
                          "returned no signature URL") == 0);
    assert(http.calls == 0);

    /* A signature: one request, the file's octets. */
    file_t sig = load("image.bin.sig");
    assert(jetkvm_ota_signature_download(http_get, &http, "1.0.1",
                                         "http://update.test/image.sig", body,
                                         sizeof(body), &length, error) == NULL);
    assert(http.calls == 1);
    assert(strcmp(http.urls[0], "http://update.test/image.sig") == 0);
    assert(length == sig.length && memcmp(body, sig.data, length) == 0);
    free(sig.data);

    /* An empty file. */
    const answer_t empty[] = {{"/image.sig", 200, NULL}};
    http = (http_t){empty, 1U, 0, {{0}}};
    reason = jetkvm_ota_signature_download(http_get, &http, "1.0.1",
                                           "http://update.test/image.sig", body,
                                           sizeof(body), &length, error);
    assert(reason != NULL && strcmp(reason, "signature file is empty") == 0);

    /* Each status other than 200. */
    const int statuses[] = {404, 500, 403};
    for (size_t i = 0U; i < 3U; ++i) {
        const answer_t refused[] = {{"/image.sig", statuses[i], "image.bin.sig"}};
        http = (http_t){refused, 1U, 0, {{0}}};
        char expected[64];
        snprintf(expected, sizeof(expected),
                 "signature download failed with status %d", statuses[i]);
        reason = jetkvm_ota_signature_download(
            http_get, &http, "1.0.1", "http://update.test/image.sig", body,
            sizeof(body), &length, error);
        assert(reason != NULL && strcmp(reason, expected) == 0);
    }

    /* No answer. */
    http = (http_t){NULL, 0U, 0, {{0}}};
    reason = jetkvm_ota_signature_download(http_get, &http, "1.0.1",
                                           "http://update.test/image.sig", body,
                                           sizeof(body), &length, error);
    assert(reason != NULL && strcmp(reason, "error downloading signature") == 0);
}

/* --- The key as a keyserver serves it ------------------------------------ */

static void test_keyring(void)
{
    osvc_pgp_key_t keys[JETKVM_OTA_SIGNING_KEYS_MAX];
    size_t count = 0U;

    /* Both signing subkeys, and not the primary key. */
    assert(read_keys("root.asc", FIXTURE_ROOT_FINGERPRINT, 0U, keys, &count) == NULL);
    assert(count == 2U);
    assert(has_key(keys, count, FIXTURE_SUBKEY1_FINGERPRINT));
    assert(has_key(keys, count, FIXTURE_SUBKEY2_FINGERPRINT));

    /* Subkey 2 expires one year after its creation. */
    const uint32_t year = 365U * 24U * 60U * 60U;
    assert(read_keys("root.asc", FIXTURE_ROOT_FINGERPRINT,
                     FIXTURE_SUBKEY2_CREATED + year - 60U, keys, &count) == NULL);
    assert(count == 2U);
    assert(read_keys("root.asc", FIXTURE_ROOT_FINGERPRINT,
                     FIXTURE_SUBKEY2_CREATED + year + 60U, keys, &count) == NULL);
    assert(count == 1U && has_key(keys, count, FIXTURE_SUBKEY1_FINGERPRINT));

    /* A revoked subkey is no signing key. */
    assert(read_keys("root-revoked.asc", FIXTURE_ROOT_FINGERPRINT, 0U, keys,
                     &count) == NULL);
    assert(count == 1U && has_key(keys, count, FIXTURE_SUBKEY2_FINGERPRINT));

    /* A primary key that revoked itself gives none, with the reason that
     * ends the search for the key. */
    assert(read_keys("root-key-revoked.asc", FIXTURE_ROOT_FINGERPRINT, 0U, keys,
                     &count) == osvc_pgp_error_key_revoked);
    assert(count == 0U);

    /* Another key in the same answer gives no signing key. */
    assert(read_keys("root-rogue.asc", FIXTURE_ROOT_FINGERPRINT, 0U, keys,
                     &count) == NULL);
    assert(count == 2U && !has_key(keys, count, FIXTURE_OTHER_FINGERPRINT));

    /* An answer without the release key. */
    const char *error = read_keys("other.asc", FIXTURE_ROOT_FINGERPRINT, 0U, keys, &count);
    assert(error != NULL && count == 0U);
    assert(strcmp(error, "fetched key fingerprint does not match expected fingerprint") == 0);
    error = read_keys("image.bin", FIXTURE_ROOT_FINGERPRINT, 0U, keys, &count);
    assert(error != NULL && strcmp(error, "not an armored OpenPGP public key") == 0);

    /* A changed octet in the Ed25519 point of a subkey makes it a key that
     * its binding signature does not cover: the key is read, that subkey is
     * no signing key, and the other one is. */
    const struct {
        size_t at; /* a base64 character within the point, in root.asc */
        const char *other;
    } changed[] = {
        {FIXTURE_SUBKEY1_POINT_AT, FIXTURE_SUBKEY2_FINGERPRINT},
        {FIXTURE_SUBKEY2_POINT_AT, FIXTURE_SUBKEY1_FINGERPRINT},
    };
    uint8_t root[OSVC_PGP_FINGERPRINT_LEN];
    fingerprint(FIXTURE_ROOT_FINGERPRINT, root);
    for (size_t i = 0U; i < 2U; ++i) {
        file_t copy = load("root.asc");
        copy.data[changed[i].at] = copy.data[changed[i].at] == 'A' ? 'B' : 'A';
        assert(osvc_pgp_read_keyring(copy.data, copy.length, root, &host_hash_ops,
                                     0U, keys, JETKVM_OTA_SIGNING_KEYS_MAX,
                                     &count) == NULL);
        assert(count == 1U && has_key(keys, count, changed[i].other));
        free(copy.data);
        assert(host_hashes_open == 0);
    }
}

/* A hash that ends with a recorded digest, for a signature whose signed file
 * is not kept here. */
typedef struct {
    const uint8_t *digest;
    size_t length;
} recorded_hash_t;

static void *recorded_begin(void *ctx, osvc_pgp_hash_t algorithm)
{
    (void)algorithm;
    return ctx;
}

static bool recorded_update(void *ctx, void *hash, const uint8_t *data, size_t length)
{
    (void)ctx;
    (void)hash;
    (void)data;
    (void)length;
    return true;
}

static bool recorded_finish(void *ctx, void *hash, uint8_t *digest,
                            size_t capacity, size_t *length)
{
    const recorded_hash_t *recorded = ctx;
    (void)hash;
    assert(recorded->length <= capacity);
    memcpy(digest, recorded->digest, recorded->length);
    *length = recorded->length;
    return true;
}

static void recorded_abort(void *ctx, void *hash)
{
    (void)ctx;
    (void)hash;
}

/* The signing subkeys of the JetKVM release key, as GnuPG lists them. */
#define REAL_SUBKEY1_FINGERPRINT "12C22057C3EF3F25CFD409F723274246E9A54430"
#define REAL_SUBKEY2_FINGERPRINT "085A93F0FE3369817B4C88EA6782551110A9180D"

/* The JetKVM release key from both keyservers gives its two signing
 * subkeys. */
static void test_real_key(void)
{
    static const char *const FILES[] = {
        "real/keys.openpgp.org.asc", "real/keyserver.ubuntu.com.asc",
    };
    for (size_t f = 0U; f < 2U; ++f) {
        osvc_pgp_key_t keys[JETKVM_OTA_SIGNING_KEYS_MAX];
        size_t count = 0U;
        /* 2026-10-13: before the subkeys' expiry in February 2027. */
        assert(read_keys(FILES[f], JETKVM_OTA_RELEASE_KEY_FINGERPRINT,
                         1791849600U, keys, &count) == NULL);
        assert(count == 2U);
        assert(has_key(keys, count, REAL_SUBKEY1_FINGERPRINT));
        assert(has_key(keys, count, REAL_SUBKEY2_FINGERPRINT));
        /* The signature of a real release, the JetKVM app 0.5.9, names one
         * of them and verifies with it. The app is not kept here: the hash
         * that the signature signs is (fixtures/signed_digest.py), so this
         * checks the packet, the 16 bits and the Ed25519 equation with the
         * real key, and not the hashing. */
        file_t sig = load("real/jetkvm_app-0.5.9.sig");
        file_t digest = load("real/jetkvm_app-0.5.9.digest");
        osvc_pgp_signature_t signature;
        const osvc_pgp_key_t *key = NULL;
        assert(osvc_pgp_find_signature(sig.data, sig.length, keys, count,
                                       &signature, &key) == NULL);
        assert(key != NULL && signature.type == OSVC_PGP_SIG_BINARY);
        assert(signature.hash_algorithm == OSVC_PGP_HASH_SHA512 && digest.length == 64U);
        recorded_hash_t recorded = {digest.data, digest.length};
        const osvc_hash_ops_t recorded_ops = {
            &recorded, recorded_begin, recorded_update, recorded_finish, recorded_abort,
        };
        assert(osvc_pgp_verify_hash(&signature, OSVC_PGP_SIG_BINARY, key,
                                    &recorded_ops, &recorded, 1791849600U) == NULL);
        /* Another hash with the same first 16 bits fails the equation. */
        digest.data[2] ^= 0x01U;
        const char *refused = osvc_pgp_verify_hash(&signature, OSVC_PGP_SIG_BINARY, key,
                                                   &recorded_ops, &recorded, 1791849600U);
        assert(refused != NULL && strcmp(refused, "the signature does not match the data") == 0);
        free(digest.data);
        free(sig.data);
        /* After the subkeys' expiry the key gives none. */
        const char *error = read_keys(FILES[f], JETKVM_OTA_RELEASE_KEY_FINGERPRINT,
                                      1830000000U, keys, &count);
        assert(error != NULL && count == 0U);
        assert(strcmp(error, "the release key has no valid signing subkey") == 0);
    }
}

/* --- FetchPublicKey ------------------------------------------------------- */

static const char *const KEYSERVERS[] = {
    JETKVM_OTA_KEYSERVER_OPENPGP,
    JETKVM_OTA_KEYSERVER_UBUNTU,
};

static const char *fetch(jetkvm_ota_release_keys_t *cache, http_t *http,
                         int64_t uptime, char error[JETKVM_OTA_SIGNATURE_ERROR_MAX])
{
    static uint8_t scratch[JETKVM_OTA_KEY_TEXT_MAX];
    const char *reason = jetkvm_ota_release_keys(
        cache, FIXTURE_ROOT_FINGERPRINT, KEYSERVERS, 2U, http_get, http,
        &host_hash_ops, uptime, 0U, scratch, error);
    assert(host_hashes_open == 0);
    return reason;
}

static void test_key_fetch(void)
{
    char error[JETKVM_OTA_SIGNATURE_ERROR_MAX];
    jetkvm_ota_release_keys_t cache = {0};
    const int64_t day = JETKVM_OTA_KEY_CACHE_SECONDS;

    /* The first keyserver, at its URL for the fingerprint. */
    const answer_t both[] = {
        {"keys.openpgp.org", 200, "root.asc"},
        {"keyserver.ubuntu.com", 200, "root.asc"},
    };
    http_t http = {both, 2U, 0, {{0}}};
    assert(fetch(&cache, &http, 100, error) == NULL);
    assert(http.calls == 1);
    assert(strcmp(http.urls[0], "https://keys.openpgp.org/vks/v1/by-fingerprint/"
                                FIXTURE_ROOT_FINGERPRINT) == 0);
    assert(cache.count == 2U);

    /* The key is kept for 24 hours and fetched again after them. */
    assert(fetch(&cache, &http, 100 + day - 1, error) == NULL);
    assert(http.calls == 1);
    assert(fetch(&cache, &http, 100 + day, error) == NULL);
    assert(http.calls == 2);

    /* Another fingerprint does not use the kept key. */
    static uint8_t scratch[JETKVM_OTA_KEY_TEXT_MAX];
    const char *reason = jetkvm_ota_release_keys(
        &cache, FIXTURE_OTHER_FINGERPRINT, KEYSERVERS, 2U, http_get, &http,
        &host_hash_ops, 100 + day, 0U, scratch, error);
    assert(reason != NULL && http.calls == 4 && !cache.valid);

    /* The second keyserver when the first fails: with a status, with no
     * answer, and with another key. */
    const answer_t first_fails[][2] = {
        {{"keys.openpgp.org", 500, NULL}, {"keyserver.ubuntu.com", 200, "root.asc"}},
        {{"keys.openpgp.example", 200, NULL}, {"keyserver.ubuntu.com", 200, "root.asc"}},
        {{"keys.openpgp.org", 200, "other.asc"}, {"keyserver.ubuntu.com", 200, "root.asc"}},
    };
    for (size_t i = 0U; i < 3U; ++i) {
        cache = (jetkvm_ota_release_keys_t){0};
        http = (http_t){first_fails[i], 2U, 0, {{0}}};
        assert(fetch(&cache, &http, 0, error) == NULL);
        assert(http.calls == 2 && cache.count == 2U);
        assert(strstr(http.urls[0], "https://keys.openpgp.org/") == http.urls[0]);
        assert(strcmp(http.urls[1], "https://keyserver.ubuntu.com/pks/lookup?op=get"
                                    "&search=0x" FIXTURE_ROOT_FINGERPRINT) == 0);
    }

    /* The first keyserver has the key with its revocation: the second,
     * which still has the key without it, is not asked. */
    const answer_t revoked[] = {
        {"keys.openpgp.org", 200, "root-key-revoked.asc"},
        {"keyserver.ubuntu.com", 200, "root.asc"},
    };
    cache = (jetkvm_ota_release_keys_t){0};
    http = (http_t){revoked, 2U, 0, {{0}}};
    reason = fetch(&cache, &http, 0, error);
    assert(reason != NULL && http.calls == 1 && !cache.valid);
    assert(strcmp(reason, "failed to fetch public key: all keyservers failed: "
                          "the release key is revoked") == 0);

    /* No keyserver answers: no key, and nothing is kept. */
    cache = (jetkvm_ota_release_keys_t){0};
    http = (http_t){NULL, 0U, 0, {{0}}};
    reason = fetch(&cache, &http, 0, error);
    assert(reason != NULL && http.calls == 2 && !cache.valid);
    assert(strcmp(reason, "failed to fetch public key: all keyservers failed: "
                          "error fetching key") == 0);

    /* Both answer with another key. */
    const answer_t wrong[] = {
        {"keys.openpgp.org", 200, "other.asc"},
        {"keyserver.ubuntu.com", 404, NULL},
    };
    http = (http_t){wrong, 2U, 0, {{0}}};
    reason = fetch(&cache, &http, 0, error);
    assert(reason != NULL && !cache.valid);
    assert(strcmp(reason, "failed to fetch public key: all keyservers failed: "
                          "keyserver returned status 404") == 0);
}

/* --- verifyFile ------------------------------------------------------------ */

static void sha256_hex(const file_t *file, char hex[65])
{
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256(file->data, file->length, digest);
    for (size_t i = 0U; i < sizeof(digest); ++i) {
        snprintf(hex + 2U * i, 3U, "%02x", digest[i]);
    }
}

/* The whole check of an image against a signature, the image given in
 * pieces of `step` octets. Returns NULL or the reason. */
static const char *check_image_with(const file_t *image, const file_t *signature,
                                    const char *expected_sha256,
                                    const osvc_pgp_key_t *keys, size_t key_count,
                                    size_t step, uint32_t now)
{
    static char error[JETKVM_OTA_SIGNATURE_ERROR_MAX];
    jetkvm_ota_image_check_t check;
    const char *reason = jetkvm_ota_image_check_begin(
        &check, &host_hash_ops, signature->data, signature->length, keys,
        key_count, error);
    if (reason == NULL) {
        for (size_t at = 0U; at < image->length; at += step) {
            const size_t piece = image->length - at < step ? image->length - at : step;
            assert(jetkvm_ota_image_check_update(&check, image->data + at, piece));
        }
        reason = jetkvm_ota_image_check_end(&check, expected_sha256, now, error);
    }
    assert(host_hashes_open == 0);
    return reason;
}

/* The same against a signature file. */
static const char *check_image_at(const file_t *image, const char *signature_name,
                                  const char *expected_sha256,
                                  const osvc_pgp_key_t *keys, size_t key_count,
                                  size_t step, uint32_t now)
{
    file_t signature = {NULL, 0U};
    if (signature_name != NULL) {
        signature = load(signature_name);
    }
    const char *reason = check_image_with(image, &signature, expected_sha256,
                                          keys, key_count, step, now);
    free(signature.data);
    return reason;
}

/* The same with an unknown time: no expiry is checked. */
static const char *check_image(const file_t *image, const char *signature_name,
                               const char *expected_sha256,
                               const osvc_pgp_key_t *keys, size_t key_count,
                               size_t step)
{
    return check_image_at(image, signature_name, expected_sha256, keys,
                          key_count, step, 0U);
}

static void test_image_check(void)
{
    osvc_pgp_key_t keys[JETKVM_OTA_SIGNING_KEYS_MAX];
    size_t count = 0U;
    assert(read_keys("root.asc", FIXTURE_ROOT_FINGERPRINT, 0U, keys, &count) == NULL);
    file_t image = load("image.bin");
    char sha[65];
    sha256_hex(&image, sha);

    /* A valid signature, of either subkey, over either hash, with the image
     * in one piece and in many. */
    assert(check_image(&image, "image.bin.sig", sha, keys, count, image.length) == NULL);
    assert(check_image(&image, "image.bin.sig", sha, keys, count, 1000U) == NULL);
    assert(check_image(&image, "image.bin.sha256.sig", sha, keys, count, 7U) == NULL);
    assert(check_image(&image, "image.bin.subkey2.sig", sha, keys, count, 4096U) == NULL);
    /* The update server's hash in capitals. */
    char upper[65];
    for (size_t i = 0U; i < sizeof(upper); ++i) {
        upper[i] = sha[i] >= 'a' && sha[i] <= 'f' ? (char)(sha[i] - 32) : sha[i];
    }
    assert(check_image(&image, "image.bin.sig", upper, keys, count, 1000U) == NULL);

    /* An update that needs no signature is checked against the hash alone. */
    assert(check_image(&image, NULL, sha, NULL, 0U, 1000U) == NULL);

    /* Keys that were read before subkey 2 expired, used after it: its
     * signature is refused, and that of subkey 1, which does not expire, is
     * not. */
    const uint32_t after_expiry = FIXTURE_SUBKEY2_CREATED + 366U * 24U * 60U * 60U;
    const char *expired = check_image_at(&image, "image.bin.subkey2.sig", sha,
                                         keys, count, 1000U, after_expiry);
    assert(expired != NULL);
    assert(strcmp(expired, "GPG signature verification failed: a signature by "
                           "an expired key") == 0);
    assert(check_image_at(&image, "image.bin.sig", sha, keys, count, 1000U,
                          after_expiry) == NULL);

    /* A signature of another key ends the check before the image. */
    const char *reason = check_image(&image, "image.bin.other.sig", sha, keys, count, 1000U);
    assert(reason != NULL);
    assert(strcmp(reason, "GPG signature verification failed: no signature by "
                          "the JetKVM release key") == 0);
    /* So does the signature of a revoked subkey. */
    osvc_pgp_key_t kept[JETKVM_OTA_SIGNING_KEYS_MAX];
    size_t kept_count = 0U;
    assert(read_keys("root-revoked.asc", FIXTURE_ROOT_FINGERPRINT, 0U, kept,
                     &kept_count) == NULL);
    reason = check_image(&image, "image.bin.sig", sha, kept, kept_count, 1000U);
    assert(reason != NULL && strstr(reason, "no signature by") != NULL);
    assert(check_image(&image, "image.bin.subkey2.sig", sha, kept, kept_count, 1000U) == NULL);
    /* A file that is no signature. */
    reason = check_image(&image, "root.h", sha, keys, count, 1000U);
    assert(reason != NULL && strstr(reason, "GPG signature verification failed: ") == reason);

    /* The update server's hash is another image's. */
    char other_sha[65];
    memcpy(other_sha, sha, sizeof(other_sha));
    other_sha[0] = other_sha[0] == '0' ? '1' : '0';
    reason = check_image(&image, "image.bin.sig", other_sha, keys, count, 1000U);
    assert(reason != NULL && strstr(reason, "hash mismatch: ") == reason);
    reason = check_image(&image, NULL, other_sha, NULL, 0U, 1000U);
    assert(reason != NULL && strstr(reason, "hash mismatch: ") == reason);

    /* An image that the key did not sign, with the hash that the update
     * server gives for it: any changed octet, the first and the last too. */
    const size_t positions[] = {0U, image.length / 2U, image.length - 1U};
    for (size_t i = 0U; i < 3U; ++i) {
        image.data[positions[i]] ^= 0x20U;
        char changed_sha[65];
        sha256_hex(&image, changed_sha);
        reason = check_image(&image, "image.bin.sig", changed_sha, keys, count, 1000U);
        assert(reason != NULL);
        assert(strcmp(reason, "GPG signature verification failed: the signature "
                              "does not match the data") == 0);
        image.data[positions[i]] ^= 0x20U;
    }
    /* A truncated image, and one with an octet more. */
    file_t shorter = {image.data, image.length - 1U};
    char shorter_sha[65];
    sha256_hex(&shorter, shorter_sha);
    reason = check_image(&shorter, "image.bin.sig", shorter_sha, keys, count, 1000U);
    assert(reason != NULL && strstr(reason, "does not match the data") != NULL);

    /* The signed image with a signature whose last octet, the end of the
     * Ed25519 S, is changed: the hash and its first 16 bits in the packet
     * are the signed ones, and the Ed25519 equation fails. */
    file_t forged = load("image.bin.sig");
    forged.data[forged.length - 1U] ^= 0x01U;
    reason = check_image_with(&image, &forged, sha, keys, count, 1000U, 0U);
    assert(reason != NULL);
    assert(strcmp(reason, "GPG signature verification failed: the signature "
                          "does not match the data") == 0);
    free(forged.data);
    free(image.data);
}

/* A release key with a third signing subkey, whose expiry the primary key
 * extended from one year to three: a keyserver serves the older and the
 * newer binding signature, in either order, and the newer one counts. */
static void test_extended_subkey(void)
{
    static const char *const FILES[] = {
        "root-extended.asc", "root-extended-newest-first.asc",
    };
    const uint32_t day = 24U * 60U * 60U;
    const uint32_t year = 365U * day;
    file_t image = load("image.bin");
    char sha[65];
    sha256_hex(&image, sha);
    for (size_t f = 0U; f < 2U; ++f) {
        osvc_pgp_key_t keys[JETKVM_OTA_SIGNING_KEYS_MAX];
        size_t count = 0U;
        assert(read_keys(FILES[f], FIXTURE_ROOT_FINGERPRINT, 0U, keys, &count) == NULL);
        assert(count == 3U);
        assert(has_key(keys, count, FIXTURE_SUBKEY1_FINGERPRINT));
        assert(has_key(keys, count, FIXTURE_SUBKEY2_FINGERPRINT));
        assert(has_key(keys, count, FIXTURE_SUBKEY3_FINGERPRINT));

        /* After the first year subkey 2 has expired and subkey 3 has not,
         * and the signature of subkey 3 verifies. */
        const uint32_t second_year = FIXTURE_SUBKEY3_CREATED + year + day;
        assert(read_keys(FILES[f], FIXTURE_ROOT_FINGERPRINT, second_year, keys,
                         &count) == NULL);
        assert(count == 2U);
        assert(has_key(keys, count, FIXTURE_SUBKEY1_FINGERPRINT));
        assert(has_key(keys, count, FIXTURE_SUBKEY3_FINGERPRINT));
        assert(check_image_at(&image, "image.bin.subkey3.sig", sha, keys, count,
                              1000U, second_year) == NULL);

        /* After three years subkey 3 has expired too. */
        assert(read_keys(FILES[f], FIXTURE_ROOT_FINGERPRINT,
                         FIXTURE_SUBKEY3_CREATED + 3U * year + 2U * day, keys,
                         &count) == NULL);
        assert(count == 1U && has_key(keys, count, FIXTURE_SUBKEY1_FINGERPRINT));
    }
    /* The key without subkey 3 does not take its signature. */
    osvc_pgp_key_t keys[JETKVM_OTA_SIGNING_KEYS_MAX];
    size_t count = 0U;
    assert(read_keys("root.asc", FIXTURE_ROOT_FINGERPRINT, 0U, keys, &count) == NULL);
    const char *reason = check_image(&image, "image.bin.subkey3.sig", sha, keys, count, 1000U);
    assert(reason != NULL && strstr(reason, "no signature by") != NULL);
    free(image.data);
}

int main(void)
{
    test_bypass_rule();
    test_signature_download();
    test_keyring();
    test_real_key();
    test_key_fetch();
    test_image_check();
    test_extended_subkey();
    puts("test_ota_signature: all assertions passed");
    return 0;
}

/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The update relay's checks against files that GnuPG signed (fixtures/,
 * made by fixtures/make_fixtures.sh with throwaway keys), with the signing
 * keys that the firmware update's key fetch (jetkvm_ota_release_keys) reads
 * from a stand-in keyserver's answer, and the browser's guestUpdate object
 * against its share of the videoInputState event. Hashes come from OpenSSL,
 * not from the code under test.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/sha.h>

#include "fixtures/test_release_key.h"
#include "host_hash.h"
#include "jetkvm_ota_signature.h"
#include "os_services_release.h"
#include "os_services_update.h"
#include "os_services_wire.h"

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
    file_t file = {malloc(1U << 20), 0U};
    assert(file.data != NULL);
    file.length = fread(file.data, 1U, 1U << 20, f);
    fclose(f);
    return file;
}

/* --- The release key as a keyserver serves it ---------------------------- */

/* Both keyservers: each answers status with file (NULL for an empty body),
 * or gives no answer when status is negative. */
typedef struct {
    int status;
    const char *file;
    int calls;
} keyserver_t;

static int keyserver_get(void *ctx, const char *url, uint8_t *body,
                         size_t capacity, size_t *length)
{
    keyserver_t *server = ctx;
    (void)url;
    server->calls++;
    *length = 0U;
    if (server->status >= 0 && server->file != NULL) {
        file_t file = load(server->file);
        assert(file.length <= capacity);
        memcpy(body, file.data, file.length);
        *length = file.length;
        free(file.data);
    }
    return server->status;
}

/* The signing keys that the checks use, as the device copies them out of
 * its key fetch: none when the fetch fails. */
static osvc_pgp_key_t s_keys[JETKVM_OTA_SIGNING_KEYS_MAX];
static size_t s_key_count;

/* Fetches the test release key from server into s_keys, as the device does
 * for the JetKVM release key. now is seconds since 1970, or 0. */
static const char *fetch_keys(keyserver_t *server, uint32_t now)
{
    static const char *const KEYSERVERS[] = {
        JETKVM_OTA_KEYSERVER_OPENPGP,
        JETKVM_OTA_KEYSERVER_UBUNTU,
    };
    static uint8_t scratch[JETKVM_OTA_KEY_TEXT_MAX];
    static char error[JETKVM_OTA_SIGNATURE_ERROR_MAX];
    jetkvm_ota_release_keys_t cache = {0};
    const char *reason = jetkvm_ota_release_keys(
        &cache, OSVC_TEST_ROOT_FINGERPRINT, KEYSERVERS, 2U, keyserver_get,
        server, &host_hash_ops, 0, now, scratch, error);
    assert(host_hashes_open == 0);
    s_key_count = reason == NULL ? cache.count : 0U;
    memcpy(s_keys, cache.keys, sizeof(s_keys));
    return reason;
}

static const char *verify_at(const file_t *signature, const file_t *data,
                             uint32_t now)
{
    const char *error = osvc_pgp_verify_detached(
        signature->data, signature->length, data->data, data->length, s_keys,
        s_key_count, &host_hash_ops, now);
    assert(host_hashes_open == 0);
    return error;
}

/* The package, read from memory and handed to no sender. */
static bool package_read(void *ctx, uint8_t *data, size_t length)
{
    file_t *package = ctx;
    memcpy(data, package->data, length);
    package->data += length;
    return true;
}

static bool package_send(void *ctx, const uint8_t *data, size_t length)
{
    (void)ctx;
    (void)data;
    (void)length;
    return true;
}

/* The relay's check of fixtures/package.bin with the signature in
 * signature_file, at now. */
static const char *relay_package_at(const char *signature_file, uint32_t now)
{
    file_t package = load("package.bin");
    file_t signature = load(signature_file);
    uint8_t sha256[32];
    SHA256(package.data, package.length, sha256);
    osvc_package_check_t check;
    const char *error = osvc_package_check_begin(
        &check, &host_hash_ops, package.length, sha256, signature.data,
        signature.length, s_keys, s_key_count);
    if (error == NULL) {
        static uint8_t buffer_a[OSVC_CHUNK_MAX_DATA];
        static uint8_t buffer_b[OSVC_CHUNK_MAX_DATA];
        uint8_t *const buffers[2] = {buffer_a, buffer_b};
        file_t cursor = package;
        const osvc_relay_io_t io = {&cursor, package_read, package_send};
        error = osvc_relay_package(&io, &check, buffers, now);
    }
    assert(host_hashes_open == 0);
    free(package.data);
    free(signature.data);
    return error;
}

static void test_keyserver_key(void)
{
    file_t index = load("releases.json");
    file_t sig = load("releases.json.sig");
    file_t expiring = load("releases.json.expiring.sig");
    file_t wrong = load("releases.json.wrong-key.sig");

    /* The keyserver serves the release key with both signing subkeys: a
     * release signed by either is accepted, the index and the package. */
    keyserver_t server = {200, "test_release_key.asc", 0};
    assert(fetch_keys(&server, 0U) == NULL);
    assert(server.calls == 1 && s_key_count == 2U);
    assert(verify_at(&sig, &index, 0U) == NULL);
    assert(verify_at(&expiring, &index, 0U) == NULL);
    assert(relay_package_at("package.bin.sig", 0U) == NULL);
    assert(relay_package_at("package.bin.expiring.sig", 0U) == NULL);

    /* A key that the keyserver does not serve as a subkey of the release
     * key signs nothing that is accepted. */
    assert(strcmp(verify_at(&wrong, &index, 0U),
                  "no signature by the JetKVM release key") == 0);

    /* One subkey expires. Its expiry is the keyserver's, read by the
     * fetch. */
    uint32_t expires = 0U;
    for (size_t i = 0U; i < s_key_count; ++i) {
        expires = s_keys[i].expires != 0U ? s_keys[i].expires : expires;
    }
    assert(expires != 0U);
    /* Fetched before its expiry, it signs until then and not after. */
    assert(fetch_keys(&server, expires - 60U) == NULL && s_key_count == 2U);
    assert(verify_at(&expiring, &index, expires - 60U) == NULL);
    assert(strcmp(verify_at(&expiring, &index, expires + 60U),
                  "a signature by an expired key") == 0);
    assert(strcmp(relay_package_at("package.bin.expiring.sig", expires + 60U),
                  "the package is not signed with the JetKVM release key") == 0);
    assert(verify_at(&sig, &index, expires + 60U) == NULL);
    /* Fetched after its expiry, it is no signing key. */
    assert(fetch_keys(&server, expires + 60U) == NULL && s_key_count == 1U);
    assert(strcmp(verify_at(&expiring, &index, expires + 60U),
                  "no signature by the JetKVM release key") == 0);
    assert(verify_at(&sig, &index, expires + 60U) == NULL);
    assert(relay_package_at("package.bin.sig", expires + 60U) == NULL);

    /* No keyserver serves the key: both are asked, there are no keys, and
     * no release is accepted. */
    const keyserver_t missing[] = {
        {404, NULL, 0}, {-1, NULL, 0}, {200, NULL, 0},
    };
    static const char failed[] = "failed to fetch public key: all keyservers failed: ";
    for (size_t i = 0U; i < sizeof(missing) / sizeof(missing[0]); ++i) {
        server = missing[i];
        const char *error = fetch_keys(&server, 0U);
        assert(error != NULL && strncmp(error, failed, strlen(failed)) == 0);
        assert(server.calls == 2 && s_key_count == 0U);
        assert(strcmp(verify_at(&sig, &index, 0U),
                      "no signature by the JetKVM release key") == 0);
        assert(strcmp(relay_package_at("package.bin.sig", 0U),
                      "no signature by the JetKVM release key") == 0);
    }

    /* The keys of the remaining checks. */
    server = (keyserver_t){200, "test_release_key.asc", 0};
    assert(fetch_keys(&server, 0U) == NULL);
    free(index.data);
    free(sig.data);
    free(expiring.data);
    free(wrong.data);
}

static void test_signature_checks(void)
{
    file_t index = load("releases.json");
    file_t sig = load("releases.json.sig");
    file_t sha512 = load("releases.json.sha512.sig");
    assert(verify_at(&sig, &index, 0U) == NULL);
    assert(verify_at(&sha512, &index, 0U) == NULL);

    /* Any changed byte of the data fails, the first and the last too. */
    const size_t positions[] = {0U, index.length / 2U, index.length - 1U};
    for (size_t i = 0U; i < 3U; ++i) {
        index.data[positions[i]] ^= 0x20U;
        assert(strcmp(verify_at(&sig, &index, 0U),
                      "the signature does not match the data") == 0);
        index.data[positions[i]] ^= 0x20U;
    }
    /* A flipped bit of R or S fails on the equation, not the prefix. */
    sig.data[sig.length - 5U] ^= 1U;
    assert(strcmp(verify_at(&sig, &index, 0U),
                  "the signature does not match the data") == 0);
    sig.data[sig.length - 5U] ^= 1U;

    /* SHA-1 is refused; a signature relabelled from SHA-256 to SHA-512
     * hashes differently and fails. */
    file_t sha1 = load("releases.json.sha1.sig");
    assert(strcmp(verify_at(&sha1, &index, 0U),
                  "a hash algorithm other than SHA-256, SHA-384 or SHA-512") == 0);
    assert(sig.data[2] == 4U && sig.data[5] == OSVC_PGP_HASH_SHA256);
    sig.data[5] = OSVC_PGP_HASH_SHA512;
    assert(verify_at(&sig, &index, 0U) != NULL);
    sig.data[5] = OSVC_PGP_HASH_SHA256;

    /* Truncated packets never verify, and never read past the end. */
    for (size_t length = 0U; length < sig.length; ++length) {
        file_t cut = {sig.data, length};
        assert(verify_at(&cut, &index, 0U) != NULL);
    }
    free(index.data);
    free(sig.data);
    free(sha512.data);
    free(sha1.data);
}

static void read_index(const char *name, bool is_signed, osvc_release_t *newest)
{
    file_t index = load(name);
    assert(osvc_release_read_index((const char *)index.data, index.length,
                                   is_signed, newest) == NULL);
    free(index.data);
}

/* The newest release in range per index, its valid packages only. */
static void test_release_index(void)
{
    static osvc_release_t stable;
    static osvc_release_t dev;
    read_index("releases.json", true, &stable);
    /* 0.5.0 is out of range, 0.4.4-rc.1 is a prerelease in the signed
     * index, "0.4.x" does not parse. */
    assert(strcmp(stable.version, "0.4.3") == 0 && stable.is_signed);
    /* The macOS path leaves the bucket and the Windows size is 0. */
    assert(stable.package_count == 1U);
    const osvc_release_package_t *linux_package =
        osvc_release_package(&stable, "linux-x86_64-appimage");
    assert(linux_package != NULL && linux_package->size == 70000U);
    assert(strcmp(linux_package->path, "0.4.3/os-services-0.4.3.AppImage") == 0);
    file_t package = load("package.bin");
    uint8_t digest[32];
    SHA256(package.data, package.length, digest);
    assert(memcmp(linux_package->sha256, digest, 32U) == 0);
    assert(osvc_release_package(&stable, "macos-aarch64") == NULL);
    assert(osvc_release_package(&stable, "") == NULL);

    /* The dev index counts only prereleases; dev.10 is newer than dev.2. */
    read_index("releases-dev.json", false, &dev);
    assert(strcmp(dev.version, "0.4.4-dev.10") == 0 && !dev.is_signed);
    assert(osvc_release_newest(&stable, &dev, false) == &stable);
    assert(osvc_release_newest(&stable, &dev, true) == &dev);
    static const osvc_release_t none;
    assert(osvc_release_newest(&none, &none, true) == NULL);
    assert(osvc_release_newest(&none, &dev, false) == NULL);

    /* The sender gets a release newer than its own, in its package. */
    osvc_update_sender_t sender = {"0.4.2", "linux-x86_64-appimage"};
    assert(osvc_release_target(&stable, &sender) == linux_package);
    snprintf(sender.version, sizeof(sender.version), "0.4.3");
    assert(osvc_release_target(&stable, &sender) == NULL);
    snprintf(sender.version, sizeof(sender.version), "0.4.3-dev.1");
    assert(osvc_release_target(&stable, &sender) == linux_package);
    snprintf(sender.package, sizeof(sender.package), "windows-x86_64");
    assert(osvc_release_target(&stable, &sender) == NULL);

    /* Unreadable indexes. */
    static const char *const bad[] = {
        "", "{}", "{\"releases\":{}}", "{\"releases\":[{\"version\":\"0.4.1\"}",
        "{\"releases\":[{\"version\":\"0.4.1\",}]}", "{\"releases\":[1,,2]}",
        "{\"other\":{\"releases\":[]}}",
    };
    for (size_t i = 0U; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(osvc_release_read_index(bad[i], strlen(bad[i]), true, &dev) != NULL);
    }
    static const char empty[] = " { \"releases\" : [ ] } ";
    assert(osvc_release_read_index(empty, strlen(empty), true, &dev) == NULL);
    assert(dev.version[0] == '\0');
    /* Compact JSON: no whitespace after the commas between releases. */
    static const char compact[] =
        "{\"releases\":[{\"version\":\"0.4.4-dev.2\",\"packages\":{}},"
        "{\"version\":\"0.4.4-dev.3\",\"packages\":{}}]}";
    assert(osvc_release_read_index(compact, strlen(compact), false, &dev) == NULL);
    assert(strcmp(dev.version, "0.4.4-dev.3") == 0);
    free(package.data);
}

static size_t rendered_length(const osvc_release_t *newest,
                              const osvc_update_sender_t *sender,
                              const osvc_update_progress_t *progress,
                              char **text)
{
    cJSON *object = cJSON_CreateObject();
    assert(osvc_update_render(object, newest, sender, progress, true));
    *text = cJSON_PrintUnformatted(object);
    cJSON_Delete(object);
    /* As the event carries it: ,"guestUpdate":{...} */
    return strlen(*text) - 1U;
}

/* What the browser reads, and the longest it can be. */
static void test_guest_update_state(void)
{
    static osvc_release_t stable;
    read_index("releases.json", true, &stable);
    const osvc_update_sender_t sender = {"0.4.1", "linux-x86_64-appimage"};
    osvc_update_progress_t progress = {0};
    char *text = NULL;
    rendered_length(&stable, &sender, &progress, &text);
    assert(strcmp(text,
                  "{\"guestUpdate\":{\"version\":\"0.4.3\",\"available\":true,"
                  "\"installable\":true,\"automatic\":true}}") == 0);
    cJSON_free(text);

    /* A failed update shows until a newer release replaces it. */
    progress = (osvc_update_progress_t){OSVC_UPDATE_PHASE_FAILED, "0.4.2", 10U,
                                        70000U, "the package download failed"};
    rendered_length(&stable, &sender, &progress, &text);
    assert(strstr(text, "\"phase\"") == NULL);
    cJSON_free(text);
    snprintf(progress.version, sizeof(progress.version), "0.4.3");
    rendered_length(&stable, &sender, &progress, &text);
    assert(strstr(text, "\"phase\":\"failed\",\"sent\":10,\"size\":70000,"
                        "\"error\":\"the package download failed\"") != NULL);
    cJSON_free(text);
    /* Nothing while no release is known and nothing ran. */
    progress.phase = OSVC_UPDATE_PHASE_NONE;
    rendered_length(NULL, &sender, &progress, &text);
    assert(strcmp(text, "{}") == 0);
    cJSON_free(text);

    /* The longest: 64-byte version and error, the largest sizes. */
    osvc_release_t longest = {0};
    memset(longest.version, '9', OSVC_UPDATE_VERSION_MAX);
    progress.phase = OSVC_UPDATE_PHASE_DOWNLOADING;
    memcpy(progress.version, longest.version, sizeof(progress.version));
    memset(progress.error, 'e', OSVC_UPDATE_MESSAGE_CAPACITY - 1U);
    progress.sent = progress.size = OSVC_RELEASE_PACKAGE_MAX_BYTES;
    const size_t length = rendered_length(&longest, &sender, &progress, &text);
    printf("guestUpdate: %zu of %u bytes at most\n", length,
           OSVC_UPDATE_STATE_JSON_MAX);
    assert(length <= OSVC_UPDATE_STATE_JSON_MAX);
    cJSON_free(text);
}

int main(void)
{
    test_keyserver_key();
    test_signature_checks();
    test_release_index();
    test_guest_update_state();
    printf("os_services update tests passed\n");
    return 0;
}

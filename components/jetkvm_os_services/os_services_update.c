/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "os_services_update.h"

#include <string.h>

#include "os_services_wire.h"

const char *osvc_package_check_begin(osvc_package_check_t *check,
                                     const osvc_hash_ops_t *ops, uint64_t size,
                                     const uint8_t sha256[32],
                                     const uint8_t *signature,
                                     size_t signature_length,
                                     const osvc_pgp_key_t *keys,
                                     size_t key_count)
{
    memset(check, 0, sizeof(*check));
    check->ops = ops;
    check->size = size;
    memcpy(check->expected, sha256, sizeof(check->expected));
    if (signature != NULL) {
        const char *error = osvc_pgp_find_signature(
            signature, signature_length, keys, key_count, &check->signature,
            &check->key);
        if (error != NULL) {
            return error;
        }
        check->signed_hash = ops->begin(ops->ctx, check->signature.hash_algorithm);
    }
    check->sha256 = ops->begin(ops->ctx, OSVC_PGP_HASH_SHA256);
    if (check->sha256 == NULL ||
        (signature != NULL && check->signed_hash == NULL)) {
        osvc_package_check_abort(check);
        return "no memory to hash the package";
    }
    return NULL;
}

void osvc_package_check_abort(osvc_package_check_t *check)
{
    void *hashes[2] = {check->sha256, check->signed_hash};
    for (size_t i = 0U; i < 2U; ++i) {
        if (hashes[i] != NULL) {
            check->ops->abort(check->ops->ctx, hashes[i]);
        }
    }
    check->sha256 = NULL;
    check->signed_hash = NULL;
}

static bool check_update(osvc_package_check_t *check, const uint8_t *data,
                         size_t length)
{
    const osvc_hash_ops_t *ops = check->ops;
    check->received += length;
    return ops->update(ops->ctx, check->sha256, data, length) &&
           (check->signed_hash == NULL ||
            ops->update(ops->ctx, check->signed_hash, data, length));
}

/* The verdict on the whole package. Ends the checks. */
static const char *check_finish(osvc_package_check_t *check, uint32_t now)
{
    const osvc_hash_ops_t *ops = check->ops;
    uint8_t digest[32];
    size_t length = 0U;
    const bool hashed =
        ops->finish(ops->ctx, check->sha256, digest, sizeof(digest), &length);
    check->sha256 = NULL;
    if (!hashed || length != sizeof(digest)) {
        osvc_package_check_abort(check);
        return "the package could not be hashed";
    }
    if (memcmp(digest, check->expected, sizeof(digest)) != 0) {
        osvc_package_check_abort(check);
        return "the package does not match the release index";
    }
    if (check->signed_hash == NULL) {
        return NULL;
    }
    void *hash = check->signed_hash;
    check->signed_hash = NULL;
    return osvc_pgp_verify_hash(&check->signature, OSVC_PGP_SIG_BINARY,
                                check->key, ops, hash, now) == NULL ?
               NULL :
               "the package is not signed with the JetKVM release key";
}

const char *osvc_relay_package(const osvc_relay_io_t *io,
                               osvc_package_check_t *check,
                               uint8_t *const buffers[2], uint32_t now)
{
    for (unsigned i = 0U; check->received < check->size; i ^= 1U) {
        const uint64_t left = check->size - check->received;
        const size_t length =
            left < OSVC_CHUNK_MAX_DATA ? (size_t)left : OSVC_CHUNK_MAX_DATA;
        if (!io->read(io->ctx, buffers[i], length)) {
            osvc_package_check_abort(check);
            return "the package download failed";
        }
        if (!check_update(check, buffers[i], length)) {
            osvc_package_check_abort(check);
            return "the package could not be hashed";
        }
        /* The last chunk waits for the verdict. */
        const char *error =
            check->received == check->size ? check_finish(check, now) : NULL;
        if (error != NULL) {
            return error;
        }
        if (!io->send(io->ctx, buffers[i], length)) {
            osvc_package_check_abort(check);
            return "the update ended";
        }
    }
    return NULL;
}

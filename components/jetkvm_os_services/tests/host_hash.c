/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "host_hash.h"

#include <openssl/evp.h>

int host_hashes_open;

static void *hash_begin(void *ctx, osvc_pgp_hash_t algorithm)
{
    (void)ctx;
    const EVP_MD *md = algorithm == OSVC_PGP_HASH_SHA1 ? EVP_sha1() :
                       algorithm == OSVC_PGP_HASH_SHA256 ? EVP_sha256() :
                       algorithm == OSVC_PGP_HASH_SHA384 ? EVP_sha384() :
                       algorithm == OSVC_PGP_HASH_SHA512 ? EVP_sha512() : NULL;
    EVP_MD_CTX *hash = md != NULL ? EVP_MD_CTX_new() : NULL;
    if (hash == NULL || EVP_DigestInit_ex(hash, md, NULL) != 1) {
        EVP_MD_CTX_free(hash);
        return NULL;
    }
    host_hashes_open++;
    return hash;
}

static bool hash_update(void *ctx, void *hash, const uint8_t *data,
                        size_t length)
{
    (void)ctx;
    return EVP_DigestUpdate(hash, data, length) == 1;
}

static void hash_abort(void *ctx, void *hash)
{
    (void)ctx;
    EVP_MD_CTX_free(hash);
    host_hashes_open--;
}

static bool hash_finish(void *ctx, void *hash, uint8_t *digest, size_t capacity,
                        size_t *length)
{
    unsigned int written = 0U;
    const bool ok = (size_t)EVP_MD_CTX_get_size(hash) <= capacity &&
                    EVP_DigestFinal_ex(hash, digest, &written) == 1;
    *length = written;
    hash_abort(ctx, hash);
    return ok;
}

const osvc_hash_ops_t host_hash_ops = {
    .begin = hash_begin,
    .update = hash_update,
    .finish = hash_finish,
    .abort = hash_abort,
};

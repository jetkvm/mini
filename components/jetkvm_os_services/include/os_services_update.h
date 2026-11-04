/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The checks on an OS Services release package as it streams through the
 * KVM, and the order that keeps an unchecked package from reaching the
 * sender: every chunk but the last goes to the sender as it arrives, and the
 * last one only once the size, the SHA-256 from the index and the package's
 * own signature check out. A sender without the last chunk cannot complete
 * its hash, so it installs nothing. Pure C: the device and the host tests
 * supply the hashing, the download and the link to the sender.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os_services_pgp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const osvc_hash_ops_t *ops;
    /* The SHA-256 the index lists, and the hash the signature covers (NULL
     * for a dev package, which carries no signature). */
    void *sha256;
    void *signed_hash;
    osvc_pgp_signature_t signature;
    const osvc_pgp_key_t *key;
    uint8_t expected[32];
    uint64_t size;
    uint64_t received;
} osvc_package_check_t;

/*
 * Starts the checks of a package of size bytes. signature is the package's
 * .sig file, or NULL for a dev package. Returns NULL, or why the package
 * cannot be checked (nothing then needs ending).
 */
const char *osvc_package_check_begin(osvc_package_check_t *check,
                                     const osvc_hash_ops_t *ops, uint64_t size,
                                     const uint8_t sha256[32],
                                     const uint8_t *signature,
                                     size_t signature_length,
                                     const osvc_pgp_key_t *keys,
                                     size_t key_count);

/* Ends the checks without a verdict. */
void osvc_package_check_abort(osvc_package_check_t *check);

/* How the relay reaches the download and the sender. */
typedef struct {
    void *ctx;
    /* Fills data with exactly length more package bytes; false when the
     * download failed or was stopped. */
    bool (*read)(void *ctx, uint8_t *data, size_t length);
    /* Hands data to the sender; false once the update has ended. The bytes
     * must stay valid until the next send returns: the device sends from one
     * buffer while the next download fills the other. */
    bool (*send)(void *ctx, const uint8_t *data, size_t length);
} osvc_relay_io_t;

/*
 * Downloads the package through io in chunks of at most 65,524 bytes,
 * alternating between buffers[0] and buffers[1], checks it and hands it to
 * the sender, the last chunk only after every check passed. Ends the checks.
 * now (seconds since 1970, or 0) is for the signature. Returns NULL once the
 * sender has every byte, or why it must not install (the caller then sends
 * update.cancel).
 */
const char *osvc_relay_package(const osvc_relay_io_t *io,
                               osvc_package_check_t *check,
                               uint8_t *const buffers[2], uint32_t now);

#ifdef __cplusplus
}
#endif

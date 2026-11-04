/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * A minimal OpenPGP detached signature check (RFC 4880, RFC 9580) for the
 * OS Services releases: one version 4 signature of a binary document
 * (type 0x00), made by an Ed25519 key (EdDSA, algorithm 22, or Ed25519,
 * algorithm 27) over SHA-256, SHA-384 or SHA-512. The caller gives the
 * signing keys. The firmware update and the OS Services releases read them
 * from the release key as a keyserver serves it (osvc_pgp_read_keyring),
 * which checks each signing subkey's binding to the primary key. Nothing
 * here fetches a key.
 *
 * Hashing goes through osvc_hash_ops_t, so the device uses PSA and the host
 * tests OpenSSL; the Ed25519 equation is Monocypher's. Nothing here depends
 * on ESP-IDF. The check needs about 2 KiB of stack (Monocypher's Ed25519).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OSVC_PGP_UNUSED __attribute__((unused))

#define OSVC_PGP_FINGERPRINT_LEN 20U
#define OSVC_PGP_KEY_ID_LEN 8U
#define OSVC_PGP_ED25519_KEY_LEN 32U
#define OSVC_PGP_DIGEST_MAX 64U

/* Signature types (RFC 9580 5.2.1). */
#define OSVC_PGP_SIG_BINARY 0x00U
#define OSVC_PGP_SIG_SUBKEY_BINDING 0x18U
#define OSVC_PGP_SIG_PRIMARY_BINDING 0x19U
#define OSVC_PGP_SIG_KEY_REVOCATION 0x20U
#define OSVC_PGP_SIG_SUBKEY_REVOCATION 0x28U

/* Key flags (5.2.3.29): the key may sign data. */
#define OSVC_PGP_KEY_FLAG_SIGN 0x02U

/* Public key algorithms. */
#define OSVC_PGP_ALGO_EDDSA_LEGACY 22U
#define OSVC_PGP_ALGO_ED25519 27U

/* Hash algorithms, by their OpenPGP IDs. A signature is accepted only over
 * SHA-256, SHA-384 or SHA-512; SHA-1 gives a version 4 key its fingerprint
 * (osvc_pgp_read_keyring) and nothing else. */
typedef enum {
    OSVC_PGP_HASH_SHA1 = 2,
    OSVC_PGP_HASH_SHA256 = 8,
    OSVC_PGP_HASH_SHA384 = 9,
    OSVC_PGP_HASH_SHA512 = 10,
} osvc_pgp_hash_t;

/* An Ed25519 signing key. */
typedef struct {
    uint8_t fingerprint[OSVC_PGP_FINGERPRINT_LEN];
    uint8_t public_key[OSVC_PGP_ED25519_KEY_LEN];
    /* Key creation time, seconds since 1970. */
    uint32_t created;
    /* When the key expires, seconds since 1970; 0 means never.
     * osvc_pgp_read_keyring sets it. */
    uint32_t expires;
} osvc_pgp_key_t;

/*
 * Hashing. begin returns a running hash of one algorithm, or NULL when it is
 * unsupported or there is no memory; finish writes the digest and ends it,
 * and abort ends it without one. A hash is used by one task at a time.
 */
typedef struct {
    void *ctx;
    void *(*begin)(void *ctx, osvc_pgp_hash_t algorithm);
    bool (*update)(void *ctx, void *hash, const uint8_t *data, size_t length);
    bool (*finish)(void *ctx, void *hash, uint8_t *digest, size_t capacity,
                   size_t *length);
    void (*abort)(void *ctx, void *hash);
} osvc_hash_ops_t;

/*
 * One parsed signature packet. hashed points into the caller's signature
 * bytes, which must outlive the structure.
 */
typedef struct {
    uint8_t type;
    uint8_t public_key_algorithm;
    osvc_pgp_hash_t hash_algorithm;
    uint32_t created;
    /* Seconds after created; 0 means none. */
    uint32_t expires_after;
    bool has_issuer_fingerprint;
    uint8_t issuer_fingerprint[OSVC_PGP_FINGERPRINT_LEN];
    bool has_issuer_key_id;
    uint8_t issuer_key_id[OSVC_PGP_KEY_ID_LEN];
    /* Of a subkey binding: the subkey's flags (0 when absent), the seconds
     * after the subkey's creation at which it expires (0 means never), and
     * the embedded signature's packet body (NULL when absent). */
    uint8_t key_flags;
    uint32_t key_expires_after;
    const uint8_t *embedded;
    size_t embedded_length;
    uint8_t hash_prefix[2];
    /* R and S of the Ed25519 signature, as RFC 8032 encodes them. */
    uint8_t signature[64];
    /* Version through the hashed subpackets: the part the digest covers. */
    const uint8_t *hashed;
    size_t hashed_length;
} osvc_pgp_signature_t;

/*
 * Finds the first signature packet in data (a binary .sig file: one or more
 * signature packets) that one of keys made, and parses it. Returns NULL with
 * *signature and *key set, or why there is none: a malformed packet, an
 * unsupported version, algorithm or hash, an unknown critical subpacket, no
 * creation time, or no signature by one of keys. Never reads past length.
 */
const char *osvc_pgp_find_signature(const uint8_t *data, size_t length,
                                    const osvc_pgp_key_t *keys,
                                    size_t key_count,
                                    osvc_pgp_signature_t *signature,
                                    const osvc_pgp_key_t **key);

/*
 * Ends a hash that has taken the signed data: adds the signature's trailer
 * (RFC 9580 5.2.4), finishes it and checks the signature type, the left 16
 * bits, the Ed25519 equation, that the key existed when the signature was
 * made, and that neither the signature nor the key has expired. now is
 * seconds since 1970, or 0 when the clock is unknown (no expiry check then).
 * Always ends the hash. Returns NULL or why the signature is not valid.
 */
const char *osvc_pgp_verify_hash(const osvc_pgp_signature_t *signature,
                                 uint8_t expected_type,
                                 const osvc_pgp_key_t *key,
                                 const osvc_hash_ops_t *ops, void *hash,
                                 uint32_t now);

/*
 * The whole check of a detached signature over data held in memory: find,
 * hash, verify. Returns NULL or why the data is not signed by one of keys.
 */
const char *osvc_pgp_verify_detached(const uint8_t *signature_data,
                                     size_t signature_length,
                                     const uint8_t *data, size_t length,
                                     const osvc_pgp_key_t *keys,
                                     size_t key_count,
                                     const osvc_hash_ops_t *ops, uint32_t now);

/*
 * Reads the signing keys from an armored OpenPGP public key, as a keyserver
 * serves it, into keys (at most capacity). Only the key whose primary key has
 * root_fingerprint counts; other keys in the text are passed over. A subkey
 * is a signing key when its newest binding signature by the primary key is
 * valid, has the signing flag and holds a valid signature by the subkey
 * itself, when the primary key has not revoked it, and when neither it nor
 * one of those two signatures has expired at now (seconds since 1970, or 0
 * when the clock is unknown: no expiry check then). Only Ed25519 keys are
 * read. The text is decoded in place and is not usable afterwards. ops must
 * serve SHA-1 for the fingerprints. Returns NULL with *count at least 1, or
 * why the text gives no signing key; osvc_pgp_error_key_revoked when the
 * primary key has revoked itself, which no other copy of the key can undo.
 */
extern const char osvc_pgp_error_key_revoked[];

const char *osvc_pgp_read_keyring(uint8_t *text, size_t length,
                                  const uint8_t root_fingerprint[OSVC_PGP_FINGERPRINT_LEN],
                                  const osvc_hash_ops_t *ops, uint32_t now,
                                  osvc_pgp_key_t *keys, size_t capacity,
                                  size_t *count);

/* The digest length of an accepted hash, or 0. */
size_t osvc_pgp_digest_length(osvc_pgp_hash_t algorithm);

#ifdef __cplusplus
}
#endif

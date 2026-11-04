/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* Written from RFC 4880 and RFC 9580; section numbers refer to RFC 9580. */

#include "os_services_pgp.h"

#include <string.h>

#include "monocypher-ed25519.h"

#define TAG_SIGNATURE 2U
#define TAG_PUBLIC_KEY 6U
#define TAG_PUBLIC_SUBKEY 14U

/* Signature subpackets (5.2.3.7). */
#define SUB_CREATION 2U
#define SUB_EXPIRATION 3U
#define SUB_KEY_EXPIRATION 9U
#define SUB_KEY_FLAGS 27U
#define SUB_EMBEDDED_SIGNATURE 32U
#define SUB_ISSUER_KEY_ID 16U
#define SUB_ISSUER_FINGERPRINT 33U
#define SUB_CRITICAL 0x80U

static uint32_t get_be16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | p[1];
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

size_t osvc_pgp_digest_length(osvc_pgp_hash_t algorithm)
{
    switch (algorithm) {
    case OSVC_PGP_HASH_SHA256:
        return 32U;
    case OSVC_PGP_HASH_SHA384:
        return 48U;
    case OSVC_PGP_HASH_SHA512:
        return 64U;
    default:
        return 0U;
    }
}

/*
 * One packet header (4.2): the tag and the body's bounds. Partial body
 * lengths and the old format's indeterminate length do not occur in a
 * detached signature and are refused.
 */
static bool next_packet(const uint8_t *data, size_t length, size_t *offset,
                        uint8_t *tag, const uint8_t **body, size_t *body_length)
{
    size_t i = *offset;
    if (i >= length) {
        return false;
    }
    const uint8_t ctb = data[i++];
    if ((ctb & 0x80U) == 0U) {
        return false;
    }
    size_t size;
    if ((ctb & 0x40U) != 0U) {
        *tag = ctb & 0x3fU;
        if (i >= length) {
            return false;
        }
        const uint8_t first = data[i++];
        if (first < 192U) {
            size = first;
        } else if (first < 224U) {
            if (i >= length) {
                return false;
            }
            size = ((size_t)(first - 192U) << 8) + data[i++] + 192U;
        } else if (first == 255U) {
            if (length - i < 4U) {
                return false;
            }
            size = get_be32(data + i);
            i += 4U;
        } else {
            return false;
        }
    } else {
        *tag = (ctb >> 2) & 0x0fU;
        const uint8_t kind = ctb & 0x03U;
        if (kind == 3U) {
            return false;
        }
        const size_t octets = kind == 0U ? 1U : kind == 1U ? 2U : 4U;
        if (length - i < octets) {
            return false;
        }
        size = 0U;
        for (size_t k = 0U; k < octets; ++k) {
            size = (size << 8) | data[i + k];
        }
        i += octets;
    }
    if (size > length - i) {
        return false;
    }
    *body = data + i;
    *body_length = size;
    *offset = i + size;
    return true;
}

/* Reads the subpackets of one area. Only the hashed area sets fields that
 * matter, and only it can make the signature invalid with a critical
 * subpacket this code does not know; the unhashed area only names the
 * issuer, which the Ed25519 check then confirms. */
static const char *read_subpackets(const uint8_t *area, size_t length,
                                   bool hashed, osvc_pgp_signature_t *sig,
                                   bool *has_creation)
{
    size_t i = 0U;
    while (i < length) {
        size_t size;
        const uint8_t first = area[i++];
        if (first < 192U) {
            size = first;
        } else if (first < 255U) {
            if (i >= length) {
                return "a malformed signature subpacket";
            }
            size = ((size_t)(first - 192U) << 8) + area[i++] + 192U;
        } else {
            if (length - i < 4U) {
                return "a malformed signature subpacket";
            }
            size = get_be32(area + i);
            i += 4U;
        }
        if (size == 0U || size > length - i) {
            return "a malformed signature subpacket";
        }
        const uint8_t type = area[i] & (uint8_t)~SUB_CRITICAL;
        const bool critical = (area[i] & SUB_CRITICAL) != 0U;
        const uint8_t *value = area + i + 1U;
        const size_t value_length = size - 1U;
        i += size;
        switch (type) {
        case SUB_CREATION:
            if (value_length != 4U) {
                return "a malformed signature creation time";
            }
            if (hashed) {
                sig->created = get_be32(value);
                *has_creation = true;
            }
            break;
        case SUB_EXPIRATION:
            if (value_length != 4U) {
                return "a malformed signature expiration time";
            }
            if (hashed) {
                sig->expires_after = get_be32(value);
            }
            break;
        case SUB_KEY_EXPIRATION:
            if (value_length != 4U) {
                return "a malformed key expiration time";
            }
            if (hashed) {
                sig->key_expires_after = get_be32(value);
            }
            break;
        case SUB_KEY_FLAGS:
            if (hashed && value_length >= 1U) {
                sig->key_flags = value[0];
            }
            break;
        case SUB_EMBEDDED_SIGNATURE:
            /* Checked on its own, so either area may hold it. */
            sig->embedded = value;
            sig->embedded_length = value_length;
            break;
        case SUB_ISSUER_KEY_ID:
            if (value_length != OSVC_PGP_KEY_ID_LEN) {
                return "a malformed issuer key ID";
            }
            if (hashed || !sig->has_issuer_key_id) {
                memcpy(sig->issuer_key_id, value, OSVC_PGP_KEY_ID_LEN);
                sig->has_issuer_key_id = true;
            }
            break;
        case SUB_ISSUER_FINGERPRINT:
            /* A version 4 key's fingerprint; a version 6 key's is longer
             * and can be no signing key here. */
            if (value_length == 1U + OSVC_PGP_FINGERPRINT_LEN && value[0] == 4U &&
                (hashed || !sig->has_issuer_fingerprint)) {
                memcpy(sig->issuer_fingerprint, value + 1,
                       OSVC_PGP_FINGERPRINT_LEN);
                sig->has_issuer_fingerprint = true;
            }
            break;
        default:
            if (hashed && critical) {
                return "an unknown critical signature subpacket";
            }
            break;
        }
    }
    return NULL;
}

/* An MPI (3.2) of at most 32 octets, as the native Ed25519 octets. */
static bool read_mpi32(const uint8_t *data, size_t length, size_t *offset,
                       uint8_t out[32])
{
    if (length - *offset < 2U) {
        return false;
    }
    const uint32_t bits = get_be16(data + *offset);
    const size_t octets = (bits + 7U) / 8U;
    *offset += 2U;
    if (bits > 256U || octets > length - *offset) {
        return false;
    }
    memset(out, 0, 32U);
    memcpy(out + 32U - octets, data + *offset, octets);
    *offset += octets;
    return true;
}

/* A version 4 signature packet body (5.2.3). */
static const char *parse_signature(const uint8_t *body, size_t length,
                                   osvc_pgp_signature_t *sig)
{
    memset(sig, 0, sizeof(*sig));
    if (length < 8U) {
        return "a truncated signature";
    }
    if (body[0] != 4U) {
        return "a signature version other than 4";
    }
    sig->type = body[1];
    sig->public_key_algorithm = body[2];
    sig->hash_algorithm = (osvc_pgp_hash_t)body[3];
    const size_t hashed_length = get_be16(body + 4);
    if (hashed_length > length - 6U - 2U) {
        return "a truncated signature";
    }
    sig->hashed = body;
    sig->hashed_length = 6U + hashed_length;
    const size_t unhashed_at = 6U + hashed_length;
    const size_t unhashed_length = get_be16(body + unhashed_at);
    if (unhashed_length > length - unhashed_at - 2U) {
        return "a truncated signature";
    }
    const size_t prefix_at = unhashed_at + 2U + unhashed_length;
    if (length - prefix_at < 2U) {
        return "a truncated signature";
    }
    if (sig->public_key_algorithm != OSVC_PGP_ALGO_EDDSA_LEGACY &&
        sig->public_key_algorithm != OSVC_PGP_ALGO_ED25519) {
        return "a signature algorithm other than Ed25519";
    }
    if (osvc_pgp_digest_length(sig->hash_algorithm) == 0U) {
        return "a hash algorithm other than SHA-256, SHA-384 or SHA-512";
    }
    bool has_creation = false;
    const char *error = read_subpackets(body + 6, hashed_length, true, sig,
                                        &has_creation);
    if (error == NULL) {
        error = read_subpackets(body + unhashed_at + 2U, unhashed_length, false,
                                sig, &has_creation);
    }
    if (error != NULL) {
        return error;
    }
    if (!has_creation) {
        return "a signature without a creation time";
    }
    memcpy(sig->hash_prefix, body + prefix_at, 2U);
    size_t offset = prefix_at + 2U;
    if (sig->public_key_algorithm == OSVC_PGP_ALGO_EDDSA_LEGACY) {
        if (!read_mpi32(body, length, &offset, sig->signature) ||
            !read_mpi32(body, length, &offset, sig->signature + 32) ||
            offset != length) {
            return "a malformed EdDSA signature";
        }
    } else {
        if (length - offset != 64U) {
            return "a malformed Ed25519 signature";
        }
        memcpy(sig->signature, body + offset, 64U);
    }
    return NULL;
}

static bool issued_by(const osvc_pgp_signature_t *sig, const osvc_pgp_key_t *key)
{
    if (sig->has_issuer_fingerprint) {
        return memcmp(sig->issuer_fingerprint, key->fingerprint,
                      OSVC_PGP_FINGERPRINT_LEN) == 0;
    }
    return sig->has_issuer_key_id &&
           memcmp(sig->issuer_key_id,
                  key->fingerprint + OSVC_PGP_FINGERPRINT_LEN -
                      OSVC_PGP_KEY_ID_LEN,
                  OSVC_PGP_KEY_ID_LEN) == 0;
}

const char *osvc_pgp_find_signature(const uint8_t *data, size_t length,
                                    const osvc_pgp_key_t *keys,
                                    size_t key_count,
                                    osvc_pgp_signature_t *signature,
                                    const osvc_pgp_key_t **key)
{
    *key = NULL;
    const char *error = "no signature packet";
    size_t offset = 0U;
    while (offset < length) {
        uint8_t tag;
        const uint8_t *body;
        size_t body_length;
        if (!next_packet(data, length, &offset, &tag, &body, &body_length)) {
            return "a malformed OpenPGP packet";
        }
        if (tag != TAG_SIGNATURE) {
            continue;
        }
        const char *parsed = parse_signature(body, body_length, signature);
        if (parsed != NULL) {
            error = parsed;
            continue;
        }
        if (!signature->has_issuer_fingerprint && !signature->has_issuer_key_id) {
            error = "a signature that names no issuer";
            continue;
        }
        for (size_t k = 0U; k < key_count; ++k) {
            if (issued_by(signature, &keys[k])) {
                *key = &keys[k];
                return NULL;
            }
        }
        error = "no signature by the JetKVM release key";
    }
    memset(signature, 0, sizeof(*signature));
    return error;
}

static bool hash_trailer(const osvc_pgp_signature_t *signature,
                         const osvc_hash_ops_t *ops, void *hash)
{
    const uint32_t n = (uint32_t)signature->hashed_length;
    const uint8_t final[6] = {
        4U, 0xffU, (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8),
        (uint8_t)n,
    };
    return ops->update(ops->ctx, hash, signature->hashed,
                       signature->hashed_length) &&
           ops->update(ops->ctx, hash, final, sizeof(final));
}

static const char *verify_digest(const osvc_pgp_signature_t *signature,
                                 const uint8_t *digest, size_t digest_length,
                                 const osvc_pgp_key_t *key, uint32_t now)
{
    if (digest_length != osvc_pgp_digest_length(signature->hash_algorithm)) {
        return "a digest of the wrong length";
    }
    if (memcmp(digest, signature->hash_prefix, 2U) != 0) {
        /* The data is not what was signed; the left 16 bits say so before
         * the equation does. */
        return "the signature does not match the data";
    }
    if (crypto_ed25519_check(signature->signature, key->public_key, digest,
                             digest_length) != 0) {
        return "the signature does not match the data";
    }
    if (signature->created < key->created) {
        return "a signature older than its key";
    }
    if (now != 0U && signature->expires_after != 0U &&
        (uint64_t)now >=
            (uint64_t)signature->created + signature->expires_after) {
        return "an expired signature";
    }
    if (now != 0U && key->expires != 0U && now >= key->expires) {
        return "a signature by an expired key";
    }
    return NULL;
}

const char *osvc_pgp_verify_hash(const osvc_pgp_signature_t *signature,
                                 uint8_t expected_type,
                                 const osvc_pgp_key_t *key,
                                 const osvc_hash_ops_t *ops, void *hash,
                                 uint32_t now)
{
    uint8_t digest[OSVC_PGP_DIGEST_MAX];
    size_t digest_length = 0U;
    if (!hash_trailer(signature, ops, hash)) {
        ops->abort(ops->ctx, hash);
        return "the signature's hash failed";
    }
    if (!ops->finish(ops->ctx, hash, digest, sizeof(digest), &digest_length)) {
        return "the signature's hash failed";
    }
    if (signature->type != expected_type) {
        return "a signature of another type";
    }
    return verify_digest(signature, digest, digest_length, key, now);
}

const char *osvc_pgp_verify_detached(const uint8_t *signature_data,
                                     size_t signature_length,
                                     const uint8_t *data, size_t length,
                                     const osvc_pgp_key_t *keys,
                                     size_t key_count,
                                     const osvc_hash_ops_t *ops, uint32_t now)
{
    osvc_pgp_signature_t signature;
    const osvc_pgp_key_t *key = NULL;
    const char *error = osvc_pgp_find_signature(
        signature_data, signature_length, keys, key_count, &signature, &key);
    if (error != NULL) {
        return error;
    }
    void *hash = ops->begin(ops->ctx, signature.hash_algorithm);
    if (hash == NULL) {
        return "the signature's hash is unavailable";
    }
    if (!ops->update(ops->ctx, hash, data, length)) {
        ops->abort(ops->ctx, hash);
        return "the signature's hash failed";
    }
    return osvc_pgp_verify_hash(&signature, OSVC_PGP_SIG_BINARY, key, ops,
                                hash, now);
}

/* --- Keys ---------------------------------------------------------------- */

/* The Ed25519 curve of the legacy EdDSA algorithm, 1.3.6.1.4.1.11591.15.1,
 * with its length octet (9.2). */
static const uint8_t OID_ED25519_LEGACY[] = {
    0x09U, 0x2bU, 0x06U, 0x01U, 0x04U, 0x01U, 0xdaU, 0x47U, 0x0fU, 0x01U,
};

/* A key packet's body as a signature and a fingerprint hash it (5.2.4). */
static bool hash_key(const osvc_hash_ops_t *ops, void *hash,
                     const uint8_t *body, size_t length)
{
    const uint8_t head[3] = {0x99U, (uint8_t)(length >> 8), (uint8_t)length};
    return ops->update(ops->ctx, hash, head, sizeof(head)) &&
           ops->update(ops->ctx, hash, body, length);
}

/* A version 4 Ed25519 public key or subkey packet body (5.5.2). */
static bool parse_public_key(const uint8_t *body, size_t length,
                             const osvc_hash_ops_t *ops, osvc_pgp_key_t *key)
{
    if (length < 6U || length > 0xffffU || body[0] != 4U) {
        return false;
    }
    key->created = get_be32(body + 1);
    if (body[5] == OSVC_PGP_ALGO_EDDSA_LEGACY) {
        /* The curve, then the point as an MPI of 263 bits: 0x40 and the
         * native key. */
        const size_t at = 6U + sizeof(OID_ED25519_LEGACY);
        if (length != at + 2U + 1U + OSVC_PGP_ED25519_KEY_LEN ||
            memcmp(body + 6, OID_ED25519_LEGACY, sizeof(OID_ED25519_LEGACY)) != 0 ||
            get_be16(body + at) != 263U || body[at + 2U] != 0x40U) {
            return false;
        }
        memcpy(key->public_key, body + at + 3U, OSVC_PGP_ED25519_KEY_LEN);
    } else if (body[5] == OSVC_PGP_ALGO_ED25519) {
        if (length != 6U + OSVC_PGP_ED25519_KEY_LEN) {
            return false;
        }
        memcpy(key->public_key, body + 6, OSVC_PGP_ED25519_KEY_LEN);
    } else {
        return false;
    }
    void *hash = ops->begin(ops->ctx, OSVC_PGP_HASH_SHA1);
    if (hash == NULL) {
        return false;
    }
    if (!hash_key(ops, hash, body, length)) {
        ops->abort(ops->ctx, hash);
        return false;
    }
    size_t digest_length = 0U;
    return ops->finish(ops->ctx, hash, key->fingerprint,
                       sizeof(key->fingerprint), &digest_length) &&
           digest_length == sizeof(key->fingerprint);
}

/* Whether signer made the signature, of the given type, over the primary
 * key and, for a signature about a subkey, the subkey. */
static bool key_signature_valid(const osvc_pgp_signature_t *signature,
                                uint8_t type, const osvc_pgp_key_t *signer,
                                const uint8_t *primary, size_t primary_length,
                                const uint8_t *subkey, size_t subkey_length,
                                const osvc_hash_ops_t *ops)
{
    void *hash = ops->begin(ops->ctx, signature->hash_algorithm);
    if (hash == NULL) {
        return false;
    }
    if (!hash_key(ops, hash, primary, primary_length) ||
        (subkey != NULL && !hash_key(ops, hash, subkey, subkey_length))) {
        ops->abort(ops->ctx, hash);
        return false;
    }
    return osvc_pgp_verify_hash(signature, type, signer, ops, hash, 0U) == NULL;
}

const char osvc_pgp_error_key_revoked[] = "the release key is revoked";

/* The subkey that the packets after it are about, and what its signatures
 * have said so far. */
typedef struct {
    bool present;
    const uint8_t *body;
    size_t length;
    osvc_pgp_key_t key;
    /* The newest valid binding signature. */
    bool bound;
    uint32_t bound_at;
    bool signs;
    /* When the subkey stops being a signing key, seconds since 1970, or 0
     * for never: the earliest of the key's expiry and the expiry of the
     * binding signature and of the subkey's own signature in it. */
    uint32_t expires;
    bool revoked;
} pgp_subkey_t;

/* The earlier of two times, where 0 is never. */
static uint32_t earlier(uint32_t a, uint64_t b)
{
    const uint32_t limited = b > UINT32_MAX ? UINT32_MAX : (uint32_t)b;
    return a == 0U ? limited : limited == 0U ? a : a < limited ? a : limited;
}

/* When a signature expires, or 0 for never. */
static uint64_t signature_expiry(const osvc_pgp_signature_t *signature)
{
    return signature->expires_after == 0U ? 0U :
           (uint64_t)signature->created + signature->expires_after;
}

/* Adds the subkey to keys when it is a signing key at now. */
static void keep_subkey(const pgp_subkey_t *subkey, uint32_t now,
                        osvc_pgp_key_t *keys, size_t capacity, size_t *count)
{
    const bool expired =
        now != 0U && subkey->expires != 0U && now >= subkey->expires;
    if (subkey->present && subkey->bound && subkey->signs && !subkey->revoked &&
        !expired && *count < capacity) {
        keys[*count] = subkey->key;
        keys[*count].expires = subkey->expires;
        (*count)++;
    }
}

/* One signature packet that follows a subkey of the release key. */
static void read_subkey_signature(const uint8_t *body, size_t length,
                                  const osvc_pgp_key_t *root,
                                  const uint8_t *root_body, size_t root_length,
                                  const osvc_hash_ops_t *ops,
                                  pgp_subkey_t *subkey)
{
    osvc_pgp_signature_t signature;
    if (parse_signature(body, length, &signature) != NULL ||
        !issued_by(&signature, root)) {
        return;
    }
    if (signature.type == OSVC_PGP_SIG_SUBKEY_REVOCATION) {
        if (key_signature_valid(&signature, OSVC_PGP_SIG_SUBKEY_REVOCATION, root,
                                root_body, root_length, subkey->body,
                                subkey->length, ops)) {
            subkey->revoked = true;
        }
        return;
    }
    if (signature.type != OSVC_PGP_SIG_SUBKEY_BINDING ||
        (subkey->bound && signature.created < subkey->bound_at) ||
        !key_signature_valid(&signature, OSVC_PGP_SIG_SUBKEY_BINDING, root,
                             root_body, root_length, subkey->body,
                             subkey->length, ops)) {
        return;
    }
    subkey->bound = true;
    subkey->bound_at = signature.created;
    subkey->expires = earlier(
        signature.key_expires_after == 0U ? 0U :
            earlier(0U, (uint64_t)subkey->key.created + signature.key_expires_after),
        signature_expiry(&signature));
    /* A signing subkey also signs the binding itself, so that nobody can
     * bind another owner's signing key to a key of their own. */
    osvc_pgp_signature_t back;
    subkey->signs =
        (signature.key_flags & OSVC_PGP_KEY_FLAG_SIGN) != 0U &&
        signature.embedded != NULL &&
        parse_signature(signature.embedded, signature.embedded_length, &back) == NULL &&
        key_signature_valid(&back, OSVC_PGP_SIG_PRIMARY_BINDING, &subkey->key,
                            root_body, root_length, subkey->body,
                            subkey->length, ops);
    if (subkey->signs) {
        subkey->expires = earlier(subkey->expires, signature_expiry(&back));
    }
}

/* Decodes the first armored public key block of text in place (6.2): the
 * base64 lines between the armor headers and the checksum line. Returns the
 * number of octets, or 0 without such a block. */
static size_t dearmor(uint8_t *text, size_t length)
{
    static const char BEGIN[] = "-----BEGIN PGP PUBLIC KEY BLOCK-----";
    const size_t begin_length = sizeof(BEGIN) - 1U;
    size_t i = 0U;
    while (i + begin_length <= length && memcmp(text + i, BEGIN, begin_length) != 0) {
        ++i;
    }
    if (i + begin_length > length) {
        return 0U;
    }
    i += begin_length;
    /* The armor headers end at the first empty line. */
    size_t line_length = 1U;
    for (; i < length; ++i) {
        if (text[i] == '\n') {
            if (line_length == 0U) {
                ++i;
                break;
            }
            line_length = 0U;
        } else if (text[i] != '\r') {
            line_length++;
        }
    }
    size_t out = 0U;
    uint32_t bits = 0U;
    unsigned held = 0U;
    for (; i < length; ++i) {
        const uint8_t c = text[i];
        uint32_t value;
        if (c >= 'A' && c <= 'Z') {
            value = (uint32_t)(c - 'A');
        } else if (c >= 'a' && c <= 'z') {
            value = (uint32_t)(c - 'a') + 26U;
        } else if (c >= '0' && c <= '9') {
            value = (uint32_t)(c - '0') + 52U;
        } else if (c == '+') {
            value = 62U;
        } else if (c == '/') {
            value = 63U;
        } else if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            continue;
        } else {
            /* Padding, the checksum line or the end line. */
            break;
        }
        bits = ((bits << 6) | value) & 0xffffU;
        held += 6U;
        if (held >= 8U) {
            held -= 8U;
            text[out++] = (uint8_t)(bits >> held);
        }
    }
    return out;
}

const char *osvc_pgp_read_keyring(uint8_t *text, size_t length,
                                  const uint8_t root_fingerprint[OSVC_PGP_FINGERPRINT_LEN],
                                  const osvc_hash_ops_t *ops, uint32_t now,
                                  osvc_pgp_key_t *keys, size_t capacity,
                                  size_t *count)
{
    *count = 0U;
    length = dearmor(text, length);
    if (length == 0U) {
        return "not an armored OpenPGP public key";
    }
    /* The release key's primary key, while the packets are its own. */
    osvc_pgp_key_t root = {0};
    const uint8_t *root_body = NULL;
    size_t root_length = 0U;
    bool found = false;
    bool own = false;
    /* A signature directly after the primary key is about that key. */
    bool direct = false;
    pgp_subkey_t subkey = {0};
    size_t offset = 0U;
    while (offset < length) {
        uint8_t tag;
        const uint8_t *body;
        size_t body_length;
        if (!next_packet(text, length, &offset, &tag, &body, &body_length)) {
            return "a malformed OpenPGP packet";
        }
        if (tag == TAG_PUBLIC_KEY) {
            keep_subkey(&subkey, now, keys, capacity, count);
            subkey.present = false;
            osvc_pgp_key_t primary;
            own = !found && parse_public_key(body, body_length, ops, &primary) &&
                  memcmp(primary.fingerprint, root_fingerprint,
                         OSVC_PGP_FINGERPRINT_LEN) == 0;
            if (own) {
                root = primary;
                root_body = body;
                root_length = body_length;
                found = true;
            }
            direct = own;
        } else if (!own) {
            continue;
        } else if (tag == TAG_PUBLIC_SUBKEY) {
            keep_subkey(&subkey, now, keys, capacity, count);
            memset(&subkey, 0, sizeof(subkey));
            subkey.present = parse_public_key(body, body_length, ops, &subkey.key);
            subkey.body = body;
            subkey.length = body_length;
            direct = false;
        } else if (tag != TAG_SIGNATURE) {
            /* A user ID: the signatures after it certify the name. */
            keep_subkey(&subkey, now, keys, capacity, count);
            subkey.present = false;
            direct = false;
        } else if (subkey.present) {
            read_subkey_signature(body, body_length, &root, root_body,
                                  root_length, ops, &subkey);
        } else if (direct) {
            osvc_pgp_signature_t signature;
            if (parse_signature(body, body_length, &signature) == NULL &&
                signature.type == OSVC_PGP_SIG_KEY_REVOCATION &&
                issued_by(&signature, &root) &&
                key_signature_valid(&signature, OSVC_PGP_SIG_KEY_REVOCATION,
                                    &root, root_body, root_length, NULL, 0U,
                                    ops)) {
                *count = 0U;
                return osvc_pgp_error_key_revoked;
            }
        }
    }
    keep_subkey(&subkey, now, keys, capacity, count);
    if (!found) {
        return "fetched key fingerprint does not match expected fingerprint";
    }
    if (*count == 0U) {
        return "the release key has no valid signing subkey";
    }
    return NULL;
}

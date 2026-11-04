/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#include <string.h>
#include <strings.h>

#include "mbedtls/md.h"
#include "media_lib_os.h"
#include "dtls_fingerprint.h"

#define FINGERPRINT_PEERS 4
#define FINGERPRINTS      4
#define DIGEST_MAX        64

typedef struct {
    mbedtls_md_type_t type;
    size_t            size;
    unsigned char     digest[DIGEST_MAX];
} fingerprint_t;

typedef struct {
    const void   *peer;
    size_t        count;
    fingerprint_t fingerprints[FINGERPRINTS];
} expectation_t;

static const struct {
    const char       *name;
    mbedtls_md_type_t type;
    size_t            size;
} s_algorithms[] = {
    {"sha-256", MBEDTLS_MD_SHA256, 32},
    {"sha-384", MBEDTLS_MD_SHA384, 48},
    {"sha-512", MBEDTLS_MD_SHA512, 64},
};

static expectation_t s_expected[FINGERPRINT_PEERS];
static media_lib_mutex_handle_t s_lock;

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "<algorithm> <XX:XX:...>", the value of one a=fingerprint line. */
static bool parse_fingerprint(const char *value, const char *end, fingerprint_t *fingerprint)
{
    const char *space = memchr(value, ' ', (size_t)(end - value));
    if (!space) return false;
    size_t name = (size_t)(space - value);
    fingerprint->size = 0;
    for (size_t i = 0; i < sizeof(s_algorithms) / sizeof(s_algorithms[0]); ++i) {
        if (strlen(s_algorithms[i].name) == name && strncasecmp(value, s_algorithms[i].name, name) == 0) {
            fingerprint->type = s_algorithms[i].type;
            fingerprint->size = s_algorithms[i].size;
        }
    }
    if (!fingerprint->size) return false;
    size_t count = 0;
    for (const char *p = space + 1;; p += 3) {
        if (end - p < 2 || count == fingerprint->size) return false;
        int high = hex_digit(p[0]), low = hex_digit(p[1]);
        if (high < 0 || low < 0) return false;
        fingerprint->digest[count++] = (unsigned char)(high << 4 | low);
        if (p + 2 == end) return count == fingerprint->size;
        if (p[2] != ':') return false;
    }
}

bool dtls_fingerprint_expect(const void *peer, const char *sdp, size_t size)
{
    static const char prefix[] = "a=fingerprint:";
    expectation_t found = {.peer = peer};
    for (const char *line = sdp, *end = sdp ? sdp + size : NULL; line && line < end;) {
        const char *next = memchr(line, '\n', (size_t)(end - line));
        const char *value_end = next ? next : end;
        if (value_end > line && value_end[-1] == '\r') --value_end;
        if ((size_t)(value_end - line) > sizeof(prefix) - 1 && memcmp(line, prefix, sizeof(prefix) - 1) == 0 &&
            found.count < FINGERPRINTS &&
            parse_fingerprint(line + sizeof(prefix) - 1, value_end, &found.fingerprints[found.count])) {
            ++found.count;
        }
        line = next ? next + 1 : NULL;
    }
    if (!peer || (!s_lock && media_lib_mutex_create(&s_lock) != 0)) return false;
    media_lib_mutex_lock(s_lock, MEDIA_LIB_MAX_LOCK_TIME);
    expectation_t *slot = NULL;
    for (size_t i = 0; i < FINGERPRINT_PEERS; ++i) {
        if (s_expected[i].peer == peer || (!slot && !s_expected[i].peer)) slot = &s_expected[i];
        if (s_expected[i].peer == peer) break;
    }
    if (slot) *slot = found.count ? found : (expectation_t){0};
    media_lib_mutex_unlock(s_lock);
    return slot && found.count;
}

bool dtls_fingerprint_matches(const void *peer, const unsigned char *der, size_t size)
{
    if (!peer || !der || !s_lock) return false;
    bool match = false;
    media_lib_mutex_lock(s_lock, MEDIA_LIB_MAX_LOCK_TIME);
    for (size_t i = 0; i < FINGERPRINT_PEERS; ++i) {
        if (s_expected[i].peer != peer) continue;
        for (size_t j = 0; j < s_expected[i].count; ++j) {
            const fingerprint_t *fingerprint = &s_expected[i].fingerprints[j];
            const mbedtls_md_info_t *md = mbedtls_md_info_from_type(fingerprint->type);
            unsigned char digest[DIGEST_MAX];
            if (!md || mbedtls_md_get_size(md) != fingerprint->size || mbedtls_md(md, der, size, digest) != 0) continue;
            unsigned char difference = 0;
            for (size_t k = 0; k < fingerprint->size; ++k) difference |= digest[k] ^ fingerprint->digest[k];
            match |= difference == 0;
        }
    }
    media_lib_mutex_unlock(s_lock);
    return match;
}

void dtls_fingerprint_forget(const void *peer)
{
    if (!peer || !s_lock) return;
    media_lib_mutex_lock(s_lock, MEDIA_LIB_MAX_LOCK_TIME);
    for (size_t i = 0; i < FINGERPRINT_PEERS; ++i) {
        if (s_expected[i].peer == peer) s_expected[i] = (expectation_t){0};
    }
    media_lib_mutex_unlock(s_lock);
}

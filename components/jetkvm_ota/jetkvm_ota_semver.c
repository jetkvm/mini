/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* Semantic versions as the OTA compares them; also used by the OS Services
 * update relay. Pure C, no ESP-IDF dependencies. */

#include "jetkvm_ota_semver.h"

#include <string.h>


static bool semver_parse_number(const char **cursor, unsigned long long *value)
{
    const char *p = *cursor;
    if (*p < '0' || *p > '9') {
        return false;
    }
    unsigned long long parsed = 0ULL;
    while (*p >= '0' && *p <= '9') {
        if (parsed > (~0ULL - 9ULL) / 10ULL) {
            return false;
        }
        parsed = parsed * 10ULL + (unsigned long long)(*p - '0');
        p++;
    }
    *cursor = p;
    *value = parsed;
    return true;
}

bool jetkvm_ota_semver_parse(const char *text, jetkvm_ota_semver_t *out)
{
    memset(out, 0, sizeof(*out));
    if (*text == 'v' || *text == 'V') {
        text++;
    }
    const char *p = text;
    for (int i = 0; i < 3; i++) {
        if (!semver_parse_number(&p, &out->core[i])) {
            return false;
        }
        if (i == 2 || *p != '.') {
            break;
        }
        p++;
    }
    if (*p == '-') {
        p++;
        out->prerelease = p;
        size_t identifier_len = 0;
        while (*p && *p != '+') {
            char c = *p;
            if (c == '.') {
                if (identifier_len == 0) {
                    return false;
                }
                identifier_len = 0;
            } else if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                       (c >= 'a' && c <= 'z') || c == '-') {
                identifier_len++;
            } else {
                return false;
            }
            p++;
        }
        if (identifier_len == 0) {
            return false;
        }
        out->prerelease_len = (size_t)(p - out->prerelease);
    }
    if (*p == '+') {
        /* build metadata never participates in precedence, but a malformed
         * suffix still fails the parse, like Masterminds semver */
        p++;
        size_t identifier_len = 0;
        while (*p) {
            char c = *p;
            if (c == '.') {
                if (identifier_len == 0) {
                    return false;
                }
                identifier_len = 0;
            } else if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                       (c >= 'a' && c <= 'z') || c == '-') {
                identifier_len++;
            } else {
                return false;
            }
            p++;
        }
        return identifier_len != 0;
    }
    return *p == '\0';
}

static int semver_compare_identifiers(const char *a, size_t a_len,
                                      const char *b, size_t b_len)
{
    bool a_numeric = true;
    bool b_numeric = true;
    for (size_t i = 0; i < a_len; i++) {
        a_numeric = a_numeric && a[i] >= '0' && a[i] <= '9';
    }
    for (size_t i = 0; i < b_len; i++) {
        b_numeric = b_numeric && b[i] >= '0' && b[i] <= '9';
    }
    if (a_numeric != b_numeric) {
        return a_numeric ? -1 : 1; /* numeric orders below alphanumeric */
    }
    if (a_numeric) {
        /* compare digit strings without overflow: strip leading zeros,
         * then longer means greater */
        while (a_len > 1U && a[0] == '0') { a++; a_len--; }
        while (b_len > 1U && b[0] == '0') { b++; b_len--; }
        if (a_len != b_len) {
            return a_len < b_len ? -1 : 1;
        }
    }
    size_t common = a_len < b_len ? a_len : b_len;
    int order = memcmp(a, b, common);
    if (order != 0) {
        return order < 0 ? -1 : 1;
    }
    if (a_len != b_len) {
        return a_len < b_len ? -1 : 1;
    }
    return 0;
}

int jetkvm_ota_semver_compare(const jetkvm_ota_semver_t *a,
                              const jetkvm_ota_semver_t *b)
{
    for (int i = 0; i < 3; i++) {
        if (a->core[i] != b->core[i]) {
            return a->core[i] < b->core[i] ? -1 : 1;
        }
    }
    if (a->prerelease_len == 0 || b->prerelease_len == 0) {
        /* a release outranks its own prereleases */
        if (a->prerelease_len == b->prerelease_len) {
            return 0;
        }
        return a->prerelease_len != 0 ? -1 : 1;
    }
    const char *pa = a->prerelease;
    const char *ea = a->prerelease + a->prerelease_len;
    const char *pb = b->prerelease;
    const char *eb = b->prerelease + b->prerelease_len;
    for (;;) {
        if (pa >= ea || pb >= eb) {
            /* the longer identifier set orders higher */
            if (pa >= ea && pb >= eb) {
                return 0;
            }
            return pa >= ea ? -1 : 1;
        }
        const char *na = pa;
        while (na < ea && *na != '.') {
            na++;
        }
        const char *nb = pb;
        while (nb < eb && *nb != '.') {
            nb++;
        }
        int order = semver_compare_identifiers(pa, (size_t)(na - pa),
                                               pb, (size_t)(nb - pb));
        if (order != 0) {
            return order;
        }
        pa = na < ea ? na + 1 : ea;
        pb = nb < eb ? nb + 1 : eb;
    }
}

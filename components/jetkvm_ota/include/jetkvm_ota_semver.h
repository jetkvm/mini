/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Version handling mirrors Go's Masterminds semver: an optional leading
 * "v", one to three numeric core fields, "-prerelease", and "+metadata"
 * that never participates in precedence. Pure C, so the OS Services update
 * relay and the host tests use it as the OTA does.
 */
typedef struct {
    unsigned long long core[3];
    const char *prerelease; /* points into the parsed text; not terminated */
    size_t prerelease_len;  /* 0 when absent */
} jetkvm_ota_semver_t;

/* Parses text, which must outlive *out (prerelease points into it). */
bool jetkvm_ota_semver_parse(const char *text, jetkvm_ota_semver_t *out);

/* semver.org precedence: negative, zero or positive. */
int jetkvm_ota_semver_compare(const jetkvm_ota_semver_t *a,
                              const jetkvm_ota_semver_t *b);

#ifdef __cplusplus
}
#endif

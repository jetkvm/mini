/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <string.h>

/* CMake sets JETKVM_MOCK_CLOUD only for bench builds that test against the
 * e2e mock cloud (see JETKVM_MOCK_CLOUD_JWKS_URL in the top-level
 * CMakeLists.txt). */
#ifndef JETKVM_MOCK_CLOUD
#define JETKVM_MOCK_CLOUD 0
#endif

/* Registration and deregistration send the device's tokens, so they use
 * HTTPS; the mock cloud serves plain HTTP. */
static inline bool cloud_registration_url_allowed(const char *url)
{
    return strncmp(url, "https://", 8U) == 0 ||
           (JETKVM_MOCK_CLOUD && strncmp(url, "http://", 7U) == 0);
}

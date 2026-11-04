/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* The device's hashes for os_services_pgp.h: PSA's SHA-1, SHA-256, SHA-384
 * and SHA-512. A running hash is allocated in PSRAM. The OS Services update
 * relay and the firmware update use them. */

#pragma once

#include "os_services_pgp.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const osvc_hash_ops_t osvc_psa_hash_ops;

#ifdef __cplusplus
}
#endif

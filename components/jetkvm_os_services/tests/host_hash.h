/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* OpenSSL's hashes behind osvc_hash_ops_t, as PSA serves them on the
 * device. Counts the hashes open, so the tests see every one ended. */

#pragma once

#include "os_services_pgp.h"

extern const osvc_hash_ops_t host_hash_ops;
extern int host_hashes_open;

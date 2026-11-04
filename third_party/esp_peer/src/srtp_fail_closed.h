/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stddef.h>

/* libsrtp uses zero for success.  A zero result is also the fail-closed
 * sentinel rejected by the peer egress guards. */
int esp_peer_srtp_output_bytes(int status, size_t output_bytes);

/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The KVM's OS Services identity: the Ed25519 key that signs each
 * session.ready. Its seed lives in NVS, which a factory reset erases; the
 * sender's user approves the key by its fingerprint.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "os_services_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the key, or makes and saves one. It writes flash, so the caller's
 * stack must be in internal RAM. */
esp_err_t osvc_identity_load(void);

bool osvc_identity_public_key(uint8_t key[OSVC_PUBLIC_KEY_LEN]);
bool osvc_identity_sign(const uint8_t *message, size_t length,
                        uint8_t signature[OSVC_SIGNATURE_LEN]);
/* "AA:BB:...": the first 8 bytes of the SHA-256 of the public key, as the
 * target's approval dialog shows it; empty until the key is loaded. */
void osvc_identity_fingerprint(char out[OSVC_FINGERPRINT_TEXT_CAPACITY]);

#ifdef __cplusplus
}
#endif

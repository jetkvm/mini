#pragma once
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/sha.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define SHA2_256 2
/* Emulate only the hardware compression/state registers. PBKDF2, HMAC,
 * padding, batching and XOR accumulation run unchanged from production. */
extern SHA256_CTX test_sha_state;
extern bool test_sha_locked;
extern unsigned test_sha_blocks, test_sha_max_blocks;
static inline void esp_sha_acquire_hardware(void) {
    assert(!test_sha_locked); test_sha_locked = true; test_sha_blocks = 0;
}
static inline void esp_sha_release_hardware(void) {
    assert(test_sha_locked); test_sha_locked = false;
    if (test_sha_blocks > test_sha_max_blocks) test_sha_max_blocks = test_sha_blocks;
    /* Another SHA/AES user is allowed to destroy all hardware state. */
    memset(&test_sha_state, 0xa5, sizeof(test_sha_state));
}
static inline void esp_sha_set_mode(int mode) { assert(test_sha_locked && mode == SHA2_256); }
static inline void esp_sha_block(int mode, const void *block, bool first) {
    assert(test_sha_locked && mode == SHA2_256 && (uintptr_t)block % 4 == 0);
    if (first) assert(SHA256_Init(&test_sha_state));
    SHA256_Transform(&test_sha_state, block); ++test_sha_blocks;
}
static inline void esp_sha_read_digest_state(int mode, void *state) {
    assert(test_sha_locked && mode == SHA2_256 && (uintptr_t)state % 4 == 0);
    uint8_t *out = state;
    for (unsigned i = 0; i < 8; ++i) for (unsigned j = 0; j < 4; ++j)
        out[4*i+j] = (uint8_t)(test_sha_state.h[i] >> (24-8*j));
}
static inline void esp_sha_write_digest_state(int mode, void *state) {
    assert(test_sha_locked && mode == SHA2_256 && (uintptr_t)state % 4 == 0);
    const uint8_t *in = state;
    for (unsigned i = 0; i < 8; ++i) {
        test_sha_state.h[i] = 0;
        for (unsigned j = 0; j < 4; ++j) test_sha_state.h[i] = (test_sha_state.h[i] << 8) | in[4*i+j];
    }
}

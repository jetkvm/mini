#include "password_pbkdf2.h"
#include <string.h>
#include "sha/sha_core.h"
/* Only long HMAC keys need one general-purpose software hash. The hot loop
 * uses the P4 SHA engine's block/state API, without per-round DMA or PSA setup. */
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/sha256.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#if !CONFIG_IDF_TARGET_ESP32P4 || !CONFIG_MBEDTLS_HARDWARE_SHA
#error "Password block acceleration requires ESP32-P4 hardware SHA"
#endif
#endif

/* SHA and AES share the IDF hardware lock. Release it after at most 32 HMACs
 * so TLS and video authentication can proceed between short batches. */
#define HMAC_BATCH_SIZE 32U
#define YIELD_INTERVAL 4096U

static void wipe(void *p, size_t size)
{
    volatile uint8_t *bytes = p;
    while (size--) *bytes++ = 0;
}

static void hmac_block(uint32_t inner_seed[8], uint32_t outer_seed[8],
                       uint32_t message[16], uint32_t outer[16], uint32_t digest[8])
{
    /* Both blocks already contain SHA padding and their total bit lengths.
     * read_digest waits for completion before any state or block is replaced. */
    esp_sha_write_digest_state(SHA2_256, inner_seed);
    esp_sha_block(SHA2_256, message, false);
    esp_sha_read_digest_state(SHA2_256, outer);
    esp_sha_write_digest_state(SHA2_256, outer_seed);
    esp_sha_block(SHA2_256, outer, false);
    esp_sha_read_digest_state(SHA2_256, digest);
}

bool jetkvm_password_pbkdf2(const char *password, const uint8_t salt[16],
    uint32_t iterations, uint8_t output[32], void (*yield)(void))
{
    if (!password || !salt || !output || !iterations) return false;
    /* The register API reads/writes uint32_t words. Keep every hardware
     * buffer aligned, even when the caller's output is an unaligned record. */
    uint32_t key[16] = {0}, pad[16], inner_seed[8], outer_seed[8];
    uint32_t message[16] = {0}, outer[16] = {0}, digest[8], result[8];
    uint8_t *key_bytes = (uint8_t *)key, *pad_bytes = (uint8_t *)pad;
    uint8_t *message_bytes = (uint8_t *)message, *outer_bytes = (uint8_t *)outer;
    size_t length = strlen(password);
    if (length > sizeof(key)) {
        mbedtls_sha256_context hash;
        mbedtls_sha256_init(&hash);
        int error = mbedtls_sha256_starts(&hash, 0);
        if (!error) error = mbedtls_sha256_update(&hash, (const uint8_t *)password, length);
        if (!error) error = mbedtls_sha256_finish(&hash, key_bytes);
        mbedtls_sha256_free(&hash);
        if (error) { wipe(key, sizeof(key)); wipe(output, 32); return false; }
    } else memcpy(key, password, length);

    esp_sha_acquire_hardware();
    esp_sha_set_mode(SHA2_256);
    for (size_t i = 0; i < sizeof(pad); ++i) pad_bytes[i] = key_bytes[i] ^ 0x36;
    esp_sha_block(SHA2_256, pad, true);
    esp_sha_read_digest_state(SHA2_256, inner_seed);
    for (size_t i = 0; i < sizeof(pad); ++i) pad_bytes[i] = key_bytes[i] ^ 0x5c;
    esp_sha_block(SHA2_256, pad, true);
    esp_sha_read_digest_state(SHA2_256, outer_seed);
    /* U1 hashes salt || INT32_BE(1), following the 64-byte inner key pad. */
    memcpy(message_bytes, salt, 16);
    message_bytes[19] = 1;
    message_bytes[20] = 0x80;
    message_bytes[62] = 0x02; message_bytes[63] = 0xa0; /* (64 + 20) * 8 */
    outer_bytes[32] = 0x80;
    outer_bytes[62] = 0x03; /* (64 + 32) * 8 = 0x300 */
    hmac_block(inner_seed, outer_seed, message, outer, digest);
    memcpy(result, digest, sizeof(result));
    esp_sha_release_hardware();

    memset(message, 0, sizeof(message));
    message_bytes[32] = 0x80;
    message_bytes[62] = 0x03;
    for (uint32_t i = 1; i < iterations;) {
        uint32_t count = iterations - i;
        if (count > HMAC_BATCH_SIZE) count = HMAC_BATCH_SIZE;
        /* End a batch exactly at a scheduling boundary. */
        uint32_t until_yield = YIELD_INTERVAL - i % YIELD_INTERVAL;
        if (count > until_yield) count = until_yield;
        esp_sha_acquire_hardware();
        esp_sha_set_mode(SHA2_256);
        for (uint32_t n = 0; n < count; ++n, ++i) {
            memcpy(message, digest, sizeof(digest));
            hmac_block(inner_seed, outer_seed, message, outer, digest);
            for (size_t j = 0; j < 8; ++j) result[j] ^= digest[j];
        }
        esp_sha_release_hardware();
        if (yield && i < iterations && i % YIELD_INTERVAL == 0) yield();
    }
    memcpy(output, result, sizeof(result));
    wipe(key, sizeof(key)); wipe(pad, sizeof(pad));
    wipe(inner_seed, sizeof(inner_seed)); wipe(outer_seed, sizeof(outer_seed));
    wipe(message, sizeof(message)); wipe(outer, sizeof(outer));
    wipe(digest, sizeof(digest)); wipe(result, sizeof(result));
    return true;
}

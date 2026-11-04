#!/usr/bin/env python3
"""Exercise the production AES-ICM backend against OpenSSL with sanitizers."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MANAGED = ROOT / "managed_components/espressif__esp_libsrtp"
SOURCE = Path("libsrtp/crypto/cipher/aes_icm_mbedtls.c")
BASE_PATCH = ROOT / "patches/esp_libsrtp-1.0.0-mbedtls-aes-icm.patch"
PATCH = ROOT / "patches/esp_libsrtp-1.0.0-aes-ctr-block-xor.patch"


def run(*args: str, cwd: Path) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(args, cwd=cwd, text=True, capture_output=True)
    if result.returncode:
        raise AssertionError(
            f"command failed ({result.returncode}): {' '.join(args)}\n"
            f"{result.stdout}\n{result.stderr}"
        )
    return result


ESP_AES_H = r"""
#pragma once
#include <stddef.h>
#include <stdint.h>
#define ESP_AES_ENCRYPT 1
typedef struct {
    uint8_t key[32];
    unsigned key_bytes;
    unsigned key_in_hardware;
} esp_aes_context;
void esp_aes_init(esp_aes_context *ctx);
void esp_aes_free(esp_aes_context *ctx);
int esp_aes_setkey(esp_aes_context *ctx, const uint8_t *key, unsigned bits);
void esp_aes_acquire_hardware(void);
void esp_aes_release_hardware(void);
"""

AES_HAL_H = r"""
#pragma once
#include <stddef.h>
#include <stdint.h>
unsigned aes_hal_setkey(const uint8_t *key, unsigned bytes, int mode);
void aes_hal_transform_block(const uint8_t input[16], uint8_t output[16]);
"""

HARNESS = r"""
#include <assert.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/aes.h>
#include <openssl/modes.h>
#include "aes_icm_ext.h"
#include "err.h"
#include "cipher_types.h"
#include "hal/aes_hal.h"

extern const srtp_cipher_type_t srtp_aes_icm_192;

static pthread_mutex_t hardware_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int owns_hardware;
static AES_KEY hardware_key;
static size_t allocations, frees, acquires, releases, transforms;
static int fail_setkey;

void *srtp_crypto_alloc(size_t size) { ++allocations; return calloc(1, size); }
void srtp_crypto_free(void *ptr) { ++frees; free(ptr); }
void octet_string_set_to_zero(void *ptr, size_t size) {
    volatile uint8_t *p = ptr;
    while (size--) *p++ = 0;
}
void v128_copy_octet_string(v128_t *dst, const uint8_t src[16]) {
    memcpy(dst, src, 16);
}
char *v128_hex_string(v128_t *x) { (void)x; return ""; }
char *srtp_octet_string_hex_string(const void *p, size_t n) {
    (void)p; (void)n; return "";
}
void srtp_err_report(srtp_err_reporting_level_t level, const char *format, ...) {
    (void)level; (void)format;
}
void esp_aes_init(esp_aes_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }
void esp_aes_free(esp_aes_context *ctx) { memset(ctx, 0, sizeof(*ctx)); }
int esp_aes_setkey(esp_aes_context *ctx, const uint8_t *key, unsigned bits) {
    assert(bits == 128 || bits == 192 || bits == 256);
    ctx->key_bytes = bits / 8;
    memcpy(ctx->key, key, ctx->key_bytes);
    return 0;
}
void esp_aes_acquire_hardware(void) {
    assert(!owns_hardware);
    assert(pthread_mutex_lock(&hardware_mutex) == 0);
    owns_hardware = 1;
    ++acquires;
}
void esp_aes_release_hardware(void) {
    assert(owns_hardware);
    ++releases;
    owns_hardware = 0;
    assert(pthread_mutex_unlock(&hardware_mutex) == 0);
}
unsigned aes_hal_setkey(const uint8_t *key, unsigned bytes, int mode) {
    assert(owns_hardware && mode == ESP_AES_ENCRYPT);
    if (fail_setkey) { fail_setkey = 0; return 0; }
    assert(AES_set_encrypt_key(key, bytes * 8, &hardware_key) == 0);
    return bytes;
}
void aes_hal_transform_block(const uint8_t input[16], uint8_t output[16]) {
    assert(owns_hardware);
    ++transforms;
    AES_encrypt(input, output, &hardware_key);
}
static void reference_block(const unsigned char input[16],
                            unsigned char output[16], const void *key) {
    AES_encrypt(input, output, key);
}

typedef struct {
    AES_KEY key;
    uint8_t counter[16], stream[16];
    unsigned offset;
} reference_t;

static void reset(srtp_cipher_t *cipher, const uint8_t *key,
                  uint8_t iv[16], reference_t *ref) {
    srtp_aes_icm_ctx_t *ctx = cipher->state;
    assert(cipher->type->init(ctx, key) == srtp_err_status_ok);
    assert(cipher->type->set_iv(ctx, iv, srtp_direction_encrypt) ==
           srtp_err_status_ok);
    assert(AES_set_encrypt_key(key, ctx->key_size * 8, &ref->key) == 0);
    for (size_t i = 0; i < 16; ++i)
        ref->counter[i] = iv[i] ^ (i < 14 ? key[ctx->key_size + i] : 0);
    memset(ref->stream, 0, 16);
    ref->offset = 0;
}

static void checked_update(srtp_cipher_t *cipher, reference_t *ref,
                           size_t length, unsigned src_offset,
                           unsigned dst_offset, int inplace) {
    /* Payload ends at the allocation boundary, exposing tail overreads and
     * overwrites to ASan; prefix sentinels catch writes before the payload. */
    size_t src_size = src_offset + length;
    size_t dst_size = dst_offset + length;
    uint8_t *src_base = malloc(src_size ? src_size : 1);
    uint8_t *dst_base = inplace ? src_base : malloc(dst_size ? dst_size : 1);
    uint8_t *expected = malloc(length ? length : 1);
    assert(src_base && dst_base && expected);
    memset(src_base, 0xa5, src_size);
    if (!inplace) memset(dst_base, 0xa5, dst_size);
    uint8_t *src = src_base + src_offset;
    uint8_t *dst = dst_base + (inplace ? src_offset : dst_offset);
    for (size_t i = 0; i < length; ++i) src[i] = (i * 37 + length * 13) & 255;
    CRYPTO_ctr128_encrypt(src, expected, length, &ref->key, ref->counter,
                         ref->stream, &ref->offset, reference_block);
    size_t output_size = length;
    size_t allocation_count = allocations;
    assert(cipher->type->encrypt(cipher->state, src, length, dst,
                                  &output_size) == srtp_err_status_ok);
    assert(allocations == allocation_count && output_size == length);
    assert(memcmp(dst, expected, length) == 0);
    srtp_aes_icm_ctx_t *ctx = cipher->state;
    assert(ctx->nc_off == ref->offset);
    assert(memcmp(ctx->counter.v8, ref->counter, 16) == 0);
    if (length != 0)
        assert(memcmp(ctx->stream_block.v8, ref->stream, 16) == 0);
    for (unsigned i = 0; i < src_offset; ++i) assert(src_base[i] == 0xa5);
    if (!inplace) {
        for (unsigned i = 0; i < dst_offset; ++i) assert(dst_base[i] == 0xa5);
        for (size_t i = 0; i < length; ++i)
            assert(src[i] == ((i * 37 + length * 13) & 255));
        free(dst_base);
    }
    free(src_base);
    free(expected);
}

static void known_answer(const srtp_cipher_type_t *type) {
    const srtp_cipher_test_case_t *test = type->test_data;
    srtp_cipher_t *cipher = NULL;
    assert(type->alloc(&cipher, test->key_length_octets, 0) == srtp_err_status_ok);
    assert(type->init(cipher->state, test->key) == srtp_err_status_ok);
    assert(type->set_iv(cipher->state, test->idx, srtp_direction_encrypt) ==
           srtp_err_status_ok);
    uint8_t *output = malloc(test->ciphertext_length_octets);
    size_t length = test->ciphertext_length_octets;
    assert(type->encrypt(cipher->state, test->plaintext,
                         test->plaintext_length_octets, output, &length) ==
           srtp_err_status_ok);
    assert(length == test->ciphertext_length_octets);
    assert(memcmp(output, test->ciphertext, length) == 0);
    /* CTR decrypt is the same production transform. */
    assert(type->set_iv(cipher->state, test->idx, srtp_direction_decrypt) ==
           srtp_err_status_ok);
    assert(type->decrypt(cipher->state, output, length, output, &length) ==
           srtp_err_status_ok);
    assert(memcmp(output, test->plaintext, length) == 0);
    free(output);
    assert(type->dealloc(cipher) == srtp_err_status_ok);
}

static void exercise(const srtp_cipher_type_t *type, unsigned key_bytes) {
    static const size_t lengths[] = {0, 1, 2, 3, 4, 7, 8, 12, 15, 16, 17,
        19, 31, 32, 33, 47, 48, 63, 64, 65, 127, 128, 129, 255, 256, 257,
        1188, 1200, 1300, 1500, 4097};
    uint8_t key[46], iv[16];
    for (size_t i = 0; i < sizeof(key); ++i) key[i] = i * 11 + key_bytes;
    for (size_t i = 0; i < sizeof(iv); ++i) iv[i] = i * 23 + key_bytes;
    srtp_cipher_t *cipher = NULL;
    assert(type->alloc(&cipher, key_bytes + 14, 0) == srtp_err_status_ok);
    reference_t ref;
    for (size_t len = 0; len < sizeof(lengths) / sizeof(lengths[0]); ++len) {
        for (unsigned src = 0; src < 8; ++src) {
            for (unsigned dst = 0; dst < 8; ++dst) {
                reset(cipher, key, iv, &ref);
                checked_update(cipher, &ref, lengths[len], src, dst, 0);
            }
            reset(cipher, key, iv, &ref);
            checked_update(cipher, &ref, lengths[len], src, src, 1);
        }
    }
    /* Reach every saved keystream offset, including prefix-only calls,
     * followed by aligned/unaligned blocks and another partial block. */
    for (unsigned prefix = 0; prefix < 32; ++prefix) {
        for (unsigned src = 0; src < 8; ++src) {
            for (unsigned dst = 0; dst < 8; ++dst) {
                reset(cipher, key, iv, &ref);
                checked_update(cipher, &ref, prefix, src, dst, 0);
                checked_update(cipher, &ref, 0, src, dst, 0);
                checked_update(cipher, &ref, 3, src, dst, 0);
                checked_update(cipher, &ref, 1300, src, dst, 0);
                checked_update(cipher, &ref, 17, src, src, 1);
            }
        }
    }
    /* Exercise counter carries at every octet, including 128-bit wrap. */
    memset(key + key_bytes, 0, 14);
    for (unsigned carry = 0; carry <= 16; ++carry) {
        memset(iv, 0x17, 16);
        memset(iv + 16 - carry, 0xff, carry);
        reset(cipher, key, iv, &ref);
        checked_update(cipher, &ref, 4097, carry % 8, (carry + 3) % 8, 0);
    }
    /* A zero-byte or saved-keystream-only call must not touch the hardware. */
    reset(cipher, key, iv, &ref);
    size_t count = acquires;
    checked_update(cipher, &ref, 0, 0, 0, 0);
    assert(acquires == count);
    checked_update(cipher, &ref, 1, 0, 0, 0);
    assert(acquires == count + 1);
    checked_update(cipher, &ref, 15, 0, 0, 0);
    assert(acquires == count + 1);

    /* Preserve existing failure semantics and always release hardware. */
    reset(cipher, key, iv, &ref);
    uint8_t input[33], output[33];
    memset(input, 0x5a, sizeof(input));
    memset(output, 0xa5, sizeof(output));
    size_t length = 32;
    count = acquires;
    assert(type->encrypt(cipher->state, input, 33, output, &length) ==
           srtp_err_status_buffer_small);
    assert(acquires == count && length == 32);
    for (size_t i = 0; i < sizeof(output); ++i) assert(output[i] == 0xa5);
    fail_setkey = 1;
    length = 33;
    assert(type->encrypt(cipher->state, input, 33, output, &length) ==
           srtp_err_status_cipher_fail);
    assert(!owns_hardware && acquires == releases && length == 33);
    for (size_t i = 0; i < sizeof(output); ++i) assert(output[i] == 0);
    assert(((srtp_aes_icm_ctx_t *)cipher->state)->nc_off == 0);
    assert(memcmp(((srtp_aes_icm_ctx_t *)cipher->state)->counter.v8,
                  ref.counter, 16) == 0);
    checked_update(cipher, &ref, 1300, 1, 3, 0);

    /* Failure after consuming a saved block leaves that prefix encrypted
     * and zeros only the remaining output, just as the original backend. */
    reset(cipher, key, iv, &ref);
    checked_update(cipher, &ref, 5, 0, 0, 0);
    uint8_t expected[33];
    CRYPTO_ctr128_encrypt(input, expected, 11, &ref.key, ref.counter,
                         ref.stream, &ref.offset, reference_block);
    memset(output, 0xa5, sizeof(output));
    fail_setkey = 1;
    length = 33;
    assert(type->encrypt(cipher->state, input, 33, output, &length) ==
           srtp_err_status_cipher_fail);
    assert(!owns_hardware && acquires == releases);
    assert(memcmp(output, expected, 11) == 0);
    for (size_t i = 11; i < sizeof(output); ++i) assert(output[i] == 0);
    assert(((srtp_aes_icm_ctx_t *)cipher->state)->nc_off == 5);
    assert(type->dealloc(cipher) == srtp_err_status_ok);
}

typedef struct { srtp_cipher_t *cipher; uint8_t key[46]; } thread_arg_t;
static void *thread_main(void *opaque) {
    thread_arg_t *arg = opaque;
    uint8_t iv[16] = {0};
    reference_t ref;
    for (unsigned i = 0; i < 1000; ++i) {
        iv[12] = i >> 8; iv[13] = i;
        reset(arg->cipher, arg->key, iv, &ref);
        checked_update(arg->cipher, &ref, 1300, i % 8, (i + 1) % 8, 0);
        checked_update(arg->cipher, &ref, 17, i % 8, i % 8, 1);
    }
    return NULL;
}

int main(void) {
    known_answer(&srtp_aes_icm_128);
    known_answer(&srtp_aes_icm_192);
    known_answer(&srtp_aes_icm_256);
    exercise(&srtp_aes_icm_128, 16);
    exercise(&srtp_aes_icm_192, 24);
    exercise(&srtp_aes_icm_256, 32);
    thread_arg_t a = {0}, b = {0};
    memset(a.key, 0x23, sizeof(a.key));
    memset(b.key, 0xb7, sizeof(b.key));
    assert(srtp_aes_icm_128.alloc(&a.cipher, 30, 0) == srtp_err_status_ok);
    assert(srtp_aes_icm_256.alloc(&b.cipher, 46, 0) == srtp_err_status_ok);
    size_t allocation_count = allocations;
    pthread_t ta, tb;
    assert(pthread_create(&ta, NULL, thread_main, &a) == 0);
    assert(pthread_create(&tb, NULL, thread_main, &b) == 0);
    assert(pthread_join(ta, NULL) == 0);
    assert(pthread_join(tb, NULL) == 0);
    assert(allocations == allocation_count);
    assert(srtp_aes_icm_128.dealloc(a.cipher) == srtp_err_status_ok);
    assert(srtp_aes_icm_256.dealloc(b.cipher) == srtp_err_status_ok);
    assert(allocations == frees && acquires == releases && transforms > 1000);
    puts("production AES-CTR: vectors, tails, alignment, split updates, carries, "
         "errors, shared hardware, no packet allocation: ok");
    return 0;
}
"""


class AesCtrBlockTest(unittest.TestCase):
    def test_incremental_patch_replays_matches_and_reverses(self) -> None:
        cache = Path.home() / ".cache/Espressif/ComponentManager"
        matches = sorted(cache.glob("**/espressif__esp_libsrtp_1.0.0_*"))
        if not matches:
            self.skipTest("esp_libsrtp 1.0.0 registry cache is absent")
        with tempfile.TemporaryDirectory(prefix="libsrtp-aes-block-patch-") as tmp:
            directory = Path(tmp)
            component = directory / "managed_components/espressif__esp_libsrtp"
            component.parent.mkdir(parents=True)
            shutil.copytree(matches[-1], component)
            run("git", "apply", str(BASE_PATCH), cwd=directory)
            before = (component / SOURCE).read_bytes()
            run("git", "apply", "--check", str(PATCH), cwd=directory)
            run("git", "apply", str(PATCH), cwd=directory)
            self.assertEqual((component / SOURCE).read_bytes(),
                             (MANAGED / SOURCE).read_bytes())
            run("git", "apply", "--reverse", "--check", str(PATCH), cwd=directory)
            run("git", "apply", "--reverse", str(PATCH), cwd=directory)
            self.assertEqual((component / SOURCE).read_bytes(), before)

    def test_production_backend_against_openssl_with_asan_ubsan(self) -> None:
        with tempfile.TemporaryDirectory(prefix="libsrtp-aes-block-host-") as tmp:
            directory = Path(tmp)
            stub = directory / "stub"
            (stub / "aes").mkdir(parents=True)
            (stub / "hal").mkdir()
            (stub / "aes/esp_aes.h").write_text(ESP_AES_H)
            (stub / "hal/aes_hal.h").write_text(AES_HAL_H)
            (stub / "esp_attr.h").write_text("#define IRAM_ATTR\n")
            harness = directory / "harness.c"
            harness.write_text(HARNESS)
            for enabled in (0, 1):
                with self.subTest(block_xor=enabled):
                    binary = directory / f"aes_ctr_{enabled}"
                    run(
                        os.environ.get("CC", "cc"), "-std=c11", "-O2",
                        "-Wall", "-Wextra", "-Werror",
                        "-Wno-deprecated-declarations", "-pthread",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-fno-sanitize-recover=all", "-no-pie", "-DESP_AES",
                        f"-DJETKVM_SRTP_AES_BLOCK_XOR={enabled}",
                        f"-I{stub}", f"-I{MANAGED / 'libsrtp/crypto/include'}",
                        f"-I{MANAGED / 'libsrtp/include'}", str(MANAGED / SOURCE),
                        str(MANAGED / "libsrtp/crypto/cipher/cipher_test_cases.c"),
                        str(harness), "-lcrypto", "-o", str(binary), cwd=directory,
                    )
                    result = run(str(binary), cwd=directory)
                    self.assertIn("production AES-CTR:", result.stdout)


if __name__ == "__main__":
    unittest.main()

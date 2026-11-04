#!/usr/bin/env python3
"""Offline checks for the ESP32-P4 allocation-free hardware HMAC backend."""

from __future__ import annotations

import hashlib
import hmac
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
PATCH_DIR = ROOT / "patches"
PATCH = PATCH_DIR / "esp_libsrtp-1.0.0-esp-hw-noalloc-hmac-sha1.patch"
AES_PATCH = PATCH_DIR / "esp_libsrtp-1.0.0-mbedtls-aes-icm.patch"
AES_BLOCK_PATCH = PATCH_DIR / "esp_libsrtp-1.0.0-aes-ctr-block-xor.patch"
HMAC_FINISH_PATCH = PATCH_DIR / "esp_libsrtp-1.0.0-hmac-fused-finish.patch"
HMAC_POLLING_PATCH = PATCH_DIR / "esp_libsrtp-1.0.0-hmac-polling-update.patch"
MBEDTLS_HMAC_PATCH = PATCH_DIR / "esp_libsrtp-1.0.0-mbedtls-hmac-sha1.patch"
MANAGED = ROOT / "managed_components" / "espressif__esp_libsrtp"


def _run(*args: str, cwd: Path) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        args,
        cwd=cwd,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if completed.returncode != 0:
        raise AssertionError(
            f"command failed ({completed.returncode}): {' '.join(args)}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed


def _registry_component() -> Path:
    cache = Path.home() / ".cache" / "Espressif" / "ComponentManager"
    matches = sorted(cache.glob("**/espressif__esp_libsrtp_1.0.0_*"))
    if not matches:
        raise unittest.SkipTest("esp_libsrtp 1.0.0 registry cache is absent")
    return matches[-1]


def _c_bytes(data: bytes) -> str:
    return ", ".join(f"0x{byte:02x}" for byte in data)


class HardwareHmacPatchTest(unittest.TestCase):
    def _prepare_patched_component(self, directory: Path) -> Path:
        component = directory / "managed_components" / "espressif__esp_libsrtp"
        component.parent.mkdir(parents=True)
        shutil.copytree(_registry_component(), component)
        _run("git", "apply", str(AES_PATCH), cwd=directory)
        _run("git", "apply", str(MBEDTLS_HMAC_PATCH), cwd=directory)
        return component

    def test_aes_packet_path_is_iram_resident_and_patch_reverses(self) -> None:
        with tempfile.TemporaryDirectory(prefix="libsrtp-hw-aes-patch-") as tmp:
            root = Path(tmp)
            component = root / "managed_components" / "espressif__esp_libsrtp"
            component.parent.mkdir(parents=True)
            shutil.copytree(_registry_component(), component)

            _run("git", "apply", "--check", str(AES_PATCH), cwd=root)
            _run("git", "apply", str(AES_PATCH), cwd=root)
            _run("git", "apply", str(AES_BLOCK_PATCH), cwd=root)

            source_path = (
                component / "libsrtp/crypto/cipher/aes_icm_mbedtls.c"
            )
            source = source_path.read_text()
            self.assertIn('#include "esp_attr.h"', source)
            self.assertIn(
                "static IRAM_ATTR int srtp_esp_aes_crypt_ctr_noalloc(", source
            )
            self.assertIn(
                "static IRAM_ATTR srtp_err_status_t "
                "srtp_aes_icm_mbedtls_encrypt(",
                source,
            )
            self.assertNotIn("mbedtls_aes_crypt_ctr(", source)
            self.assertEqual(
                source_path.read_bytes(),
                (
                    MANAGED
                    / "libsrtp/crypto/cipher/aes_icm_mbedtls.c"
                ).read_bytes(),
            )

            _run("git", "apply", "--reverse", str(AES_BLOCK_PATCH), cwd=root)
            _run("git", "apply", "--reverse", "--check", str(AES_PATCH), cwd=root)
            _run("git", "apply", "--reverse", str(AES_PATCH), cwd=root)
            _run("git", "apply", "--check", str(AES_PATCH), cwd=root)

    def test_patch_replays_after_existing_crypto_patches_and_reverses(self) -> None:
        with tempfile.TemporaryDirectory(prefix="libsrtp-hw-hmac-patch-") as tmp:
            root = Path(tmp)
            component = self._prepare_patched_component(root)

            _run("git", "apply", "--check", str(PATCH), cwd=root)
            _run("git", "apply", str(PATCH), cwd=root)
            _run("git", "apply", str(HMAC_FINISH_PATCH), cwd=root)
            _run("git", "apply", str(HMAC_POLLING_PATCH), cwd=root)
            _run("git", "apply", "--reverse", "--check", str(HMAC_POLLING_PATCH), cwd=root)

            cmake = (component / "CMakeLists.txt").read_text()
            self.assertIn("libsrtp/crypto/hash/hmac_esp_sha1.c", cmake)
            self.assertIn("PRIV_REQUIRES heap mbedtls", cmake)
            self.assertNotIn("libsrtp/crypto/hash/hmac.c\n", cmake)
            self.assertNotIn("libsrtp/crypto/hash/sha1.c\n", cmake)
            self.assertEqual(
                (component / "libsrtp/crypto/hash/hmac_esp_sha1.c").read_bytes(),
                (MANAGED / "libsrtp/crypto/hash/hmac_esp_sha1.c").read_bytes(),
            )

            _run("git", "apply", "--reverse", str(HMAC_POLLING_PATCH), cwd=root)
            _run("git", "apply", "--reverse", "--check", str(HMAC_FINISH_PATCH), cwd=root)
            _run("git", "apply", "--reverse", str(HMAC_FINISH_PATCH), cwd=root)
            _run("git", "apply", "--reverse", "--check", str(PATCH), cwd=root)
            _run("git", "apply", "--reverse", str(PATCH), cwd=root)
            self.assertFalse(
                (component / "libsrtp/crypto/hash/hmac_esp_sha1.c").exists()
            )
            _run("git", "apply", "--check", str(PATCH), cwd=root)

    def test_source_uses_dma_session_state_not_packet_psa_operations(self) -> None:
        source = (MANAGED / "libsrtp/crypto/hash/hmac_esp_sha1.c").read_text()
        self.assertIn("MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA", source)
        self.assertEqual(source.count("heap_caps_calloc("), 1)
        self.assertEqual(source.count("heap_caps_free("), 1)
        self.assertIn("esp_sha1_context inner_seed", source)
        self.assertIn("esp_sha1_context outer_seed", source)
        self.assertIn("esp_sha1_context work", source)
        self.assertNotIn("psa_hash_", source)
        self.assertNotIn("mbedtls_md_", source)

    def test_rfc_vectors_truncation_split_updates_threads_and_no_packet_alloc(
        self,
    ) -> None:
        data_a = bytes((index * 17 + 3) & 0xFF for index in range(1500))
        key_a = bytes(range(20))
        tag_a = hmac.new(key_a, data_a, hashlib.sha1).digest()
        data_b = b"Test With Truncation"
        key_b = bytes([0x0C]) * 20
        tag_b = hmac.new(key_b, data_b, hashlib.sha1).digest()[:12]
        rfc_key = bytes([0x0B]) * 20
        rfc_data = b"Hi There"
        rfc_tag = bytes.fromhex("b617318655057264e28bc0b6fb378c8ef146be00")

        with tempfile.TemporaryDirectory(prefix="libsrtp-hw-hmac-host-") as tmp:
            tmpdir = Path(tmp)
            stub = tmpdir / "stub"
            (stub / "soc").mkdir(parents=True)
            (stub / "sdkconfig.h").write_text(
                "#define CONFIG_IDF_TARGET_ESP32P4 1\n"
                "#define CONFIG_MBEDTLS_HARDWARE_SHA 1\n"
            )
            (stub / "soc" / "soc_caps.h").write_text(
                "#define SOC_SHA_SUPPORTED 1\n#define SOC_SHA_SUPPORT_SHA1 1\n"
            )
            (stub / "esp_heap_caps.h").write_text(
                textwrap.dedent(
                    """
                    #pragma once
                    #include <stddef.h>
                    #define MALLOC_CAP_8BIT 1u
                    #define MALLOC_CAP_INTERNAL 2u
                    #define MALLOC_CAP_DMA 4u
                    void *heap_caps_calloc(size_t count, size_t size,
                                           unsigned caps);
                    void heap_caps_free(void *ptr);
                    """
                )
            )
            (stub / "psa_crypto_driver_esp_sha.h").write_text(
                textwrap.dedent(
                    """
                    #pragma once
                    #include <stddef.h>
                    #include <stdint.h>
                    #include <openssl/sha.h>
                    typedef struct { SHA_CTX sha; } esp_sha1_context;
                    int esp_sha1_starts(esp_sha1_context *ctx);
                    int esp_sha1_update(esp_sha1_context *ctx,
                                        const unsigned char *input,
                                        size_t length);
                    int esp_sha1_finish(esp_sha1_context *ctx,
                                        uint8_t output[20]);
                    """
                )
            )

            harness = textwrap.dedent(
                f"""
                #include <assert.h>
                #include <pthread.h>
                #include <stdarg.h>
                #include <stdint.h>
                #include <stdio.h>
                #include <stdlib.h>
                #include <string.h>

                #include "auth.h"
                #include "auth_test_cases.h"
                #include "err.h"
                #include "esp_heap_caps.h"
                #include "psa_crypto_driver_esp_sha.h"

                extern const srtp_auth_type_t srtp_hmac;

                static size_t crypto_allocs;
                static size_t crypto_frees;
                static size_t dma_allocs;
                static size_t dma_frees;
                static size_t dma_size;
                static unsigned dma_caps;
                static int dma_was_zeroized;
                static int sha_update_failure_countdown = -1;

                const srtp_auth_test_case_t srtp_hmac_test_case_0 = {{0}};

                void *srtp_crypto_alloc(size_t size) {{
                    ++crypto_allocs;
                    return calloc(1, size);
                }}
                void srtp_crypto_free(void *ptr) {{
                    ++crypto_frees;
                    free(ptr);
                }}
                void *heap_caps_calloc(size_t count, size_t size,
                                       unsigned caps) {{
                    ++dma_allocs;
                    dma_size = count * size;
                    dma_caps = caps;
                    return calloc(count, size);
                }}
                void heap_caps_free(void *ptr) {{
                    const uint8_t *bytes = ptr;
                    dma_was_zeroized = 1;
                    for (size_t i = 0; i < dma_size; ++i) {{
                        if (bytes[i] != 0) {{
                            dma_was_zeroized = 0;
                            break;
                        }}
                    }}
                    ++dma_frees;
                    free(ptr);
                }}
                void octet_string_set_to_zero(void *ptr, size_t size) {{
                    volatile uint8_t *bytes = ptr;
                    while (size-- != 0) {{
                        *bytes++ = 0;
                    }}
                }}
                char *srtp_octet_string_hex_string(const void *ptr,
                                                   size_t size) {{
                    (void)ptr;
                    (void)size;
                    static char empty[] = "";
                    return empty;
                }}
                void srtp_err_report(srtp_err_reporting_level_t level,
                                     const char *format, ...) {{
                    (void)level;
                    (void)format;
                }}

                int esp_sha1_starts(esp_sha1_context *ctx) {{
                    return SHA1_Init(&ctx->sha) == 1 ? 0 : -1;
                }}
                int esp_sha1_update(esp_sha1_context *ctx,
                                    const unsigned char *input,
                                    size_t length) {{
                    if (sha_update_failure_countdown == 0) {{
                        sha_update_failure_countdown = -1;
                        return -1;
                    }}
                    if (sha_update_failure_countdown > 0) {{
                        --sha_update_failure_countdown;
                    }}
                    if (length == 0) {{
                        return 0;
                    }}
                    return SHA1_Update(&ctx->sha, input, length) == 1 ? 0 : -1;
                }}
                int esp_sha1_finish(esp_sha1_context *ctx,
                                    uint8_t output[20]) {{
                    return SHA1_Final(output, &ctx->sha) == 1 ? 0 : -1;
                }}

                static const uint8_t key_a[20] = {{{_c_bytes(key_a)}}};
                static const uint8_t data_a[1500] = {{{_c_bytes(data_a)}}};
                static const uint8_t tag_a[20] = {{{_c_bytes(tag_a)}}};
                static const uint8_t key_b[20] = {{{_c_bytes(key_b)}}};
                static const uint8_t data_b[{len(data_b)}] =
                    {{{_c_bytes(data_b)}}};
                static const uint8_t tag_b[12] = {{{_c_bytes(tag_b)}}};
                static const uint8_t rfc_key[20] = {{{_c_bytes(rfc_key)}}};
                static const uint8_t rfc_data[{len(rfc_data)}] =
                    {{{_c_bytes(rfc_data)}}};
                static const uint8_t rfc_tag[20] = {{{_c_bytes(rfc_tag)}}};

                typedef struct {{
                    srtp_auth_t *auth;
                    const uint8_t *data;
                    size_t data_size;
                    size_t split;
                    const uint8_t *tag;
                    size_t tag_size;
                    int iterations;
                }} thread_args_t;

                static void compute_checked(thread_args_t *args) {{
                    uint8_t result[20] = {{0}};
                    assert(srtp_auth_start(args->auth) == srtp_err_status_ok);
                    assert(srtp_auth_update(args->auth, args->data, args->split) ==
                           srtp_err_status_ok);
                    assert(srtp_auth_compute(args->auth,
                                             args->data + args->split,
                                             args->data_size - args->split,
                                             result) == srtp_err_status_ok);
                    assert(memcmp(result, args->tag, args->tag_size) == 0);
                }}

                static void *thread_main(void *opaque) {{
                    thread_args_t *args = opaque;
                    for (int i = 0; i < args->iterations; ++i) {{
                        compute_checked(args);
                    }}
                    return NULL;
                }}

                int main(void) {{
                    /* RFC 2202 test case 1, with the full SHA-1 tag. */
                    srtp_auth_t *auth_rfc = NULL;
                    uint8_t rfc_result[20] = {{0}};
                    assert(srtp_auth_type_alloc(&srtp_hmac, &auth_rfc, 20, 20) ==
                           srtp_err_status_ok);
                    assert(srtp_auth_init(auth_rfc, rfc_key) ==
                           srtp_err_status_ok);
                    assert(srtp_auth_start(auth_rfc) == srtp_err_status_ok);
                    assert(srtp_auth_compute(auth_rfc, rfc_data,
                                             sizeof(rfc_data), rfc_result) ==
                           srtp_err_status_ok);
                    assert(memcmp(rfc_result, rfc_tag, sizeof(rfc_tag)) == 0);
                    assert(srtp_auth_dealloc(auth_rfc) == srtp_err_status_ok);
                    assert(dma_was_zeroized == 1);

                    srtp_auth_t *auth_a = NULL;
                    srtp_auth_t *auth_b = NULL;
                    assert(srtp_auth_type_alloc(&srtp_hmac, &auth_a, 20, 20) ==
                           srtp_err_status_ok);
                    assert(srtp_auth_type_alloc(&srtp_hmac, &auth_b, 20, 12) ==
                           srtp_err_status_ok);
                    assert(dma_allocs == 3 && crypto_allocs == 3);
                    assert(dma_caps == (MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL |
                                        MALLOC_CAP_DMA));
                    assert(dma_size >= 3 * sizeof(esp_sha1_context) + 20);
                    assert(srtp_auth_init(auth_a, key_a) == srtp_err_status_ok);
                    assert(srtp_auth_init(auth_b, key_b) == srtp_err_status_ok);

                    const size_t allocs_before_packets = dma_allocs + crypto_allocs;
                    thread_args_t a = {{auth_a, data_a, sizeof(data_a), 12,
                                        tag_a, sizeof(tag_a), 25000}};
                    thread_args_t b = {{auth_b, data_b, sizeof(data_b), 7,
                                        tag_b, sizeof(tag_b), 25000}};
                    pthread_t thread_a;
                    pthread_t thread_b;
                    assert(pthread_create(&thread_a, NULL, thread_main, &a) == 0);
                    assert(pthread_create(&thread_b, NULL, thread_main, &b) == 0);
                    assert(pthread_join(thread_a, NULL) == 0);
                    assert(pthread_join(thread_b, NULL) == 0);
                    assert(dma_allocs + crypto_allocs == allocs_before_packets);

                    /* A driver error is surfaced, and start() restores the
                     * precomputed inner state for the following packet. */
                    uint8_t result[20] = {{0}};
                    assert(srtp_auth_start(auth_a) == srtp_err_status_ok);
                    sha_update_failure_countdown = 0;
                    assert(srtp_auth_compute(auth_a, data_a, sizeof(data_a),
                                             result) ==
                           srtp_err_status_auth_fail);
                    compute_checked(&a);
                    assert(dma_allocs + crypto_allocs == allocs_before_packets);

                    assert(srtp_auth_dealloc(auth_a) == srtp_err_status_ok);
                    assert(dma_was_zeroized == 1);
                    assert(srtp_auth_dealloc(auth_b) == srtp_err_status_ok);
                    assert(dma_was_zeroized == 1);
                    assert(dma_frees == 3 && crypto_frees == 3);
                    puts("hardware HMAC host model: ok");
                    return 0;
                }}
                """
            )
            harness_path = tmpdir / "harness.c"
            harness_path.write_text(harness)
            binary = tmpdir / "hmac_test"
            include = MANAGED / "libsrtp" / "crypto" / "include"
            hash_include = MANAGED / "libsrtp" / "crypto" / "hash"
            public_include = MANAGED / "libsrtp" / "include"
            source = MANAGED / "libsrtp" / "crypto" / "hash" / "hmac_esp_sha1.c"
            _run(
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-Wno-deprecated-declarations",
                "-pthread",
                f"-I{stub}",
                f"-I{include}",
                f"-I{hash_include}",
                f"-I{public_include}",
                str(source),
                str(harness_path),
                "-lcrypto",
                "-o",
                str(binary),
                cwd=tmpdir,
            )
            completed = _run(str(binary), cwd=tmpdir)
            self.assertIn("hardware HMAC host model: ok", completed.stdout)


if __name__ == "__main__":
    unittest.main()

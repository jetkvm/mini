#!/usr/bin/env python3
"""Run the HMAC backend with IDF's real SHA streaming code and OpenSSL blocks."""

from __future__ import annotations

import hashlib
import hmac
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "managed_components/espressif__esp_libsrtp/libsrtp/crypto/hash/hmac_esp_sha1.c"
IDF = Path(os.environ.get("IDF_PATH", str(Path.home() / ".espressif/v6.0.2/esp-idf")))


class FusedFinishTest(unittest.TestCase):
    def test_sdk_streaming_reference_vectors_boundaries_locks_and_errors(self):
        driver = IDF / "components/mbedtls/port/psa_driver/esp_sha/core/psa_crypto_driver_esp_sha1.c"
        if not driver.exists():
            self.skipTest("ESP-IDF SHA core source is unavailable")
        # Compile the unmodified streaming algorithms, omitting only the PSA
        # entrypoints and their declaration include. No optimized padding logic
        # is duplicated in the reference path.
        sdk_code = driver.read_text().split("psa_status_t esp_sha1_driver_compute(", 1)[0]
        sdk_code = sdk_code.replace('#include "../include/psa_crypto_driver_esp_sha1.h"', "")
        lengths = list(range(0, 140)) + [255, 256, 511, 512, 513, 1172, 1200, 1254, 1500]
        vectors = []
        for length in lengths:
            key = bytes(range(20))
            data = bytes((i * 17 + 3) & 255 for i in range(length))
            vectors.append((length, hmac.new(key, data, hashlib.sha1).digest()))
        with tempfile.TemporaryDirectory(prefix="hmac-fused-") as temp:
            tempdir = Path(temp)
            for directory in ("mbedtls", "soc", "sha", "hal"):
                (tempdir / directory).mkdir()
            (tempdir / "sdkconfig.h").write_text("#define CONFIG_IDF_TARGET_ESP32P4 1\n#define CONFIG_MBEDTLS_HARDWARE_SHA 1\n")
            (tempdir / "soc/soc_caps.h").write_text("#define SOC_SHA_SUPPORTED 1\n#define SOC_SHA_SUPPORT_SHA1 1\n#define SOC_SHA_SUPPORT_DMA 1\n")
            (tempdir / "mbedtls/esp_config.h").write_text("#define MBEDTLS_PSA_ACCEL_ALG_SHA_1 1\n")
            (tempdir / "esp_err.h").write_text("#define ESP_OK 0\n#define unlikely(x) (x)\n")
            (tempdir / "esp_sha_internal.h").write_bytes((IDF / "components/mbedtls/port/sha/core/include/esp_sha_internal.h").read_bytes())
            (tempdir / "psa_crypto_driver_esp_sha.h").write_text(r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
enum { ESP_SHA1_STATE_INIT, ESP_SHA1_STATE_IN_PROCESS };
typedef struct {
    uint32_t total[2]; uint32_t state[5]; unsigned char buffer[64];
    bool first_block; int sha_state;
} esp_sha1_context;
#define PUT_UINT32_BE(n,b,i) do { uint32_t v_ = (n); \
    (b)[i] = v_ >> 24; (b)[(i)+1] = v_ >> 16; \
    (b)[(i)+2] = v_ >> 8; (b)[(i)+3] = v_; } while (0)
int esp_sha1_starts(esp_sha1_context *);
int esp_sha1_update(esp_sha1_context *, const unsigned char *, size_t);
int esp_sha1_finish(esp_sha1_context *, uint8_t *);
''')
            (tempdir / "sha/sha_core.h").write_text(r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
enum { SHA1 };
void esp_sha_acquire_hardware(void);
void esp_sha_release_hardware(void);
void esp_sha_set_mode(int);
void esp_sha_write_digest_state(int, void *);
void esp_sha_read_digest_state(int, void *);
void esp_sha_block(int, const void *, bool);
int esp_sha_dma(int, const void *, uint32_t, const void *, uint32_t, bool);
''')
            (tempdir / "hal/sha_hal.h").write_text("void sha_hal_wait_idle(void);\n")
            (tempdir / "esp_heap_caps.h").write_text(r'''
#pragma once
#include <stddef.h>
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_DMA 4
void *heap_caps_calloc(size_t, size_t, unsigned);
void heap_caps_free(void *);
''')
            (tempdir / "sdk_sha1.c").write_text(sdk_code)
            vector_text = ",\n".join("{" + str(length) + ", {" + ",".join(str(x) for x in tag) + "}}" for length, tag in vectors)
            harness = r'''
#include <assert.h>
#include <pthread.h>
#define SHA1 openssl_sha1_one_shot
#include <openssl/sha.h>
#undef SHA1
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include "sha/sha_core.h"
#include "psa_crypto_driver_esp_sha.h"
#include "@SOURCE@"

static SHA_CTX hardware;
static pthread_mutex_t hardware_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned owned;
static unsigned acquisitions, allocations, frees;
static int fail_dma;
static int zero_next_block;
static size_t allocation_size;
const srtp_auth_test_case_t srtp_hmac_test_case_0 = {0};
static uint32_t read_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
void esp_sha_acquire_hardware(void) {
    assert(owned == 0); pthread_mutex_lock(&hardware_mutex);
    owned = 1; acquisitions++;
}
void esp_sha_release_hardware(void) {
    assert(owned == 1); owned = 0; pthread_mutex_unlock(&hardware_mutex);
}
void esp_sha_set_mode(int mode) { assert(owned == 1 && mode == SHA1); }
void esp_sha_write_digest_state(int mode, void *p) {
    assert(owned == 1 && mode == SHA1);
    const uint8_t *b = p;
    hardware.h0 = read_be(b); hardware.h1 = read_be(b+4);
    hardware.h2 = read_be(b+8); hardware.h3 = read_be(b+12);
    hardware.h4 = read_be(b+16);
}
void esp_sha_read_digest_state(int mode, void *p) {
    assert(owned == 1 && mode == SHA1);
    uint8_t *b = p;
    PUT_UINT32_BE(hardware.h0,b,0); PUT_UINT32_BE(hardware.h1,b,4);
    PUT_UINT32_BE(hardware.h2,b,8); PUT_UINT32_BE(hardware.h3,b,12);
    PUT_UINT32_BE(hardware.h4,b,16);
    /* Match IDF sha_hal_read_digest's all-zero fault-injection check. */
    if (!(hardware.h0 | hardware.h1 | hardware.h2 | hardware.h3 | hardware.h4))
        abort();
}
void esp_sha_block(int mode, const void *p, bool first) {
    assert(owned == 1 && mode == SHA1);
    if (first) SHA1_Init(&hardware);
    SHA1_Transform(&hardware, p);
    if (zero_next_block) {
        zero_next_block = 0;
        hardware.h0 = hardware.h1 = hardware.h2 = hardware.h3 = hardware.h4 = 0;
    }
}
void sha_hal_wait_idle(void) { assert(owned == 1); }
int esp_sha_dma(int mode, const void *input, uint32_t length,
                const void *prefix, uint32_t prefix_length, bool first) {
    assert(owned == 1 && length % 64 == 0 && prefix_length % 64 == 0);
    if (fail_dma) { fail_dma = 0; return -1; }
    for (uint32_t i = 0; i < prefix_length; i += 64) {
        esp_sha_block(mode, (const uint8_t *)prefix+i, first); first = false;
    }
    for (uint32_t i = 0; i < length; i += 64) {
        esp_sha_block(mode, (const uint8_t *)input+i, first); first = false;
    }
    return 0;
}
void *srtp_crypto_alloc(size_t size) { allocations++; return calloc(1,size); }
void srtp_crypto_free(void *p) { frees++; free(p); }
void *heap_caps_calloc(size_t n, size_t size, unsigned caps) {
    assert(caps == 7); allocations++; allocation_size = n*size;
    return calloc(n,size);
}
void heap_caps_free(void *p) {
    const uint8_t *b = p;
    for (size_t i=0; i<allocation_size; i++) assert(b[i] == 0);
    frees++; free(p);
}
void octet_string_set_to_zero(void *p, size_t n) {
    volatile uint8_t *b = p; while(n--) *b++ = 0;
}
char *srtp_octet_string_hex_string(const void *p, size_t n) {
    (void)p; (void)n; static char empty[] = ""; return empty;
}
void srtp_err_report(srtp_err_reporting_level_t level, const char *fmt, ...) {
    (void)level; (void)fmt;
}
struct vector { size_t length; uint8_t tag[20]; };
static const struct vector vectors[] = { @VECTORS@ };
static void check_message(srtp_auth_t *auth, const uint8_t *data,
                          const struct vector *v, size_t split, size_t taglen) {
    uint8_t actual[20] = {0};
    assert(srtp_hmac_esp_sha1_start(auth->state) == srtp_err_status_ok);
    assert(srtp_hmac_esp_sha1_update(auth->state,data,split) == srtp_err_status_ok);
    assert(srtp_hmac_esp_sha1_compute(auth->state,data+split,v->length-split,
                                     taglen,actual) == srtp_err_status_ok);
    assert(memcmp(actual,v->tag,taglen) == 0);
    for(size_t i=taglen;i<20;i++) assert(actual[i] == 0);
    assert(owned == 0);
    srtp_hmac_esp_sha1_ctx_t *state = auth->state;
    assert(state->work.total[0] == 128 && state->work.total[1] == 0);
    for(size_t i=0;i<20;i++) assert(state->digest[i] == 0);
}
static void *thread_run(void *p) {
    srtp_auth_t *auth = p;
    uint8_t data[1500]; for(size_t i=0;i<sizeof(data);i++) data[i]=i*17+3;
    for(unsigned i=0;i<2000;i++)
        check_message(auth,data,&vectors[sizeof(vectors)/sizeof(vectors[0])-1],i%65,20);
    return NULL;
}
int main(int argc, char **argv) {
    struct rlimit no_core = {0,0};
    assert(setrlimit(RLIMIT_CORE, &no_core) == 0);
    uint8_t key[20], data[1500];
    for(size_t i=0;i<sizeof(key);i++) key[i]=i;
    for(size_t i=0;i<sizeof(data);i++) data[i]=i*17+3;
    srtp_auth_t *auth, *other;
    assert(srtp_hmac_esp_sha1_alloc(&auth,20,20) == srtp_err_status_ok);
    assert(srtp_hmac_esp_sha1_alloc(&other,20,20) == srtp_err_status_ok);
    assert(allocation_size == 3*sizeof(esp_sha1_context)+20);
    assert(srtp_hmac_esp_sha1_init(auth->state,key,20) == srtp_err_status_ok);
    assert(srtp_hmac_esp_sha1_init(other->state,key,20) == srtp_err_status_ok);
    if(argc > 1 && strcmp(argv[1], "fault-first-padding") == 0) {
        uint8_t result[20];
        assert(srtp_hmac_esp_sha1_start(auth->state) == srtp_err_status_ok);
        assert(srtp_hmac_esp_sha1_update(auth->state,data,56) == srtp_err_status_ok);
        zero_next_block = 1;
        /* Two-block padding must check the first block before continuing;
         * otherwise hashing the second block masks this injected zero state. */
        srtp_hmac_esp_sha1_compute(auth->state,NULL,0,20,result);
        return 3;
    }
    unsigned initial_allocations = allocations;
    for(size_t v=0;v<sizeof(vectors)/sizeof(vectors[0]);v++) {
        for(size_t split=0;split<=vectors[v].length;split++) {
            if(split > 70 && split+70 < vectors[v].length) continue;
            check_message(auth,data,&vectors[v],split,20);
        }
        check_message(auth,data,&vectors[v],vectors[v].length/2,10);
        check_message(auth,data,&vectors[v],vectors[v].length/2,4);
        check_message(auth,data,&vectors[v],vectors[v].length/2,0);
    }
    /* Compare whole post-finish state and 64-bit length handling to the
     * original IDF finish path using identical arbitrary SHA intermediate
     * states. High totals test carry/bit lengths without hashing gigabytes. */
    for(unsigned i=0;i<64;i++) {
        srtp_hmac_esp_sha1_start(auth->state);
        srtp_hmac_esp_sha1_ctx_t *state = auth->state;
        state->work.total[0] = 0xffffffc0u + i;
        state->work.total[1] = 0x12345678u;
        memcpy(state->work.buffer,data,i);
        esp_sha1_context reference = state->work;
        uint8_t expected[20], actual[20];
        assert(esp_sha1_update(&reference,data,4) == 0);
        assert(esp_sha1_finish(&reference,expected) == 0);
        reference = state->outer_seed;
        assert(esp_sha1_update(&reference,expected,20) == 0);
        assert(esp_sha1_finish(&reference,expected) == 0);
        assert(srtp_hmac_esp_sha1_compute(state,data,4,20,actual) == srtp_err_status_ok);
        assert(memcmp(expected,actual,20) == 0);
        assert(memcmp(&reference,&state->work,sizeof(reference)) == 0);
    }
    srtp_hmac_esp_sha1_start(auth->state);
    srtp_hmac_esp_sha1_update(auth->state,data,1200);
    unsigned before = acquisitions;
    uint8_t actual[20];
    assert(srtp_hmac_esp_sha1_compute(auth->state,data,4,20,actual) == srtp_err_status_ok);
    assert(acquisitions-before == (JETKVM_SRTP_HMAC_FUSED_FINISH ? 1u : 2u));
    fail_dma = 1;
    srtp_hmac_esp_sha1_start(auth->state);
    memset(actual,0xa5,sizeof(actual));
    assert(srtp_hmac_esp_sha1_compute(auth->state,data,1200,20,actual) == srtp_err_status_auth_fail);
    for(size_t i=0;i<20;i++) assert(actual[i] == 0xa5);
    assert(owned == 0);
    check_message(auth,data,&vectors[0],0,20);
    assert(srtp_hmac_esp_sha1_compute(auth->state,NULL,1,20,actual) == srtp_err_status_bad_param);
    assert(srtp_hmac_esp_sha1_compute(auth->state,data,1,21,actual) == srtp_err_status_bad_param);
    assert(srtp_hmac_esp_sha1_compute(auth->state,data,1,20,NULL) == srtp_err_status_bad_param);
    pthread_t a,b;
    assert(pthread_create(&a,NULL,thread_run,auth) == 0);
    assert(pthread_create(&b,NULL,thread_run,other) == 0);
    assert(pthread_join(a,NULL) == 0); assert(pthread_join(b,NULL) == 0);
    assert(allocations == initial_allocations);
    srtp_hmac_esp_sha1_dealloc(auth); srtp_hmac_esp_sha1_dealloc(other);
    assert(frees == allocations);
    puts("SDK SHA + OpenSSL blocks: vectors, boundaries, carries, locks, errors, threads OK");
}
'''.replace("@SOURCE@", str(SOURCE)).replace("@VECTORS@", vector_text)
            (tempdir / "harness.c").write_text(harness)
            include = SOURCE.parents[1] / "include"
            common = [os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations", "-pthread", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-no-pie", f"-I{tempdir}", f"-I{include}", f"-I{SOURCE.parent}", f"-I{SOURCE.parents[2] / 'include'}"]
            for enabled in (0, 1):
                binary = tempdir / f"hmac-{enabled}"
                result = subprocess.run(common + [f"-DJETKVM_SRTP_HMAC_FUSED_FINISH={enabled}", str(tempdir / "sdk_sha1.c"), str(tempdir / "harness.c"), "-lcrypto", "-o", str(binary)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(binary)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("threads OK", result.stdout)
                fault = subprocess.run([str(binary), "fault-first-padding"], capture_output=True, text=True)
                self.assertEqual(fault.returncode, -signal.SIGABRT, fault.stdout + fault.stderr)


if __name__ == "__main__":
    unittest.main()

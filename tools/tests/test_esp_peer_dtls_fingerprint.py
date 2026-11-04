#!/usr/bin/env python3

import hashlib
import pathlib
import subprocess
import tempfile
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
COMPONENT_ROOT = REPO_ROOT / "third_party" / "esp_peer"

# Two certificates as the handshake sees them: the bytes are hashed, not parsed.
BROWSER = bytes(range(200))
OTHER = bytes(reversed(range(200)))


def colon_hex(digest: bytes) -> str:
    return ":".join(f"{byte:02X}" for byte in digest)


def c_bytes(data: bytes) -> str:
    return "{" + ",".join(str(byte) for byte in data) + "}"


def c_string(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("\r", "\\r").replace("\n", "\\n") + '"'


def offer(*fingerprints: str) -> str:
    lines = ["v=0", "o=- 1 2 IN IP4 127.0.0.1", "s=-", "t=0 0", "a=group:BUNDLE 0",
             "m=application 9 UDP/DTLS/SCTP webrtc-datachannel", "c=IN IP4 0.0.0.0"]
    lines += [f"a=fingerprint:{fingerprint}" for fingerprint in fingerprints]
    lines += ["a=setup:actpass", "a=mid:0", ""]
    return "\r\n".join(lines)


SHIM_MD = r'''
#pragma once
#include <stddef.h>
#include <openssl/sha.h>
typedef enum { MBEDTLS_MD_NONE, MBEDTLS_MD_SHA256, MBEDTLS_MD_SHA384, MBEDTLS_MD_SHA512 } mbedtls_md_type_t;
typedef struct { mbedtls_md_type_t type; unsigned char size; } mbedtls_md_info_t;
static const mbedtls_md_info_t shim_md[] = {{MBEDTLS_MD_SHA256, 32}, {MBEDTLS_MD_SHA384, 48}, {MBEDTLS_MD_SHA512, 64}};
static inline const mbedtls_md_info_t *mbedtls_md_info_from_type(mbedtls_md_type_t type)
{
    return type >= MBEDTLS_MD_SHA256 && type <= MBEDTLS_MD_SHA512 ? &shim_md[type - 1] : NULL;
}
static inline unsigned char mbedtls_md_get_size(const mbedtls_md_info_t *md) { return md->size; }
static inline int mbedtls_md(const mbedtls_md_info_t *md, const unsigned char *input, size_t size, unsigned char *output)
{
    if (md->type == MBEDTLS_MD_SHA256) SHA256(input, size, output);
    else if (md->type == MBEDTLS_MD_SHA384) SHA384(input, size, output);
    else SHA512(input, size, output);
    return 0;
}
'''

MEDIA_LIB = r'''
int media_lib_mutex_create(media_lib_mutex_handle_t *mutex) { static int lock; *mutex = &lock; return 0; }
int media_lib_mutex_lock(media_lib_mutex_handle_t mutex, uint32_t timeout) { (void)mutex; (void)timeout; return 0; }
int media_lib_mutex_unlock(media_lib_mutex_handle_t mutex) { (void)mutex; return 0; }
'''


def run_c(test: unittest.TestCase, harness: str, sources: list) -> None:
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        (root / "mbedtls").mkdir()
        (root / "mbedtls" / "md.h").write_text(SHIM_MD)
        (root / "harness.c").write_text(harness)
        binary = root / "harness"
        build = subprocess.run(
            ["cc", "-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", f"-I{root}", f"-I{COMPONENT_ROOT / 'src'}",
             str(root / "harness.c"), *[str(source) for source in sources], "-lcrypto", "-o", str(binary)],
            capture_output=True, text=True)
        test.assertEqual(build.returncode, 0, build.stderr)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        test.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class EspPeerDtlsFingerprintTest(unittest.TestCase):

    def test_a_certificate_matches_only_a_fingerprint_of_the_remote_sdp(self) -> None:
        sha256 = colon_hex(hashlib.sha256(BROWSER).digest())
        cases = [
            # SDP, whether it is taken, whether BROWSER then matches, whether OTHER does
            (offer(f"sha-256 {sha256}"), True, True, False),
            (offer(f"SHA-256 {sha256.lower()}"), True, True, False),
            (offer(f"sha-384 {colon_hex(hashlib.sha384(BROWSER).digest())}"), True, True, False),
            (offer(f"sha-512 {colon_hex(hashlib.sha512(BROWSER).digest())}"), True, True, False),
            (offer(f"sha-1 {colon_hex(hashlib.sha1(BROWSER).digest())}", f"sha-256 {colon_hex(hashlib.sha256(OTHER).digest())}"),
             True, False, True),
            (offer(f"sha-1 {colon_hex(hashlib.sha1(BROWSER).digest())}"), False, False, False),
            (offer(), False, False, False),
            (offer(f"sha-256 {sha256[:-3]}"), False, False, False),
            (offer(f"sha-256 {sha256}:00"), False, False, False),
            (offer(f"sha-256 {sha256} "), False, False, False),
            (offer(f"sha-256 {sha256.replace(':', '', 1)}"), False, False, False),
            (offer(f"sha-256 {sha256[:-1]}G"), False, False, False),
            (offer(f"sha-256{sha256}"), False, False, False),
            (offer(f"sha-256 {sha256}").replace("\r\n", "\n"), True, True, False),
        ]
        checks = []
        for sdp, taken, browser, other in cases:
            checks.append(
                f"    assert(dtls_fingerprint_expect(peer, {c_string(sdp)}, {len(sdp.encode())}) == {int(taken)});\n"
                f"    assert(dtls_fingerprint_matches(peer, browser, sizeof(browser)) == {int(browser)});\n"
                f"    assert(dtls_fingerprint_matches(peer, other, sizeof(other)) == {int(other)});\n")
        sdp = offer(f"sha-256 {sha256}")
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "media_lib_os.h"
#include "dtls_fingerprint.h"
''' + MEDIA_LIB + f'''
static const unsigned char browser[] = {c_bytes(BROWSER)};
static const unsigned char other[] = {c_bytes(OTHER)};
static const char sdp[] = {c_string(sdp)};
int main(void)
{{
    int peers[6] = {{0}};
    const void *peer = &peers[0];
    assert(!dtls_fingerprint_matches(peer, browser, sizeof(browser)));
{"".join(checks)}
    /* An SDP without a fingerprint replaces the earlier one, which then no
     * longer matches; each peer connection has its own. */
    assert(dtls_fingerprint_expect(peer, sdp, sizeof(sdp) - 1));
    assert(!dtls_fingerprint_expect(peer, "v=0\\r\\n", 5));
    assert(!dtls_fingerprint_matches(peer, browser, sizeof(browser)));
    assert(dtls_fingerprint_expect(&peers[1], sdp, sizeof(sdp) - 1));
    assert(!dtls_fingerprint_matches(&peers[2], browser, sizeof(browser)));
    assert(dtls_fingerprint_matches(&peers[1], browser, sizeof(browser)));
    dtls_fingerprint_forget(&peers[1]);
    assert(!dtls_fingerprint_matches(&peers[1], browser, sizeof(browser)));
    /* Four peer connections at once; a fifth SDP is refused. */
    for (int i = 0; i < 4; ++i) assert(dtls_fingerprint_expect(&peers[i], sdp, sizeof(sdp) - 1));
    assert(!dtls_fingerprint_expect(&peers[4], sdp, sizeof(sdp) - 1));
    assert(!dtls_fingerprint_matches(&peers[4], browser, sizeof(browser)));
    dtls_fingerprint_forget(&peers[3]);
    assert(dtls_fingerprint_expect(&peers[4], sdp, sizeof(sdp) - 1));
    assert(dtls_fingerprint_matches(&peers[4], browser, sizeof(browser)));
    assert(!dtls_fingerprint_expect(NULL, sdp, sizeof(sdp) - 1));
    assert(!dtls_fingerprint_expect(&peers[5], NULL, 0));
    return 0;
}}
'''
        run_c(self, harness, [COMPONENT_ROOT / "src" / "dtls_fingerprint.c"])

    def test_the_handshake_fails_for_another_certificate_and_for_none(self) -> None:
        source = (COMPONENT_ROOT / "src" / "dtls_common.h").read_text()
        start = source.index("static int dtls_srtp_verify_peer(")
        verify = source[start:source.index("static int dtls_srtp_do_handshake(", start)]
        start = source.index("int dtls_srtp_handshake(dtls_srtp_t *dtls_srtp)")
        handshake = source[start:source.index("dtls_srtp_role_t dtls_srtp_get_role(", start)]
        sha256 = colon_hex(hashlib.sha256(BROWSER).digest())
        sdp = offer(f"sha-256 {sha256}")
        # The handshake of mbedTLS as far as these functions see it: the
        # verify callback for the peer's certificate, when there is one, then
        # the key export that creates the SRTP sessions.
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "media_lib_os.h"
#include "dtls_fingerprint.h"
#define TAG "DTLS"
#define ESP_LOGE(tag, ...) ((void)(tag), (void)sizeof(printf(__VA_ARGS__)))
#define ESP_LOGI(tag, ...) ((void)(tag), (void)sizeof(printf(__VA_ARGS__)))
#define MBEDTLS_X509_BADCERT_OTHER 0x0100
#define MBEDTLS_ERR_X509_FATAL_ERROR -0x3000
typedef struct { struct { unsigned char *p; size_t len; } raw; } mbedtls_x509_crt;
typedef struct { int resets; } mbedtls_ssl_context;
typedef struct { int profile; } mbedtls_dtls_srtp_info;
typedef int *srtp_t;
typedef enum { DTLS_SRTP_ROLE_CLIENT, DTLS_SRTP_ROLE_SERVER } dtls_srtp_role_t;
typedef enum { DTLS_SRTP_STATE_NONE, DTLS_SRTP_STATE_INIT, DTLS_SRTP_STATE_HANDSHAKE, DTLS_SRTP_STATE_CONNECTED } dtls_srtp_state_t;
typedef struct {
    void *ctx; mbedtls_ssl_context ssl; dtls_srtp_role_t role; dtls_srtp_state_t state;
    srtp_t srtp_in, srtp_out; char remote_fingerprint[160];
} dtls_srtp_t;
''' + MEDIA_LIB + f'''
static unsigned char browser[] = {c_bytes(BROWSER)};
static unsigned char other[] = {c_bytes(OTHER)};
static const char sdp[] = {c_string(sdp)};
''' + r'''
static int sessions, deallocs;
static mbedtls_x509_crt *presented;
static void dtls_srtp_x509_digest(const mbedtls_x509_crt *crt, char *buf) { snprintf(buf, 160, "%zu", crt->raw.len); }
static void srtp_dealloc(srtp_t session) { assert(session); ++deallocs; }
static void mbedtls_ssl_session_reset(mbedtls_ssl_context *ssl) { ++ssl->resets; }
static const char *mbedtls_ssl_get_ciphersuite(const mbedtls_ssl_context *ssl) { (void)ssl; return "suite"; }
static void mbedtls_ssl_get_dtls_srtp_negotiation_result(const mbedtls_ssl_context *ssl, mbedtls_dtls_srtp_info *info) { (void)ssl; info->profile = 0; }
''' + verify + r'''
static int handshake(dtls_srtp_t *dtls_srtp)
{
    dtls_srtp->remote_fingerprint[0] = '\0';
    uint32_t flags = 0;
    if (presented) {
        int ret = dtls_srtp_verify_peer(dtls_srtp, presented, 0, &flags);
        if (ret != 0) return ret;
    }
    static int in, out;
    dtls_srtp->srtp_in = &in; dtls_srtp->srtp_out = &out; ++sessions;
    dtls_srtp->state = DTLS_SRTP_STATE_CONNECTED;
    return 0;
}
static int dtls_srtp_handshake_server(dtls_srtp_t *dtls_srtp) { return handshake(dtls_srtp); }
static int dtls_srtp_handshake_client(dtls_srtp_t *dtls_srtp) { return handshake(dtls_srtp) ? -1 : 0; }
''' + handshake + r'''
int main(void)
{
    int connection = 0;
    mbedtls_x509_crt browser_certificate = {{browser, sizeof(browser)}};
    mbedtls_x509_crt other_certificate = {{other, sizeof(other)}};
    assert(dtls_fingerprint_expect(&connection, sdp, sizeof(sdp) - 1));
    for (int role = 0; role < 2; ++role) {
        dtls_srtp_t dtls = {.ctx = &connection, .role = role, .state = DTLS_SRTP_STATE_INIT};
        sessions = deallocs = 0;
        presented = &other_certificate;
        assert(dtls_srtp_handshake(&dtls) != 0);
        assert(sessions == 0 && dtls.state == DTLS_SRTP_STATE_INIT && !dtls.remote_fingerprint[0]);
        presented = NULL;
        assert(dtls_srtp_handshake(&dtls) == -1);
        assert(sessions == 1 && deallocs == 2 && !dtls.srtp_in && !dtls.srtp_out);
        assert(dtls.state == DTLS_SRTP_STATE_INIT && dtls.ssl.resets == 1);
        presented = &browser_certificate;
        assert(dtls_srtp_handshake(&dtls) == 0);
        assert(dtls.state == DTLS_SRTP_STATE_CONNECTED && dtls.srtp_in && dtls.srtp_out);
        assert(!strcmp(dtls.remote_fingerprint, "200"));
        /* Another peer connection's SDP names nothing for this one. */
        dtls_srtp_t stranger = {.ctx = &dtls, .role = role, .state = DTLS_SRTP_STATE_INIT};
        assert(dtls_srtp_handshake(&stranger) != 0 && stranger.state == DTLS_SRTP_STATE_INIT);
    }
    return 0;
}
'''
        run_c(self, harness, [COMPONENT_ROOT / "src" / "dtls_fingerprint.c"])

    def test_both_roles_request_the_peer_certificate(self) -> None:
        source = (COMPONENT_ROOT / "src" / "dtls_srtp_v6.c").read_text()
        init = source[source.index("dtls_srtp_t *dtls_srtp_init("):source.index("void dtls_srtp_deinit(")]
        reset = source[source.index("void dtls_srtp_reset_session("):source.index("void utils_get_hmac_sha1(")]
        for function in (init, reset):
            server = function[function.index("MBEDTLS_SSL_IS_SERVER"):function.index("} else {")]
            self.assertIn("mbedtls_ssl_conf_authmode(&dtls_srtp->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);", server)
        handshake = (COMPONENT_ROOT / "src" / "dtls_common.h").read_text()
        handshake = handshake[handshake.index("static int dtls_srtp_do_handshake("):handshake.index("static int dtls_srtp_handshake_server(")]
        self.assertIn("mbedtls_ssl_set_verify(&dtls_srtp->ssl, dtls_srtp_verify_peer, dtls_srtp);", handshake)


if __name__ == "__main__":
    unittest.main()

/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "http_candidate_policy.h"

static webrtc_http_ipv4_t ipv4(const char *text)
{
    webrtc_http_ipv4_t address;
    assert(webrtc_http_ipv4_parse(text, &address));
    return address;
}

static webrtc_http_candidate_policy_t local_policy(void)
{
    webrtc_http_candidate_policy_t policy;
    webrtc_http_ipv4_t owner = ipv4("192.168.50.1");
    webrtc_http_ipv4_t device = ipv4("192.168.50.2");
    webrtc_http_candidate_policy_begin(&policy, 0x1234U, &owner, &device);
    assert(policy.owner_bound);
    assert(policy.rewrite_enabled);
    return policy;
}

static webrtc_http_candidate_result_t rewrite(
    webrtc_http_candidate_policy_t *policy, const char *candidate,
    char *output, size_t output_size)
{
    return webrtc_http_candidate_policy_rewrite(
        policy, 0x1234U, candidate, strlen(candidate), output, output_size);
}

static void test_ipv4_and_session_ownership(void)
{
    webrtc_http_ipv4_t address = ipv4("192.168.50.1");
    char formatted[16];
    assert(webrtc_http_ipv4_format(&address, formatted, sizeof(formatted)));
    assert(strcmp(formatted, "192.168.50.1") == 0);
    assert(!webrtc_http_ipv4_parse("192.168.50", &address));
    assert(!webrtc_http_ipv4_parse("192.168.50.256", &address));
    assert(!webrtc_http_ipv4_parse("192.168.50.1x", &address));
    assert(!webrtc_http_ipv4_parse("", &address));

    webrtc_http_candidate_policy_t policy = local_policy();
    webrtc_http_ipv4_t owner = ipv4("192.168.50.1");
    webrtc_http_ipv4_t other = ipv4("192.168.50.3");
    assert(webrtc_http_candidate_policy_authorize(
               &policy, 0x1234U, &owner) == WEBRTC_HTTP_OWNER_ALLOW);
    assert(webrtc_http_candidate_policy_authorize(
               &policy, 0x1234U, NULL) == WEBRTC_HTTP_OWNER_PEER_REQUIRED);
    assert(webrtc_http_candidate_policy_authorize(
               &policy, 0x1234U, &other) == WEBRTC_HTTP_OWNER_PEER_MISMATCH);
    assert(webrtc_http_candidate_policy_authorize(
               &policy, 0x9999U, &other) == WEBRTC_HTTP_OWNER_ALLOW);

    webrtc_http_candidate_policy_clear(&policy, 0x9999U);
    assert(policy.active);
    webrtc_http_candidate_policy_clear(&policy, 0x1234U);
    assert(!policy.active);
}

static void test_authoritative_ipv4_and_mdns_rewrite(void)
{
    static const char ipv4_candidate[] =
        "candidate:842163049 1 udp 1677734911 100.72.109.4 58483 typ host "
        "generation 0 ufrag abc network-cost 999";
    static const char expected[] =
        "candidate:842163049 1 udp 1677734911 192.168.50.1 58483 typ host "
        "generation 0 ufrag abc network-cost 999";
    char output[WEBRTC_HTTP_CANDIDATE_REWRITTEN_MAX];
    webrtc_http_candidate_policy_t policy = local_policy();
    assert(rewrite(&policy, ipv4_candidate, output, sizeof(output)) ==
           WEBRTC_HTTP_CANDIDATE_OK);
    assert(strcmp(output, expected) == 0);

    static const char mdns_candidate[] =
        "candidate:abcDEF/12 1 UDP 2122260223 "
        "9218d6f0-3ab4-41c0-a171-67e12a8d203f.local 49152 typ HOST";
    assert(rewrite(&policy, mdns_candidate, output, sizeof(output)) ==
           WEBRTC_HTTP_CANDIDATE_OK);
    assert(strcmp(output,
                  "candidate:abcDEF/12 1 UDP 2122260223 192.168.50.1 "
                  "49152 typ HOST") == 0);
}

static void assert_invalid(const char *candidate)
{
    char output[WEBRTC_HTTP_CANDIDATE_REWRITTEN_MAX];
    webrtc_http_candidate_policy_t policy = local_policy();
    assert(rewrite(&policy, candidate, output, sizeof(output)) ==
           WEBRTC_HTTP_CANDIDATE_INVALID);
    assert(policy.candidate_count == 0U);
}

static void test_candidate_validation(void)
{
    assert_invalid("candidate:f 2 udp 1 192.168.50.1 5000 typ host");
    assert_invalid("candidate:f 1 tcp 1 192.168.50.1 5000 typ host");
    assert_invalid("candidate:f 1 udp 1 192.168.50.1 5000 typ srflx");
    assert_invalid("candidate:f 1 udp 1 192.168.50.1 5000 typ relay");
    assert_invalid("candidate:f 1 udp 1 2001:db8::1 5000 typ host");
    assert_invalid("candidate:f 1 udp 1 evil.example 5000 typ host");
    assert_invalid("candidate:f 1 udp 1 -evil.local 5000 typ host");
    assert_invalid("candidate:f 1 udp 1 evil..local 5000 typ host");
    assert_invalid("candidate:f 1 udp 1 evil.local 0 typ host");
    assert_invalid("candidate:f 1 udp 1 evil.local 65536 typ host");
    assert_invalid("candidate:f 1 udp 0 192.168.50.1 5000 typ host");
    assert_invalid("candidate:f 1 udp bad 192.168.50.1 5000 typ host");
    assert_invalid("candidate:f 1 udp 1 192.168.50.1 5000 typ host\r\nx:y");
    assert_invalid(" candidate:f 1 udp 1 192.168.50.1 5000 typ host");
}

static void test_policy_and_resource_limits(void)
{
    static const char candidate[] =
        "candidate:f 1 udp 1 100.72.109.4 58483 typ host";
    char output[WEBRTC_HTTP_CANDIDATE_REWRITTEN_MAX];
    webrtc_http_candidate_policy_t policy = local_policy();

    assert(webrtc_http_candidate_policy_rewrite(
               &policy, 0x9999U, candidate, strlen(candidate), output,
               sizeof(output)) == WEBRTC_HTTP_CANDIDATE_POLICY_UNAVAILABLE);
    assert(rewrite(&policy, candidate, output, 4U) ==
           WEBRTC_HTTP_CANDIDATE_OUTPUT_TOO_SMALL);
    assert(policy.candidate_count == 0U);

    assert(rewrite(&policy, candidate, output, sizeof(output)) ==
           WEBRTC_HTTP_CANDIDATE_OK);
    assert(rewrite(&policy, candidate, output, sizeof(output)) ==
           WEBRTC_HTTP_CANDIDATE_OK);
    assert(policy.candidate_count == 1U);

    policy = local_policy();
    char unique_candidate[96];
    for (unsigned i = 0; i < WEBRTC_HTTP_CANDIDATE_LIMIT; ++i) {
        int written = snprintf(
            unique_candidate, sizeof(unique_candidate),
            "candidate:f 1 udp 1 100.72.109.4 %u typ host", 5000U + i);
        assert(written > 0 && (size_t)written < sizeof(unique_candidate));
        assert(rewrite(&policy, unique_candidate, output, sizeof(output)) ==
               WEBRTC_HTTP_CANDIDATE_OK);
    }
    assert(rewrite(
               &policy,
               "candidate:f 1 udp 1 100.72.109.4 6000 typ host",
               output, sizeof(output)) ==
           WEBRTC_HTTP_CANDIDATE_LIMIT_REACHED);

    char oversized[WEBRTC_HTTP_CANDIDATE_MAX + 2U];
    memset(oversized, 'x', sizeof(oversized));
    policy = local_policy();
    assert(webrtc_http_candidate_policy_rewrite(
               &policy, 0x1234U, oversized, sizeof(oversized), output,
               sizeof(output)) == WEBRTC_HTTP_CANDIDATE_TOO_LONG);

    webrtc_http_ipv4_t owner = ipv4("100.72.109.4");
    webrtc_http_ipv4_t device = ipv4("192.168.50.2");
    webrtc_http_candidate_policy_begin(&policy, 0x1234U, &owner, &device);
    assert(policy.owner_bound);
    assert(!policy.rewrite_enabled);
    assert(rewrite(&policy, candidate, output, sizeof(output)) ==
           WEBRTC_HTTP_CANDIDATE_POLICY_UNAVAILABLE);

    /* A browser on another private subnet (a routed VLAN) or on the
     * tailnet or the device's own /24 is rewritten to its own address; a
     * public address elsewhere or a missing device address is not. */
    const struct { const char *owner, *device; bool rewrite; } cases[] = {
        {"192.168.1.232", "192.168.20.65", true},
        {"10.0.4.7", "192.168.20.65", true},
        {"172.20.0.9", "172.31.255.1", true},
        {"169.254.10.1", "169.254.20.2", true},
        {"172.32.0.1", "192.168.20.65", false},
        {"8.8.8.8", "192.168.20.65", false},
        {"192.168.1.232", "203.0.113.5", false},
        {"100.64.1.20", "100.64.1.7", true},
        {"100.64.1.20", "100.64.2.7", true},
        {"203.0.113.9", "203.0.113.5", true},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        owner = ipv4(cases[i].owner);
        device = ipv4(cases[i].device);
        webrtc_http_candidate_policy_begin(&policy, 0x1234U, &owner, &device);
        assert(policy.rewrite_enabled == cases[i].rewrite);
        if (cases[i].rewrite) {
            assert(rewrite(&policy, candidate, output, sizeof(output)) == WEBRTC_HTTP_CANDIDATE_OK);
            assert(strstr(output, cases[i].owner) != NULL);
        }
    }
    webrtc_http_candidate_policy_begin(&policy, 0x1234U, &owner, NULL);
    assert(!policy.rewrite_enabled);
}

static void test_embedded_offer_candidates(void)
{
    webrtc_http_candidate_policy_t policy = local_policy();
    const char *sdp = "v=0\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:0\r\n"
        "a=candidate:1 1 udp 100 hidden.local 1234 typ host\r\n"
        "a=candidate:2 1 tcp 100 192.168.50.99 9 typ host\r\n"
        "a=candidate:3 1 udp 100 ::1 1235 typ host\r\n"
        "a=end-of-candidates";
    const char *expected = "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:0\r\n"
        "a=candidate:1 1 udp 100 192.168.50.1 1234 typ host\r\n"
        "a=end-of-candidates\r\n";
    char output[1024];
    assert(webrtc_http_candidate_policy_rewrite_sdp(
        &policy, 0x1234U, sdp, strlen(sdp), output, sizeof(output)) == WEBRTC_HTTP_CANDIDATE_OK);
    assert(strcmp(output, expected) == 0);
    assert(policy.candidate_count == 1U);
    assert(webrtc_http_candidate_policy_rewrite_sdp(
        &policy, 0x1234U, sdp, strlen(sdp), output, strlen(expected) + 1U) == WEBRTC_HTTP_CANDIDATE_OK);
    assert(webrtc_http_candidate_policy_rewrite_sdp(
        &policy, 0x1234U, sdp, strlen(sdp), output, strlen(expected)) == WEBRTC_HTTP_CANDIDATE_OUTPUT_TOO_SMALL);
    assert(webrtc_http_candidate_policy_rewrite_sdp(
        &policy, 0x5678U, sdp, strlen(sdp), output, sizeof(output)) == WEBRTC_HTTP_CANDIDATE_OK);
    assert(strcmp(output, "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:0\r\n"
                          "a=end-of-candidates\r\n") == 0);
}

static void test_embedded_candidate_limits_preserve_offer(void)
{
    char sdp[4096] = "v=0\r\n";
    size_t used = strlen(sdp);
    for (unsigned i = 0; i <= WEBRTC_HTTP_CANDIDATE_LIMIT; ++i) {
        int n = snprintf(sdp + used, sizeof(sdp) - used,
            "a=candidate:f 1 udp 1 hidden.local %u typ host\r\n", 5000U + i);
        assert(n > 0 && (size_t)n < sizeof(sdp) - used);
        used += (size_t)n;
    }
    /* A rejected oversized line must not consume a slot or hide later SDP. */
    memcpy(sdp + used, "a=candidate:", 12U);
    used += 12U;
    memset(sdp + used, 'x', WEBRTC_HTTP_CANDIDATE_MAX);
    used += WEBRTC_HTTP_CANDIDATE_MAX;
    const char *tail = "\r\na=mid:0\r\n";
    strcpy(sdp + used, tail);
    used += strlen(tail);

    webrtc_http_candidate_policy_t policy = local_policy();
    char output[4096];
    assert(webrtc_http_candidate_policy_rewrite_sdp(
        &policy, 0x1234U, sdp, used, output, sizeof(output)) == WEBRTC_HTTP_CANDIDATE_OK);
    assert(policy.candidate_count == WEBRTC_HTTP_CANDIDATE_LIMIT);
    assert(strstr(output, "192.168.50.1 5000 typ host\r\n"));
    assert(strstr(output, "192.168.50.1 5015 typ host\r\n"));
    assert(!strstr(output, "5016 typ host"));
    assert(!strstr(output, "xxxxxxxx"));
    assert(strstr(output, "a=mid:0\r\n"));

    /* With no IPv4 policy (including before an offer), only candidates vanish. */
    memset(&policy, 0, sizeof(policy));
    assert(webrtc_http_candidate_policy_rewrite_sdp(
        &policy, 0x1234U, sdp, used, output, sizeof(output)) == WEBRTC_HTTP_CANDIDATE_OK);
    assert(strcmp(output, "v=0\r\na=mid:0\r\n") == 0);
}

static void test_tailnet_candidates(void)
{
    webrtc_http_candidate_policy_t policy, device_policy;
    webrtc_http_ipv4_t owner = ipv4("100.91.148.97");
    webrtc_http_ipv4_t device = ipv4("100.90.1.2");
    webrtc_http_candidate_policy_begin(&policy, 0x1234U, &owner, &device);
    char output[1024];
    const char *browser = "candidate:f 1 udp 1 browser.local 51000 typ host";
    assert(rewrite(&policy, browser, output, sizeof(output)) == WEBRTC_HTTP_CANDIDATE_OK);
    assert(strstr(output, "100.91.148.97 51000 typ host"));
    assert(webrtc_http_candidate_policy_for_device(&policy, &device_policy));
    const char *sdp = "v=0\r\na=candidate:f 1 udp 1 192.168.1.44 52000 typ host\r\na=mid:0\r\n";
    assert(webrtc_http_candidate_policy_rewrite_sdp(&device_policy, 0x1234U,
        sdp, strlen(sdp), output, sizeof(output)) == WEBRTC_HTTP_CANDIDATE_OK);
    assert(strstr(output, "100.90.1.2 52000 typ host\r\na=mid:0"));
    assert(!strstr(output, "192.168.1.44"));
    assert(policy.candidate_count == 1 && !memcmp(policy.owner.octet, owner.octet, 4));
    /* A trickled host candidate is rewritten; one that the policy does not
     * admit is skipped, and only a full output buffer fails. */
    const char *host = "a=candidate:f 1 udp 1 192.168.1.44 52001 typ host\r\n";
    assert(webrtc_http_candidate_policy_rewrite_device_signal(&device_policy, 0x1234U, false,
        host, strlen(host), output, sizeof(output)) == WEBRTC_HTTP_DEVICE_SIGNAL_SEND);
    assert(strcmp(output, "candidate:f 1 udp 1 100.90.1.2 52001 typ host") == 0);
    const char *srflx = "a=candidate:g 1 udp 1 203.0.113.5 52002 typ srflx raddr 0.0.0.0 rport 0\r\n";
    assert(webrtc_http_candidate_policy_rewrite_device_signal(&device_policy, 0x1234U, false,
        srflx, strlen(srflx), output, sizeof(output)) == WEBRTC_HTTP_DEVICE_SIGNAL_SKIP);
    assert(webrtc_http_candidate_policy_rewrite_device_signal(&device_policy, 0x1234U, false,
        host, strlen(host), output, 8) == WEBRTC_HTTP_DEVICE_SIGNAL_FAIL);
    assert(webrtc_http_candidate_policy_rewrite_device_signal(&device_policy, 0x1234U, true,
        sdp, strlen(sdp), output, 8) == WEBRTC_HTTP_DEVICE_SIGNAL_FAIL);
    device = ipv4("192.168.1.44");
    webrtc_http_candidate_policy_begin(&policy, 0x1234U, &owner, &device);
    assert(!webrtc_http_candidate_policy_for_device(&policy, &device_policy));
    owner = ipv4("100.128.1.1");
    device = ipv4("100.90.1.2");
    webrtc_http_candidate_policy_begin(&policy, 0x1234U, &owner, &device);
    assert(!policy.rewrite_enabled);
}

int main(void)
{
    test_ipv4_and_session_ownership();
    test_authoritative_ipv4_and_mdns_rewrite();
    test_candidate_validation();
    test_policy_and_resource_limits();
    test_embedded_offer_candidates();
    test_embedded_candidate_limits_preserve_offer();
    test_tailnet_candidates();
    puts("HTTP ICE candidate policy: OK");
    return 0;
}

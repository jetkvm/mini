#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jetkvm_cloud_signal.h"
#include "mbedtls/base64.h"
#include "upstream_fixtures.h"

static void assert_offer_empty(const jetkvm_cloud_signal_offer_t *offer)
{
    assert(offer->sdp.data == NULL && offer->sdp.length == 0);
    assert(offer->ip.data == NULL && offer->ip.length == 0);
    assert(offer->oidc_google.data == NULL &&
           offer->oidc_google.length == 0);
    assert(offer->ice_servers == NULL && offer->ice_server_count == 0);
    assert(!offer->has_ip);
    assert(!offer->has_oidc_google);
    assert(!offer->has_ice_servers);
}

static void assert_candidate_empty(
    const jetkvm_cloud_signal_candidate_t *candidate)
{
    assert(candidate->candidate.data == NULL &&
           candidate->candidate.length == 0);
    assert(candidate->sdp_mid.data == NULL && candidate->sdp_mid.length == 0);
    assert(candidate->username_fragment.data == NULL &&
           candidate->username_fragment.length == 0);
    assert(candidate->sdp_mline_index == 0);
    assert(!candidate->has_username_fragment);
}

static char *base64_encode(const char *plain)
{
    const size_t plain_length = strlen(plain);
    const size_t encoded_length = ((plain_length + 2) / 3) * 4;
    char *encoded = malloc(encoded_length + 1);
    assert(encoded != NULL);
    size_t actual_length = 0;
    assert(mbedtls_base64_encode((unsigned char *)encoded, encoded_length + 1,
                                 &actual_length,
                                 (const unsigned char *)plain,
                                 plain_length) == 0);
    assert(actual_length == encoded_length);
    encoded[encoded_length] = '\0';
    return encoded;
}

static char *make_offer(const char *session_json, const char *data_suffix)
{
    char *encoded = base64_encode(session_json);
    const char *prefix = "{\"type\":\"offer\",\"data\":{\"sd\":\"";
    const char *suffix = "}}";
    const size_t length = strlen(prefix) + strlen(encoded) + 1 +
                          strlen(data_suffix) + strlen(suffix);
    char *message = malloc(length + 1);
    assert(message != NULL);
    const int written = snprintf(message, length + 1, "%s%s\"%s%s", prefix,
                                 encoded, data_suffix, suffix);
    assert(written >= 0 && (size_t)written == length);
    free(encoded);
    return message;
}

static void expect_offer_error(const char *message,
                               jetkvm_cloud_signal_status_t expected)
{
    jetkvm_cloud_signal_offer_t offer = { 0 };
    assert(jetkvm_cloud_signal_parse_offer(
               (const uint8_t *)message, strlen(message), &offer) == expected);
    assert_offer_empty(&offer);
    jetkvm_cloud_signal_offer_free(&offer);
}

static void expect_candidate_error(const char *message,
                                   jetkvm_cloud_signal_status_t expected)
{
    jetkvm_cloud_signal_candidate_t candidate = { 0 };
    assert(jetkvm_cloud_signal_parse_candidate(
               (const uint8_t *)message, strlen(message), &candidate) ==
           expected);
    assert_candidate_empty(&candidate);
    jetkvm_cloud_signal_candidate_free(&candidate);
}

static void test_upstream_offer_fixture(void)
{
    jetkvm_cloud_signal_offer_t offer = { 0 };
    assert(jetkvm_cloud_signal_parse_offer(
               (const uint8_t *)UPSTREAM_OFFER_ENVELOPE,
               strlen(UPSTREAM_OFFER_ENVELOPE), &offer) ==
           JETKVM_CLOUD_SIGNAL_OK);
    assert(offer.sdp.length == strlen(UPSTREAM_OFFER_SDP));
    assert(strcmp(offer.sdp.data, UPSTREAM_OFFER_SDP) == 0);
    assert(offer.has_ip && strcmp(offer.ip.data, "203.0.113.7") == 0);
    assert(offer.has_oidc_google);
    assert(strcmp(offer.oidc_google.data, "header.payload.signature") == 0);
    assert(offer.has_ice_servers && offer.ice_server_count == 2);
    assert(strcmp(offer.ice_servers[0].data,
                  "stun:stun.jetkvm.com:3478") == 0);
    assert(strcmp(offer.ice_servers[1].data,
                  "turn:turn.jetkvm.com:3478?transport=udp") == 0);
    jetkvm_cloud_signal_offer_free(&offer);
    assert_offer_empty(&offer);

    char *minimal = make_offer(
        "{\"type\":\"offer\",\"sdp\":\"v=0\\r\\n\"}", "");
    assert(jetkvm_cloud_signal_parse_offer((const uint8_t *)minimal,
                                           strlen(minimal), &offer) ==
           JETKVM_CLOUD_SIGNAL_OK);
    assert(strcmp(offer.sdp.data, "v=0\r\n") == 0);
    assert(!offer.has_ip && !offer.has_oidc_google &&
           !offer.has_ice_servers);
    jetkvm_cloud_signal_offer_free(&offer);
    free(minimal);

    char *empty_ice = make_offer(
        "{\"type\":\"offer\",\"sdp\":\"v=0\"}",
        ",\"ip\":\"\",\"OidcGoogle\":\"\",\"iceServers\":[]");
    assert(jetkvm_cloud_signal_parse_offer((const uint8_t *)empty_ice,
                                           strlen(empty_ice), &offer) ==
           JETKVM_CLOUD_SIGNAL_OK);
    assert(offer.has_ip && offer.ip.length == 0);
    assert(offer.has_oidc_google && offer.oidc_google.length == 0);
    assert(offer.has_ice_servers && offer.ice_server_count == 0);
    jetkvm_cloud_signal_offer_free(&offer);
    free(empty_ice);
}

static void test_upstream_answer_fixture(void)
{
    char output[4096];
    size_t output_length = 99;
    assert(jetkvm_cloud_signal_encode_answer(
               UPSTREAM_ANSWER_SDP, strlen(UPSTREAM_ANSWER_SDP), output,
               sizeof(output), &output_length) == JETKVM_CLOUD_SIGNAL_OK);
    assert(output_length == strlen(UPSTREAM_ANSWER_ENVELOPE));
    assert(strcmp(output, UPSTREAM_ANSWER_ENVELOPE) == 0);

    char tiny[12] = "unchanged!!";
    char before[sizeof(tiny)];
    memcpy(before, tiny, sizeof(tiny));
    output_length = 123;
    assert(jetkvm_cloud_signal_encode_answer(
               UPSTREAM_ANSWER_SDP, strlen(UPSTREAM_ANSWER_SDP), tiny,
               sizeof(tiny), &output_length) ==
           JETKVM_CLOUD_SIGNAL_OUTPUT_TOO_SMALL);
    assert(output_length == 0);
    assert(memcmp(tiny, before, sizeof(tiny)) == 0);
}

static void test_upstream_candidate_fixture(void)
{
    jetkvm_cloud_signal_candidate_t candidate = { 0 };
    assert(jetkvm_cloud_signal_parse_candidate(
               (const uint8_t *)UPSTREAM_CANDIDATE_ENVELOPE,
               strlen(UPSTREAM_CANDIDATE_ENVELOPE), &candidate) ==
           JETKVM_CLOUD_SIGNAL_OK);
    assert(strcmp(candidate.candidate.data, UPSTREAM_CANDIDATE_VALUE) == 0);
    assert(strcmp(candidate.sdp_mid.data, "0") == 0);
    assert(candidate.sdp_mline_index == 0);
    assert(candidate.has_username_fragment);
    assert(strcmp(candidate.username_fragment.data, "p4Kvm") == 0);

    const jetkvm_cloud_signal_candidate_view_t view = {
        .candidate = candidate.candidate.data,
        .candidate_length = candidate.candidate.length,
        .sdp_mid = candidate.sdp_mid.data,
        .sdp_mid_length = candidate.sdp_mid.length,
        .sdp_mline_index = candidate.sdp_mline_index,
        .username_fragment = candidate.username_fragment.data,
        .username_fragment_length = candidate.username_fragment.length,
        .has_username_fragment = true,
    };
    char output[4096];
    size_t output_length = 0;
    assert(jetkvm_cloud_signal_encode_candidate(
               &view, output, sizeof(output), &output_length) ==
           JETKVM_CLOUD_SIGNAL_OK);
    assert(output_length == strlen(UPSTREAM_CANDIDATE_ENVELOPE));
    assert(strcmp(output, UPSTREAM_CANDIDATE_ENVELOPE) == 0);
    jetkvm_cloud_signal_candidate_free(&candidate);
    assert_candidate_empty(&candidate);

    const char no_username[] =
        "{\"type\":\"new-ice-candidate\",\"data\":{" 
        "\"candidate\":\"candidate:1 1 udp 1 192.0.2.1 9 typ host\"," 
        "\"sdpMid\":\"video\",\"sdpMLineIndex\":12}}";
    assert(jetkvm_cloud_signal_parse_candidate(
               (const uint8_t *)no_username, strlen(no_username),
               &candidate) == JETKVM_CLOUD_SIGNAL_OK);
    assert(!candidate.has_username_fragment);
    assert(candidate.sdp_mline_index == 12);
    jetkvm_cloud_signal_candidate_free(&candidate);

    const jetkvm_cloud_signal_candidate_view_t upstream_device_view = {
        .candidate = "candidate:1 1 udp 1 192.0.2.1 9 typ host",
        .candidate_length =
            strlen("candidate:1 1 udp 1 192.0.2.1 9 typ host"),
        .sdp_mid = "0",
        .sdp_mid_length = 1,
        .sdp_mline_index = 0,
    };
    assert(jetkvm_cloud_signal_encode_candidate(
               &upstream_device_view, output, sizeof(output),
               &output_length) == JETKVM_CLOUD_SIGNAL_OK);
    assert(strcmp(output, UPSTREAM_DEVICE_CANDIDATE_WITH_NULL_USERNAME) == 0);
    assert(jetkvm_cloud_signal_parse_candidate(
               (const uint8_t *)output, output_length, &candidate) ==
           JETKVM_CLOUD_SIGNAL_OK);
    assert(!candidate.has_username_fragment);
    jetkvm_cloud_signal_candidate_free(&candidate);

    const char odd_candidate[] = "candidate:quote\\\" slash\\\\ ok";
    const jetkvm_cloud_signal_candidate_view_t escaped_view = {
        .candidate = odd_candidate,
        .candidate_length = strlen(odd_candidate),
        .sdp_mid = "video/0",
        .sdp_mid_length = strlen("video/0"),
        .sdp_mline_index = 7,
    };
    assert(jetkvm_cloud_signal_encode_candidate(
               &escaped_view, output, sizeof(output), &output_length) ==
           JETKVM_CLOUD_SIGNAL_OK);
    assert(jetkvm_cloud_signal_parse_candidate(
               (const uint8_t *)output, output_length, &candidate) ==
           JETKVM_CLOUD_SIGNAL_OK);
    assert(strcmp(candidate.candidate.data, odd_candidate) == 0);
    assert(strcmp(candidate.sdp_mid.data, "video/0") == 0);
    jetkvm_cloud_signal_candidate_free(&candidate);
}

static void test_malformed_offer_envelopes(void)
{
    expect_offer_error("not-json", JETKVM_CLOUD_SIGNAL_INVALID_JSON);
    expect_offer_error("[]", JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    expect_offer_error("{\"type\":\"offer\"}",
                       JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    expect_offer_error("{\"type\":\"answer\",\"data\":{}}",
                       JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    expect_offer_error(
        "{\"type\":\"offer\",\"type\":\"offer\",\"data\":{}}",
        JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    expect_offer_error("{\"type\":\"offer\",\"data\":\"bad\"}",
                       JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    expect_offer_error("{\"type\":\"offer\",\"data\":{}}",
                       JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    expect_offer_error(
        "{\"type\":\"offer\",\"data\":{\"sd\":7}}",
        JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    expect_offer_error(
        "{\"type\":\"offer\",\"data\":{\"sd\":\"A===\"}}",
        JETKVM_CLOUD_SIGNAL_INVALID_BASE64);
    expect_offer_error(
        "{\"type\":\"offer\",\"data\":{\"sd\":\"TR==\"}}",
        JETKVM_CLOUD_SIGNAL_INVALID_BASE64);
    expect_offer_error(
        "{\"type\":\"offer\",\"data\":{\"sd\":\"bm90LWpzb24=\"}}",
        JETKVM_CLOUD_SIGNAL_INVALID_SESSION_DESCRIPTION);

    char *wrong_type = make_offer(
        "{\"type\":\"answer\",\"sdp\":\"v=0\"}", "");
    expect_offer_error(wrong_type,
                       JETKVM_CLOUD_SIGNAL_INVALID_SESSION_DESCRIPTION);
    free(wrong_type);
    char *missing_sdp = make_offer("{\"type\":\"offer\"}", "");
    expect_offer_error(missing_sdp,
                       JETKVM_CLOUD_SIGNAL_INVALID_SESSION_DESCRIPTION);
    free(missing_sdp);
    char *number_sdp = make_offer(
        "{\"type\":\"offer\",\"sdp\":1}", "");
    expect_offer_error(number_sdp,
                       JETKVM_CLOUD_SIGNAL_INVALID_SESSION_DESCRIPTION);
    free(number_sdp);
    char *duplicate_session_type = make_offer(
        "{\"type\":\"offer\",\"type\":\"offer\",\"sdp\":\"v=0\"}",
        "");
    expect_offer_error(duplicate_session_type,
                       JETKVM_CLOUD_SIGNAL_INVALID_SESSION_DESCRIPTION);
    free(duplicate_session_type);

    char *bad_ip = make_offer(
        "{\"type\":\"offer\",\"sdp\":\"v=0\"}", ",\"ip\":5");
    expect_offer_error(bad_ip, JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    free(bad_ip);
    char *bad_oidc = make_offer(
        "{\"type\":\"offer\",\"sdp\":\"v=0\"}",
        ",\"OidcGoogle\":{}");
    expect_offer_error(bad_oidc, JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    free(bad_oidc);
    char *bad_ice_type = make_offer(
        "{\"type\":\"offer\",\"sdp\":\"v=0\"}",
        ",\"iceServers\":{}");
    expect_offer_error(bad_ice_type,
                       JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    free(bad_ice_type);
    char *bad_ice_entry = make_offer(
        "{\"type\":\"offer\",\"sdp\":\"v=0\"}",
        ",\"iceServers\":[\"stun:good\",4]");
    expect_offer_error(bad_ice_entry,
                       JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    free(bad_ice_entry);
    char *empty_ice_entry = make_offer(
        "{\"type\":\"offer\",\"sdp\":\"v=0\"}",
        ",\"iceServers\":[\"\"]");
    expect_offer_error(empty_ice_entry, JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED);
    free(empty_ice_entry);

    char many_ice_suffix[512] = ",\"iceServers\":[";
    size_t used = strlen(many_ice_suffix);
    for (size_t i = 0; i < JETKVM_CLOUD_SIGNAL_MAX_ICE_SERVERS + 1; ++i) {
        const int written = snprintf(many_ice_suffix + used,
                                     sizeof(many_ice_suffix) - used,
                                     "%s\"stun:%zu\"", i == 0 ? "" : ",",
                                     i);
        assert(written > 0 && (size_t)written < sizeof(many_ice_suffix) - used);
        used += (size_t)written;
    }
    assert(used + 2 <= sizeof(many_ice_suffix));
    memcpy(many_ice_suffix + used, "]", 2);
    char *many_ice = make_offer(
        "{\"type\":\"offer\",\"sdp\":\"v=0\"}", many_ice_suffix);
    expect_offer_error(many_ice, JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED);
    free(many_ice);

    const char unicode_nul[] =
        "{\"type\":\"offer\",\"data\":{\"sd\":\"abc\"," 
        "\"ip\":\"bad\\u0000tail\"}}";
    expect_offer_error(unicode_nul, JETKVM_CLOUD_SIGNAL_INVALID_JSON);

    const char too_deep[] =
        "{\"type\":\"offer\",\"data\":{\"unused\":[[[[[[[0]]]]]]]}}";
    expect_offer_error(too_deep, JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED);

    uint8_t invalid_utf8[] = { '{', '"', 'x', '"', ':', '"', 0xc0, 0x80,
                               '"', '}' };
    jetkvm_cloud_signal_offer_t offer = { 0 };
    assert(jetkvm_cloud_signal_parse_offer(invalid_utf8,
                                           sizeof(invalid_utf8), &offer) ==
           JETKVM_CLOUD_SIGNAL_INVALID_JSON);
    assert_offer_empty(&offer);

    uint8_t embedded_nul[] = { '{', '}', 0, 'x' };
    assert(jetkvm_cloud_signal_parse_offer(embedded_nul,
                                           sizeof(embedded_nul), &offer) ==
           JETKVM_CLOUD_SIGNAL_INVALID_JSON);
    assert_offer_empty(&offer);

    char *oversized_message = malloc(JETKVM_CLOUD_SIGNAL_MAX_MESSAGE_SIZE + 2);
    assert(oversized_message != NULL);
    memset(oversized_message, ' ', JETKVM_CLOUD_SIGNAL_MAX_MESSAGE_SIZE + 1);
    oversized_message[JETKVM_CLOUD_SIGNAL_MAX_MESSAGE_SIZE + 1] = '\0';
    assert(jetkvm_cloud_signal_parse_offer(
               (const uint8_t *)oversized_message,
               JETKVM_CLOUD_SIGNAL_MAX_MESSAGE_SIZE + 1, &offer) ==
           JETKVM_CLOUD_SIGNAL_MESSAGE_TOO_LARGE);
    assert_offer_empty(&offer);
    free(oversized_message);

    const size_t prefix_length = strlen("{\"type\":\"offer\",\"sdp\":\"");
    const char *session_suffix = "\"}";
    const size_t huge_session_length = prefix_length +
                                       JETKVM_CLOUD_SIGNAL_MAX_SDP_SIZE + 1 +
                                       strlen(session_suffix);
    char *huge_session = malloc(huge_session_length + 1);
    assert(huge_session != NULL);
    memcpy(huge_session, "{\"type\":\"offer\",\"sdp\":\"", prefix_length);
    memset(huge_session + prefix_length, 'x',
           JETKVM_CLOUD_SIGNAL_MAX_SDP_SIZE + 1);
    memcpy(huge_session + prefix_length + JETKVM_CLOUD_SIGNAL_MAX_SDP_SIZE + 1,
           session_suffix, strlen(session_suffix) + 1);
    char *huge_offer = make_offer(huge_session, "");
    expect_offer_error(huge_offer, JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED);
    free(huge_offer);
    free(huge_session);
}

static void test_malformed_candidates(void)
{
    expect_candidate_error("not-json", JETKVM_CLOUD_SIGNAL_INVALID_JSON);
    expect_candidate_error(
        "{\"type\":\"offer\",\"data\":{}}",
        JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    expect_candidate_error(
        "{\"type\":\"new-ice-candidate\",\"data\":{}}",
        JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    expect_candidate_error(
        "{\"type\":\"new-ice-candidate\",\"data\":{" 
        "\"candidate\":\"c\",\"sdpMid\":\"0\"," 
        "\"sdpMLineIndex\":1.5}}",
        JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    expect_candidate_error(
        "{\"type\":\"new-ice-candidate\",\"data\":{" 
        "\"candidate\":\"c\",\"sdpMid\":\"0\"," 
        "\"sdpMLineIndex\":-1}}",
        JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    expect_candidate_error(
        "{\"type\":\"new-ice-candidate\",\"data\":{" 
        "\"candidate\":\"c\",\"sdpMid\":\"0\"," 
        "\"sdpMLineIndex\":65536}}",
        JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    expect_candidate_error(
        "{\"type\":\"new-ice-candidate\",\"data\":{" 
        "\"candidate\":7,\"sdpMid\":\"0\",\"sdpMLineIndex\":0}}",
        JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    expect_candidate_error(
        "{\"type\":\"new-ice-candidate\",\"data\":{" 
        "\"candidate\":\"c\",\"sdpMid\":7,\"sdpMLineIndex\":0}}",
        JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    expect_candidate_error(
        "{\"type\":\"new-ice-candidate\",\"data\":{" 
        "\"candidate\":\"c\",\"sdpMid\":\"0\"," 
        "\"sdpMLineIndex\":0,\"usernameFragment\":7}}",
        JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    expect_candidate_error(
        "{\"type\":\"new-ice-candidate\",\"data\":{" 
        "\"candidate\":\"c\",\"candidate\":\"d\"," 
        "\"sdpMid\":\"0\",\"sdpMLineIndex\":0}}",
        JETKVM_CLOUD_SIGNAL_INVALID_ENVELOPE);
    expect_candidate_error(
        "{\"type\":\"new-ice-candidate\",\"data\":{" 
        "\"candidate\":\"\",\"sdpMid\":\"0\",\"sdpMLineIndex\":0}}",
        JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED);
    expect_candidate_error(
        "{\"type\":\"new-ice-candidate\",\"data\":{" 
        "\"candidate\":\"c\",\"sdpMid\":\"\",\"sdpMLineIndex\":0}}",
        JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED);
}

static void test_encode_validation(void)
{
    char output[4096] = "sentinel";
    size_t output_length = 7;
    assert(jetkvm_cloud_signal_encode_answer(
               NULL, 0, output, sizeof(output), &output_length) ==
           JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT);
    assert(output_length == 0 && strcmp(output, "sentinel") == 0);
    assert(jetkvm_cloud_signal_encode_answer(
               "", 0, output, sizeof(output), &output_length) ==
           JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT);
    const char embedded_nul[] = { 'a', 0, 'b' };
    assert(jetkvm_cloud_signal_encode_answer(
               embedded_nul, sizeof(embedded_nul), output, sizeof(output),
               &output_length) == JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT);
    const char invalid_utf8[] = { (char)0xc0, (char)0x80 };
    assert(jetkvm_cloud_signal_encode_answer(
               invalid_utf8, sizeof(invalid_utf8), output, sizeof(output),
               &output_length) == JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT);

    char *oversized_sdp = malloc(JETKVM_CLOUD_SIGNAL_MAX_SDP_SIZE + 2);
    assert(oversized_sdp != NULL);
    memset(oversized_sdp, 's', JETKVM_CLOUD_SIGNAL_MAX_SDP_SIZE + 1);
    oversized_sdp[JETKVM_CLOUD_SIGNAL_MAX_SDP_SIZE + 1] = '\0';
    assert(jetkvm_cloud_signal_encode_answer(
               oversized_sdp, JETKVM_CLOUD_SIGNAL_MAX_SDP_SIZE + 1, output,
               sizeof(output), &output_length) ==
           JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED);
    assert(output_length == 0 && strcmp(output, "sentinel") == 0);
    free(oversized_sdp);

    const jetkvm_cloud_signal_candidate_view_t missing = { 0 };
    assert(jetkvm_cloud_signal_encode_candidate(
               &missing, output, sizeof(output), &output_length) ==
           JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT);
    const jetkvm_cloud_signal_candidate_view_t contradictory_username = {
        .candidate = "c",
        .candidate_length = 1,
        .sdp_mid = "0",
        .sdp_mid_length = 1,
        .username_fragment = "ufrag",
        .username_fragment_length = 5,
        .has_username_fragment = false,
    };
    assert(jetkvm_cloud_signal_encode_candidate(
               &contradictory_username, output, sizeof(output),
               &output_length) == JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT);

    char *oversized_candidate =
        malloc(JETKVM_CLOUD_SIGNAL_MAX_CANDIDATE_SIZE + 2);
    assert(oversized_candidate != NULL);
    memset(oversized_candidate, 'c',
           JETKVM_CLOUD_SIGNAL_MAX_CANDIDATE_SIZE + 1);
    const jetkvm_cloud_signal_candidate_view_t too_long = {
        .candidate = oversized_candidate,
        .candidate_length = JETKVM_CLOUD_SIGNAL_MAX_CANDIDATE_SIZE + 1,
        .sdp_mid = "0",
        .sdp_mid_length = 1,
    };
    assert(jetkvm_cloud_signal_encode_candidate(
               &too_long, output, sizeof(output), &output_length) ==
           JETKVM_CLOUD_SIGNAL_LIMIT_EXCEEDED);
    free(oversized_candidate);

    char tiny[8] = "keepme";
    char before[sizeof(tiny)];
    memcpy(before, tiny, sizeof(tiny));
    const jetkvm_cloud_signal_candidate_view_t valid = {
        .candidate = "candidate:1 1 udp 1 192.0.2.1 9 typ host",
        .candidate_length =
            strlen("candidate:1 1 udp 1 192.0.2.1 9 typ host"),
        .sdp_mid = "0",
        .sdp_mid_length = 1,
        .sdp_mline_index = 0,
    };
    assert(jetkvm_cloud_signal_encode_candidate(
               &valid, tiny, sizeof(tiny), &output_length) ==
           JETKVM_CLOUD_SIGNAL_OUTPUT_TOO_SMALL);
    assert(output_length == 0 && memcmp(tiny, before, sizeof(tiny)) == 0);
}

static void test_argument_and_status_contract(void)
{
    jetkvm_cloud_signal_offer_t offer = { 0 };
    jetkvm_cloud_signal_candidate_t candidate = { 0 };
    assert(jetkvm_cloud_signal_parse_offer(NULL, 1, &offer) ==
           JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT);
    assert(jetkvm_cloud_signal_parse_offer((const uint8_t *)"{}", 2, NULL) ==
           JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT);
    assert(jetkvm_cloud_signal_parse_candidate(NULL, 1, &candidate) ==
           JETKVM_CLOUD_SIGNAL_INVALID_ARGUMENT);
    jetkvm_cloud_signal_offer_free(NULL);
    jetkvm_cloud_signal_candidate_free(NULL);

    for (int value = JETKVM_CLOUD_SIGNAL_OK;
         value <= JETKVM_CLOUD_SIGNAL_OUTPUT_TOO_SMALL; ++value) {
        assert(strcmp(jetkvm_cloud_signal_status_name(
                          (jetkvm_cloud_signal_status_t)value),
                      "unknown") != 0);
    }
    assert(strcmp(jetkvm_cloud_signal_status_name(
                      (jetkvm_cloud_signal_status_t)999),
                  "unknown") == 0);
}

static void test_truncation_and_mutation_cleanup(void)
{
    const char *offer_fixture = UPSTREAM_OFFER_ENVELOPE;
    const size_t offer_length = strlen(offer_fixture);
    char *mutation = malloc(offer_length + 1);
    assert(mutation != NULL);
    memcpy(mutation, offer_fixture, offer_length + 1);

    for (size_t length = 0; length < offer_length; ++length) {
        jetkvm_cloud_signal_offer_t offer = { 0 };
        const jetkvm_cloud_signal_status_t status =
            jetkvm_cloud_signal_parse_offer((const uint8_t *)offer_fixture,
                                            length, &offer);
        if (status == JETKVM_CLOUD_SIGNAL_OK) {
            jetkvm_cloud_signal_offer_free(&offer);
        } else {
            assert_offer_empty(&offer);
        }
    }
    for (size_t i = 0; i < offer_length; ++i) {
        const char original = mutation[i];
        mutation[i] = (char)((unsigned char)original ^ 0x5aU);
        jetkvm_cloud_signal_offer_t offer = { 0 };
        const jetkvm_cloud_signal_status_t status =
            jetkvm_cloud_signal_parse_offer((const uint8_t *)mutation,
                                            offer_length, &offer);
        if (status == JETKVM_CLOUD_SIGNAL_OK) {
            jetkvm_cloud_signal_offer_free(&offer);
        } else {
            assert_offer_empty(&offer);
        }
        mutation[i] = original;
    }
    free(mutation);

    const char *candidate_fixture = UPSTREAM_CANDIDATE_ENVELOPE;
    const size_t candidate_length = strlen(candidate_fixture);
    for (size_t length = 0; length < candidate_length; ++length) {
        jetkvm_cloud_signal_candidate_t candidate = { 0 };
        const jetkvm_cloud_signal_status_t status =
            jetkvm_cloud_signal_parse_candidate(
                (const uint8_t *)candidate_fixture, length, &candidate);
        if (status == JETKVM_CLOUD_SIGNAL_OK) {
            jetkvm_cloud_signal_candidate_free(&candidate);
        } else {
            assert_candidate_empty(&candidate);
        }
    }
}

int main(void)
{
    test_upstream_offer_fixture();
    test_upstream_answer_fixture();
    test_upstream_candidate_fixture();
    test_malformed_offer_envelopes();
    test_malformed_candidates();
    test_encode_validation();
    test_argument_and_status_contract();
    test_truncation_and_mutation_cleanup();
    puts("JetKVM cloud signaling wire codec vectors: OK");
    return 0;
}

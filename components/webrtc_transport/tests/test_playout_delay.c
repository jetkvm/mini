/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "playout_delay.h"

static const webrtc_playout_delay_allocator_t s_test_allocator = {
    .alloc = malloc,
    .free = free,
};

static int prepare_offer(const uint8_t *sdp, size_t size,
                         webrtc_playout_delay_offer_t *offer)
{
    return webrtc_playout_delay_prepare_offer(
        sdp, size, &s_test_allocator, offer);
}

static int prepare_answer(const uint8_t *sdp, size_t size,
                          uint8_t extension_id,
                          webrtc_playout_delay_answer_t *answer)
{
    return webrtc_playout_delay_prepare_answer(
        sdp, size, extension_id, &s_test_allocator, answer);
}

static void test_offer_inserts_preferred_extmap(void)
{
    static const char sdp[] =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=rtpmap:102 H264/90000\r\n"
        "a=sendonly\r\n";
    webrtc_playout_delay_offer_t offer = { 0 };
    assert(prepare_offer(
               (const uint8_t *)sdp, strlen(sdp), &offer) == 0);
    assert(offer.data != NULL);
    assert(offer.extension_id == 6);
    assert(offer.inserted);
    assert(offer.size == strlen(offer.data));
    const char *video = strstr(offer.data, "m=video ");
    const char *extmap = strstr(offer.data,
        "a=extmap:6 " WEBRTC_PLAYOUT_DELAY_URI "\r\n");
    const char *rtpmap = strstr(offer.data, "a=rtpmap:102");
    assert(video && extmap && rtpmap && video < extmap && extmap < rtpmap);
    webrtc_playout_delay_release_offer(&offer);
    assert(offer.data == NULL && offer.size == 0 && offer.extension_id == 0);
}

static void test_offer_reuses_existing_and_avoids_conflict(void)
{
    static const char existing[] =
        "v=0\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\n"
        "a=extmap:4/sendonly " WEBRTC_PLAYOUT_DELAY_URI "\n"
        "a=sendonly\n";
    webrtc_playout_delay_offer_t offer = { 0 };
    assert(prepare_offer(
               (const uint8_t *)existing, strlen(existing), &offer) == 0);
    assert(offer.extension_id == 4);
    assert(!offer.inserted);
    assert(strcmp(offer.data, existing) == 0);
    webrtc_playout_delay_release_offer(&offer);

    static const char conflict[] =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=extmap:6 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
        "a=sendonly\r\n";
    assert(prepare_offer(
               (const uint8_t *)conflict, strlen(conflict), &offer) == 0);
    assert(offer.extension_id == 1);
    assert(offer.inserted);
    assert(strstr(offer.data,
        "a=extmap:1 " WEBRTC_PLAYOUT_DELAY_URI "\r\n") != NULL);
    webrtc_playout_delay_release_offer(&offer);

    static const char incompatible[] =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=extmap:4/recvonly " WEBRTC_PLAYOUT_DELAY_URI "\r\n"
        "a=sendonly\r\n";
    assert(prepare_offer(
               (const uint8_t *)incompatible, strlen(incompatible), &offer) < 0);

    static const char recvonly_media[] =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=rtpmap:102 H264/90000\r\n"
        "a=recvonly\r\n";
    assert(prepare_offer(
               (const uint8_t *)recvonly_media, strlen(recvonly_media), &offer) < 0);
}

static void test_offer_preserves_media_field_order(void)
{
    static const char sdp[] =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "i=host video\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "b=AS:8000\r\n"
        "k=clear:unused\r\n"
        "a=rtpmap:102 H264/90000\r\n";
    webrtc_playout_delay_offer_t offer = { 0 };
    assert(prepare_offer(
               (const uint8_t *)sdp, strlen(sdp), &offer) == 0);
    const char *key = strstr(offer.data, "k=clear:unused");
    const char *extmap = strstr(offer.data, "a=extmap:6 ");
    const char *attribute = strstr(offer.data, "a=rtpmap:102");
    assert(key && extmap && attribute && key < extmap && extmap < attribute);
    webrtc_playout_delay_release_offer(&offer);
}

static void test_offer_rejects_missing_video_or_line_delimiter(void)
{
    webrtc_playout_delay_offer_t offer = { 0 };
    static const char audio[] = "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 0\r\n";
    assert(prepare_offer(
               (const uint8_t *)audio, strlen(audio), &offer) < 0);
    static const char unterminated[] = "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 102";
    assert(prepare_offer(
               (const uint8_t *)unterminated, strlen(unterminated), &offer) < 0);
}

static void test_answer_requires_matching_video_extmap(void)
{
    static const char accepted[] =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=extmap:6/recvonly " WEBRTC_PLAYOUT_DELAY_URI "\r\n"
        "a=recvonly\r\n";
    assert(webrtc_playout_delay_answer_accepts(
        accepted, strlen(accepted), 6));
    assert(!webrtc_playout_delay_answer_accepts(
        accepted, strlen(accepted), 5));

    static const char audio_only[] =
        "v=0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=extmap:6 " WEBRTC_PLAYOUT_DELAY_URI "\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=recvonly\r\n";
    assert(!webrtc_playout_delay_answer_accepts(
        audio_only, strlen(audio_only), 6));

    static const char chrome_answer[] =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=extmap:6 " WEBRTC_PLAYOUT_DELAY_URI "\r\n"
        "a=recvonly\r\n";
    assert(webrtc_playout_delay_answer_accepts(
        chrome_answer, strlen(chrome_answer), 6));

    static const char wrong_direction[] =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=extmap:6/sendonly " WEBRTC_PLAYOUT_DELAY_URI "\r\n"
        "a=recvonly\r\n";
    assert(!webrtc_playout_delay_answer_accepts(
        wrong_direction, strlen(wrong_direction), 6));

    static const char inactive_media[] =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=extmap:6 " WEBRTC_PLAYOUT_DELAY_URI "\r\n"
        "a=inactive\r\n";
    assert(!webrtc_playout_delay_answer_accepts(
        inactive_media, strlen(inactive_media), 6));
}

static void test_controlled_offer_requires_browser_receive_direction(void)
{
    uint8_t extension_id = 99;
    static const char recv_offer[] =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=extmap:9/recvonly " WEBRTC_PLAYOUT_DELAY_URI "\r\n"
        "a=recvonly\r\n";
    assert(webrtc_playout_delay_offer_accepts_sender(
        recv_offer, strlen(recv_offer), &extension_id));
    assert(extension_id == 9);

    static const char wrong_media_direction[] =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=extmap:9 " WEBRTC_PLAYOUT_DELAY_URI "\r\n"
        "a=sendonly\r\n";
    extension_id = 99;
    assert(!webrtc_playout_delay_offer_accepts_sender(
        wrong_media_direction, strlen(wrong_media_direction), &extension_id));
    assert(extension_id == 0);

    static const char wrong_ext_direction[] =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=extmap:9/sendonly " WEBRTC_PLAYOUT_DELAY_URI "\r\n"
        "a=recvonly\r\n";
    assert(!webrtc_playout_delay_offer_accepts_sender(
        wrong_ext_direction, strlen(wrong_ext_direction), &extension_id));

    static const char duplicate[] =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=extmap:8 " WEBRTC_PLAYOUT_DELAY_URI "\r\n"
        "a=extmap:9 " WEBRTC_PLAYOUT_DELAY_URI "\r\n"
        "a=recvonly\r\n";
    assert(!webrtc_playout_delay_offer_accepts_sender(
        duplicate, strlen(duplicate), &extension_id));
}

static void test_controlled_answer_reflects_exact_offered_id(void)
{
    static const char generated[] =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=rtpmap:102 H264/90000\r\n"
        "a=sendonly\r\n";
    webrtc_playout_delay_answer_t answer = { 0 };
    assert(prepare_answer(
               (const uint8_t *)generated, strlen(generated), 9,
               &answer) == 0);
    assert(answer.extension_id == 9);
    assert(answer.inserted);
    assert(strstr(answer.data,
                  "a=extmap:9/sendonly " WEBRTC_PLAYOUT_DELAY_URI "\r\n"));
    webrtc_playout_delay_release_answer(&answer);

    static const char already_reflected[] =
        "v=0\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\n"
        "a=extmap:9/sendonly " WEBRTC_PLAYOUT_DELAY_URI "\n"
        "a=sendonly\n";
    assert(prepare_answer(
               (const uint8_t *)already_reflected,
               strlen(already_reflected), 9, &answer) == 0);
    assert(!answer.inserted);
    assert(strcmp(answer.data, already_reflected) == 0);
    webrtc_playout_delay_release_answer(&answer);

    static const char conflicting_id[] =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=extmap:9 urn:ietf:params:rtp-hdrext:sdes:mid\r\n"
        "a=sendonly\r\n";
    assert(prepare_answer(
               (const uint8_t *)conflicting_id, strlen(conflicting_id), 9,
               &answer) < 0);

    static const char remapped_uri[] =
        "v=0\r\n"
        "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
        "a=extmap:8/sendonly " WEBRTC_PLAYOUT_DELAY_URI "\r\n"
        "a=sendonly\r\n";
    assert(prepare_answer(
               (const uint8_t *)remapped_uri, strlen(remapped_uri), 9,
               &answer) < 0);
}

static void test_rtp_stamps_zero_delay_after_fixed_header(void)
{
    static const uint8_t packet[] = {
        0x80, 0xe6, 0x12, 0x34, 0x01, 0x02, 0x03, 0x04,
        0xaa, 0xbb, 0xcc, 0xdd, 0xde, 0xad, 0xbe, 0xef,
    };
    size_t planned = 0;
    assert(webrtc_playout_delay_rtp_plan(packet, sizeof(packet), 6, &planned) ==
           WEBRTC_PLAYOUT_DELAY_RTP_STAMPED);
    assert(planned == sizeof(packet) + WEBRTC_PLAYOUT_DELAY_RTP_OVERHEAD);
    uint8_t output[sizeof(packet) + WEBRTC_PLAYOUT_DELAY_RTP_OVERHEAD] = { 0 };
    size_t written = 0;
    assert(webrtc_playout_delay_rtp_write(
               packet, sizeof(packet), 6, output, sizeof(output), &written) ==
           WEBRTC_PLAYOUT_DELAY_RTP_STAMPED);
    assert(written == sizeof(output));
    assert(output[0] == 0x90);
    assert(memcmp(output + 1, packet + 1, 11) == 0);
    static const uint8_t extension[] = {
        0xbe, 0xde, 0x00, 0x01, 0x62, 0x00, 0x00, 0x00,
    };
    assert(memcmp(output + 12, extension, sizeof(extension)) == 0);
    assert(memcmp(output + 20, packet + 12, sizeof(packet) - 12) == 0);
}

static void test_rtp_inserts_after_csrc_and_bypasses_existing_extension(void)
{
    static const uint8_t with_csrc[] = {
        0x81, 0x66, 0x00, 0x01, 0x00, 0x00, 0x00, 0x5a,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xaa,
    };
    uint8_t output[sizeof(with_csrc) + WEBRTC_PLAYOUT_DELAY_RTP_OVERHEAD] = { 0 };
    size_t written = 0;
    assert(webrtc_playout_delay_rtp_write(
               with_csrc, sizeof(with_csrc), 3, output, sizeof(output), &written) ==
           WEBRTC_PLAYOUT_DELAY_RTP_STAMPED);
    assert(memcmp(output + 1, with_csrc + 1, 15) == 0);
    assert(output[16] == 0xbe && output[17] == 0xde);
    assert(output[20] == 0x32);
    assert(output[24] == 0x99 && output[25] == 0xaa);

    uint8_t already_extended[12] = { 0x90, 0x66 };
    size_t planned = 99;
    assert(webrtc_playout_delay_rtp_plan(
               already_extended, sizeof(already_extended), 6, &planned) ==
           WEBRTC_PLAYOUT_DELAY_RTP_BYPASS_EXISTING_EXTENSION);
    assert(planned == sizeof(already_extended));
}

static void test_rtp_rejects_invalid_packets(void)
{
    uint8_t short_packet[11] = { 0x80 };
    size_t output_size = 0;
    assert(webrtc_playout_delay_rtp_plan(
               short_packet, sizeof(short_packet), 6, &output_size) ==
           WEBRTC_PLAYOUT_DELAY_RTP_INVALID);
    uint8_t wrong_version[12] = { 0x40 };
    assert(webrtc_playout_delay_rtp_plan(
               wrong_version, sizeof(wrong_version), 6, &output_size) ==
           WEBRTC_PLAYOUT_DELAY_RTP_INVALID);
    uint8_t truncated_csrc[12] = { 0x8f };
    assert(webrtc_playout_delay_rtp_plan(
               truncated_csrc, sizeof(truncated_csrc), 6, &output_size) ==
           WEBRTC_PLAYOUT_DELAY_RTP_INVALID);
    uint8_t valid[12] = { 0x80 };
    assert(webrtc_playout_delay_rtp_plan(
               valid, sizeof(valid), 0, &output_size) ==
           WEBRTC_PLAYOUT_DELAY_RTP_INVALID);
}

int main(void)
{
    test_offer_inserts_preferred_extmap();
    test_offer_reuses_existing_and_avoids_conflict();
    test_offer_preserves_media_field_order();
    test_offer_rejects_missing_video_or_line_delimiter();
    test_answer_requires_matching_video_extmap();
    test_controlled_offer_requires_browser_receive_direction();
    test_controlled_answer_reflects_exact_offered_id();
    test_rtp_stamps_zero_delay_after_fixed_header();
    test_rtp_inserts_after_csrc_and_bypasses_existing_extension();
    test_rtp_rejects_invalid_packets();
    puts("playout-delay SDP/RTP tests: OK");
    return 0;
}

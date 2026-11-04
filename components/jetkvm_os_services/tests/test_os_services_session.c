/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The session against a sender written from the OS Services protocol in
 * this file. The session.ready signature is checked with OpenSSL's
 * Ed25519, not with the Monocypher code that made it.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "cJSON.h"
#include "fake_platform.h"
#include "fixtures/test_release_key.h"
#include "host_hash.h"
#include "fake_video.h"
#include "os_services_session.h"
#include "os_services_update.h"

static uint8_t s_payload_buffer[OSVC_MAX_CONTROL_PAYLOAD];
static fake_platform_t s_fake;
static osvc_platform_t s_platform;
static osvc_session_t s_session;

static osvc_settings_t default_settings(void)
{
    osvc_settings_t settings = {
        .clipboard_enabled = true,
        .terminal_enabled = true,
        .files_enabled = false,
        .video_enabled = true,
        .width = 2560U,
        .height = 1440U,
        .layout = OSVC_LAYOUT_EXTEND,
    };
    return settings;
}

/* The flags in the video.capabilities of the test's sender. */
static uint32_t s_video_flags = OSVC_VIDEO_CODEC_FLAG_H264;

static void setup(void)
{
    fake_platform_init(&s_fake, &s_platform);
    const osvc_settings_t settings = default_settings();
    osvc_session_init(&s_session, &s_platform, &settings, s_payload_buffer,
                      sizeof(s_payload_buffer));
}

/* Sender side: one message, as the sender's codec frames it. */
static size_t encode(uint8_t *out, uint16_t service, uint16_t kind,
                     const void *payload, size_t length)
{
    osvc_put_envelope(out, service, kind, (uint32_t)length);
    if (length > 0U) {
        memcpy(out + OSVC_ENVELOPE_LEN, payload, length);
    }
    return OSVC_ENVELOPE_LEN + length;
}

static void feed(const uint8_t *data, size_t length)
{
    const size_t used = osvc_session_receive(&s_session, data, length);
    assert(used == length);
}

/* Feeds in chunks of a given size, as USB transfers would split it. */
static void feed_chunked(const uint8_t *data, size_t length, size_t chunk)
{
    for (size_t offset = 0U; offset < length; offset += chunk) {
        const size_t n = length - offset < chunk ? length - offset : chunk;
        feed(data + offset, n);
    }
}

static void send_message(uint16_t service, uint16_t kind, const void *payload,
                         size_t length)
{
    static uint8_t buffer[OSVC_ENVELOPE_LEN + OSVC_MAX_CONTROL_PAYLOAD];
    feed(buffer, encode(buffer, service, kind, payload, length));
}

static void reset_and_admit(uint8_t token[OSVC_TOKEN_LEN])
{
    uint8_t status[OSVC_USB_STATUS_LEN];
    osvc_session_usb_reset(&s_session, status);
    assert(memcmp(status, "JKV5", 4) == 0);
    assert(status[4] == OSVC_PHASE_RESETTING);
    assert(status[5] == 0U && status[6] == 0U && status[7] == 0U);
    static const uint8_t zero[OSVC_TOKEN_LEN] = {0};
    assert(memcmp(status + 8, zero, OSVC_TOKEN_LEN) != 0);
    memcpy(token, status + 8, OSVC_TOKEN_LEN);
    osvc_session_usb_status(&s_session, status);
    assert(status[4] == OSVC_PHASE_RESETTING);
    osvc_session_transport_drained(&s_session);
    osvc_session_usb_status(&s_session, status);
    assert(status[4] == OSVC_PHASE_READY);
    assert(memcmp(status + 8, token, OSVC_TOKEN_LEN) == 0);
}

static void make_hello(uint8_t hello[OSVC_HELLO_LEN], uint16_t major,
                       uint16_t minor, const uint8_t token[OSVC_TOKEN_LEN],
                       uint8_t nonce_seed)
{
    osvc_put_u16(hello, major);
    osvc_put_u16(hello + 2, minor);
    memcpy(hello + 4, token, OSVC_TOKEN_LEN);
    for (size_t i = 0U; i < OSVC_NONCE_LEN; ++i) {
        hello[20 + i] = (uint8_t)(nonce_seed + i * 13U);
    }
}

static bool openssl_verify(const uint8_t public_key[32],
                           const uint8_t *message, size_t length,
                           const uint8_t signature[64])
{
    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL,
                                                public_key, 32);
    assert(key != NULL);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    assert(ctx != NULL);
    assert(EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, key) == 1);
    const int ok = EVP_DigestVerify(ctx, signature, 64, message, length);
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok == 1;
}

/* What the sender does with session.ready (KVM approval). */
static void check_ready(const fake_message_t *ready,
                        const uint8_t hello[OSVC_HELLO_LEN],
                        const uint8_t token[OSVC_TOKEN_LEN])
{
    assert(ready->service == OSVC_SERVICE_SESSION);
    assert(ready->kind == OSVC_SESSION_READY);
    assert(ready->length == OSVC_READY_LEN);
    assert(osvc_get_u16(ready->payload) == 5U);
    assert(osvc_get_u16(ready->payload + 2) == 0U);
    assert(memcmp(ready->payload + 4, token, OSVC_TOKEN_LEN) == 0);
    assert(memcmp(ready->payload + 20, s_fake.public_key, 32) == 0);
    uint8_t message[35 + OSVC_HELLO_LEN + OSVC_READY_SIGNED_LEN];
    memcpy(message, "IP-KVM OS Services session.ready v5", 35);
    memcpy(message + 35, hello, OSVC_HELLO_LEN);
    memcpy(message + 35 + OSVC_HELLO_LEN, ready->payload,
           OSVC_READY_SIGNED_LEN);
    assert(openssl_verify(ready->payload + 20, message, sizeof(message),
                          ready->payload + OSVC_READY_SIGNED_LEN));
    /* A signature for another hello (another nonce) must not verify. */
    message[35 + 20] ^= 1U;
    assert(!openssl_verify(ready->payload + 20, message, sizeof(message),
                           ready->payload + OSVC_READY_SIGNED_LEN));
}

static void check_config(const fake_message_t *config, bool terminal,
                         bool files, const char *display)
{
    assert(config->service == OSVC_SERVICE_SESSION);
    assert(config->kind == OSVC_SESSION_CONFIG);
    cJSON *json = cJSON_ParseWithLength((const char *)config->payload,
                                        config->length);
    assert(json != NULL);
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(json, "terminal_enabled");
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(json, "files_enabled");
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(json, "capture_display");
    assert(cJSON_IsBool(t) && cJSON_IsTrue(t) == terminal);
    assert(cJSON_IsBool(f) && cJSON_IsTrue(f) == files);
    assert(cJSON_IsString(d) && strcmp(d->valuestring, display) == 0);
    /* The KVM leaves the display's mode to a sender that says it sets it. */
    assert(cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(json, "display_mode_by_sender")));
    cJSON_Delete(json);
}

/* Reset, hello, ready and config: an open session. */
static void open_session(uint8_t token[OSVC_TOKEN_LEN])
{
    reset_and_admit(token);
    uint8_t hello[OSVC_HELLO_LEN];
    make_hello(hello, 5U, 0U, token, 3U);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_HELLO, hello, sizeof(hello));
    assert(osvc_session_open(&s_session));
    fake_message_t ready;
    fake_message_t config;
    assert(fake_next_message(&s_fake, &ready));
    check_ready(&ready, hello, token);
    assert(fake_next_message(&s_fake, &config));
    check_config(&config, true, false, "");
    fake_message_t extra;
    assert(!fake_next_message(&s_fake, &extra));
}

static void test_reset_handshake_and_signature(void)
{
    setup();
    uint8_t token[OSVC_TOKEN_LEN];
    uint8_t status[OSVC_USB_STATUS_LEN];
    osvc_session_usb_status(&s_session, status);
    assert(status[4] == OSVC_PHASE_RESETTING);
    open_session(token);
    osvc_session_usb_status(&s_session, status);
    assert(status[4] == OSVC_PHASE_ACTIVE);
    assert(memcmp(status + 8, token, OSVC_TOKEN_LEN) == 0);
    assert(s_session.counters.sessions_opened == 1U);
    /* A second RESET makes a new token and ends the session. */
    uint8_t next[OSVC_TOKEN_LEN];
    reset_and_admit(next);
    assert(memcmp(next, token, OSVC_TOKEN_LEN) != 0);
    assert(!osvc_session_open(&s_session));
    assert(s_fake.resets == 2U);
}

/*
 * Stale bytes before the hello: a frame of the old session, a hello with the
 * old token, half an envelope. Fed in every chunk size from 1 to 70, the
 * KVM must find exactly the new hello and stay aligned for the next
 * message.
 */
static void test_hello_search_across_chunks(void)
{
    static uint8_t stream[200000];
    for (size_t chunk = 1U; chunk <= 70U; ++chunk) {
        setup();
        uint8_t old_token[OSVC_TOKEN_LEN];
        reset_and_admit(old_token);
        uint8_t token[OSVC_TOKEN_LEN];
        reset_and_admit(token);
        size_t n = 0U;
        uint8_t frame_header[OSVC_ENVELOPE_LEN];
        osvc_put_envelope(frame_header, OSVC_SERVICE_VIDEO, OSVC_VIDEO_FRAME,
                          100000U);
        /* The tail of a frame, no envelope. */
        for (size_t i = 0U; i < 1234U; ++i) {
            stream[n++] = (uint8_t)(i * 31U);
        }
        uint8_t old_hello[OSVC_HELLO_LEN];
        make_hello(old_hello, 5U, 0U, old_token, 9U);
        n += encode(stream + n, OSVC_SERVICE_SESSION, OSVC_SESSION_HELLO,
                    old_hello, sizeof(old_hello));
        memcpy(stream + n, frame_header, 5U);
        n += 5U;
        uint8_t hello[OSVC_HELLO_LEN];
        make_hello(hello, 5U, 0U, token, (uint8_t)chunk);
        n += encode(stream + n, OSVC_SERVICE_SESSION, OSVC_SESSION_HELLO,
                    hello, sizeof(hello));
        n += encode(stream + n, OSVC_SERVICE_SESSION, OSVC_SESSION_PING, NULL,
                    0U);
        const char info[] = "{\"version\":\"0.5.0\",\"displays\":[\"HDMI-A-1\"]}";
        n += encode(stream + n, OSVC_SERVICE_SESSION, OSVC_SESSION_INFO, info,
                    strlen(info));
        feed_chunked(stream, n, chunk);
        assert(osvc_session_open(&s_session));
        fake_message_t ready;
        assert(fake_next_message(&s_fake, &ready));
        check_ready(&ready, hello, token);
        assert(s_session.sender_info_valid);
        assert(strcmp(s_session.sender_version, "0.5.0") == 0);
        assert(s_session.display_count == 1U);
        assert(strcmp(s_session.displays, "HDMI-A-1") == 0);
        assert(s_session.counters.messages_in == 2U);
    }
}

static void test_hello_search_gives_up_after_8_mib(void)
{
    setup();
    uint8_t token[OSVC_TOKEN_LEN];
    reset_and_admit(token);
    static uint8_t junk[65536];
    memset(junk, 0, sizeof(junk));
    size_t total = 0U;
    while (osvc_session_receive(&s_session, junk, sizeof(junk)) == sizeof(junk)) {
        total += sizeof(junk);
        if (s_session.link != OSVC_LINK_ADMITTED) {
            break;
        }
        assert(total <= OSVC_HELLO_SEARCH_LIMIT + sizeof(junk));
    }
    assert(s_session.link == OSVC_LINK_IDLE);
    assert(total >= OSVC_HELLO_SEARCH_LIMIT);
    assert(strstr(s_fake.end_reason, "8 MiB") != NULL);
    /* A stopped session reads nothing more until RESET. */
    assert(osvc_session_receive(&s_session, junk, 10U) == 0U);
}

static void test_wrong_major_ends_session(void)
{
    setup();
    uint8_t token[OSVC_TOKEN_LEN];
    reset_and_admit(token);
    uint8_t hello[OSVC_HELLO_LEN];
    make_hello(hello, 4U, 0U, token, 1U);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_HELLO, hello, sizeof(hello));
    assert(!osvc_session_open(&s_session));
    fake_message_t message;
    assert(!fake_next_message(&s_fake, &message));
}

static void test_newer_minor_selects_ours(void)
{
    setup();
    uint8_t token[OSVC_TOKEN_LEN];
    reset_and_admit(token);
    uint8_t hello[OSVC_HELLO_LEN];
    make_hello(hello, 5U, 7U, token, 1U);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_HELLO, hello, sizeof(hello));
    fake_message_t ready;
    assert(fake_next_message(&s_fake, &ready));
    assert(osvc_get_u16(ready.payload + 2) == 0U);
}

static void test_timers(void)
{
    setup();
    uint8_t token[OSVC_TOKEN_LEN];
    reset_and_admit(token);
    s_fake.now += OSVC_HANDSHAKE_TIMEOUT_MS;
    osvc_session_tick(&s_session);
    assert(s_session.link == OSVC_LINK_ADMITTED);
    s_fake.now += 1;
    osvc_session_tick(&s_session);
    assert(s_session.link == OSVC_LINK_IDLE);

    setup();
    open_session(token);
    for (int i = 0; i < 10; ++i) {
        s_fake.now += 14000;
        osvc_session_tick(&s_session);
        assert(osvc_session_open(&s_session));
        send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_PING, NULL, 0U);
    }
    /* An unknown message also counts as life. */
    s_fake.now += 14000;
    send_message(77U, 1U, "xyz", 3U);
    s_fake.now += 14000;
    osvc_session_tick(&s_session);
    assert(osvc_session_open(&s_session));
    s_fake.now += 1001;
    osvc_session_tick(&s_session);
    assert(!osvc_session_open(&s_session));
    assert(strstr(s_fake.end_reason, "15 seconds") != NULL);
}

static void test_session_errors(void)
{
    uint8_t token[OSVC_TOKEN_LEN];
    uint8_t hello[OSVC_HELLO_LEN];

    setup();
    open_session(token);
    make_hello(hello, 5U, 0U, token, 2U);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_HELLO, hello, sizeof(hello));
    assert(!osvc_session_open(&s_session));

    setup();
    open_session(token);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_PING, "x", 1U);
    assert(!osvc_session_open(&s_session));

    setup();
    open_session(token);
    const uint8_t bad_close = 1U;
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_CLOSE, &bad_close, 1U);
    assert(!osvc_session_open(&s_session));

    /* A payload over its limit ends the session at the envelope. */
    setup();
    open_session(token);
    uint8_t envelope[OSVC_ENVELOPE_LEN];
    osvc_put_envelope(envelope, 9U, 9U, OSVC_MAX_CONTROL_PAYLOAD + 1U);
    assert(osvc_session_receive(&s_session, envelope, sizeof(envelope)) ==
           sizeof(envelope));
    assert(!osvc_session_open(&s_session));
    /* video.frame may carry 8 MiB; one byte more ends the session. */
    setup();
    open_session(token);
    osvc_put_envelope(envelope, OSVC_SERVICE_VIDEO, OSVC_VIDEO_FRAME,
                      OSVC_MAX_FRAME_PAYLOAD);
    feed(envelope, sizeof(envelope));
    assert(osvc_session_open(&s_session));
    setup();
    open_session(token);
    osvc_put_envelope(envelope, OSVC_SERVICE_VIDEO, OSVC_VIDEO_FRAME,
                      OSVC_MAX_FRAME_PAYLOAD + 1U);
    feed(envelope, sizeof(envelope));
    assert(!osvc_session_open(&s_session));

    /* session.info over a limit. */
    setup();
    open_session(token);
    char info[512];
    snprintf(info, sizeof(info), "{\"version\":\"%065d\",\"displays\":[]}", 1);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_INFO, info, strlen(info));
    assert(!osvc_session_open(&s_session));
    setup();
    open_session(token);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_INFO, "[1]", 3U);
    assert(!osvc_session_open(&s_session));
    setup();
    open_session(token);
    static char many[8192];
    size_t n = (size_t)snprintf(many, sizeof(many), "{\"displays\":[");
    for (int i = 0; i < 65; ++i) {
        n += (size_t)snprintf(many + n, sizeof(many) - n, "%s\"d%d\"",
                              i ? "," : "", i);
    }
    n += (size_t)snprintf(many + n, sizeof(many) - n, "]}");
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_INFO, many, n);
    assert(!osvc_session_open(&s_session));

    /* A KVM-to-sender kind from the sender. */
    setup();
    open_session(token);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_CONFIG, "{}", 2U);
    assert(!osvc_session_open(&s_session));

    /* A write that cannot be queued ends the session. */
    setup();
    open_session(token);
    s_fake.fail_writes = true;
    const uint8_t caps[8] = { 3U, 0, 0, 0, OSVC_OS_LINUX, 0, 0, 0 };
    send_message(OSVC_SERVICE_CLIPBOARD, OSVC_CLIPBOARD_CAPABILITIES, caps,
                 sizeof(caps));
    assert(!osvc_session_open(&s_session));
}

static void test_unknown_messages_keep_alignment(void)
{
    setup();
    uint8_t token[OSVC_TOKEN_LEN];
    open_session(token);
    static uint8_t stream[4 * OSVC_MAX_CONTROL_PAYLOAD];
    static uint8_t big[80000];
    memset(big, 0x5a, sizeof(big));
    size_t n = 0U;
    n += encode(stream + n, 42U, 1U, big, OSVC_MAX_CONTROL_PAYLOAD);
    n += encode(stream + n, OSVC_SERVICE_CLIPBOARD, 99U, big, 100U);
    n += encode(stream + n, OSVC_SERVICE_SESSION, 0U, NULL, 0U);
    /* A frame while video is not shown: discarded whole. */
    n += encode(stream + n, OSVC_SERVICE_VIDEO, OSVC_VIDEO_FRAME, big, 70000U);
    const char info[] = "{\"version\":\"9.9\",\"displays\":[\"A\",\"B\"]}";
    n += encode(stream + n, OSVC_SERVICE_SESSION, OSVC_SESSION_INFO, info,
                strlen(info));
    feed_chunked(stream, n, 4093U);
    assert(osvc_session_open(&s_session));
    assert(s_session.counters.messages_skipped == 4U);
    assert(strcmp(s_session.sender_version, "9.9") == 0);
    assert(s_session.display_count == 2U);
}

/* session.info changes what the browser sees only when it differs; the
 * comparison happens in place, so a list that shrinks and grows again must
 * still count as a change. */
static void test_session_info_reports_changes(void)
{
    setup();
    uint8_t token[OSVC_TOKEN_LEN];
    open_session(token);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_PING, NULL, 0U);
    static const struct {
        const char *json;
        bool changes;
    } steps[] = {
        { "{\"version\":\"1.0\",\"displays\":[\"A\",\"B\"]}", true },
        { "{\"version\":\"1.0\",\"displays\":[\"A\",\"B\"]}", false },
        { "{\"version\":\"1.0\",\"displays\":[\"A\",\"C\"]}", true },
        { "{\"version\":\"1.0\",\"displays\":[\"A\"]}", true },
        { "{\"version\":\"1.0\",\"displays\":[\"A\",\"C\"]}", true },
        { "{\"version\":\"1.0\",\"displays\":[\"AC\"]}", true },
        { "{\"version\":\"1.1\",\"displays\":[\"AC\"]}", true },
        { "{\"displays\":[\"AC\"]}", true },
        { "{}", true },
        { "{\"version\":null,\"displays\":null}", false },
    };
    for (size_t i = 0U; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        const unsigned before = s_fake.state_changes;
        send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_INFO, steps[i].json,
                     strlen(steps[i].json));
        assert(osvc_session_open(&s_session));
        assert((s_fake.state_changes != before) == steps[i].changes);
    }
    assert(s_session.sender_version[0] == '\0');
    assert(s_session.display_count == 0U);

    /* The largest list the protocol allows: 64 names of 256 bytes. */
    static char info[OSVC_INFO_DISPLAYS_MAX * (OSVC_INFO_DISPLAY_MAX + 3U) +
                     64U];
    size_t n = (size_t)snprintf(info, sizeof(info),
                                "{\"version\":\"0.4.0\",\"displays\":[");
    for (size_t d = 0U; d < OSVC_INFO_DISPLAYS_MAX; ++d) {
        info[n++] = d ? ',' : ' ';
        info[n++] = '"';
        for (size_t c = 0U; c < OSVC_INFO_DISPLAY_MAX; ++c) {
            info[n++] = (char)('a' + (d + c) % 26U);
        }
        info[n++] = '"';
    }
    n += (size_t)snprintf(info + n, sizeof(info) - n, "]}");
    unsigned before = s_fake.state_changes;
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_INFO, info, n);
    assert(osvc_session_open(&s_session));
    assert(s_fake.state_changes == before + 1U);
    assert(s_session.display_count == OSVC_INFO_DISPLAYS_MAX);
    const char *last = s_session.displays;
    for (size_t d = 0U; d + 1U < OSVC_INFO_DISPLAYS_MAX; ++d) {
        last += strlen(last) + 1U;
    }
    assert(strlen(last) == OSVC_INFO_DISPLAY_MAX);
    assert(last[0] == (char)('a' + (OSVC_INFO_DISPLAYS_MAX - 1U) % 26U));
    before = s_fake.state_changes;
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_INFO, info, n);
    assert(s_fake.state_changes == before);
    /* The last byte of the last name. */
    info[n - 4U] = '#';
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_INFO, info, n);
    assert(s_fake.state_changes == before + 1U);
    assert(last[OSVC_INFO_DISPLAY_MAX - 1U] == '#');
}

static void test_approval_pending(void)
{
    setup();
    uint8_t token[OSVC_TOKEN_LEN];
    open_session(token);
    const uint8_t reason = OSVC_CLOSE_NOT_APPROVED;
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_CLOSE, &reason, 1U);
    assert(!osvc_session_open(&s_session));
    assert(s_session.approval_pending);
    /* The sender retries; it is still not approved. */
    fake_clear_output(&s_fake);
    open_session(token);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_CLOSE, &reason, 1U);
    assert(s_session.approval_pending);
    /* Approved now: the first message that is not a close clears it. */
    fake_clear_output(&s_fake);
    open_session(token);
    assert(s_session.approval_pending);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_PING, NULL, 0U);
    assert(!s_session.approval_pending);
}

static void register_clipboard(uint32_t flags, osvc_target_os_t os)
{
    uint8_t caps[8] = {0};
    osvc_put_u32(caps, flags);
    caps[4] = (uint8_t)os;
    send_message(OSVC_SERVICE_CLIPBOARD, OSVC_CLIPBOARD_CAPABILITIES, caps,
                 sizeof(caps));
}

static void expect_clipboard_config(uint8_t enabled)
{
    fake_message_t message;
    assert(fake_next_message(&s_fake, &message));
    assert(message.service == OSVC_SERVICE_CLIPBOARD);
    assert(message.kind == OSVC_CLIPBOARD_CONFIG);
    assert(message.length == 1U && message.payload[0] == enabled);
}

static void answer_content(uint32_t id, uint8_t status, uint8_t truncated,
                           const char *text)
{
    static uint8_t payload[8 + OSVC_CLIPBOARD_MAX_TEXT];
    memset(payload, 0, 8);
    osvc_put_u32(payload, id);
    payload[4] = status;
    payload[5] = truncated;
    const size_t length = text ? strlen(text) : 0U;
    if (length > 0U) {
        memcpy(payload + 8, text, length);
    }
    send_message(OSVC_SERVICE_CLIPBOARD, OSVC_CLIPBOARD_CONTENT, payload,
                 8U + length);
}

static void test_clipboard(void)
{
    setup();
    uint8_t token[OSVC_TOKEN_LEN];
    open_session(token);
    osvc_clipboard_status_t status;
    assert(osvc_session_clipboard_request(&s_session, NULL, 0U, &status) == 0U);
    assert(status == OSVC_CLIPBOARD_UNSUPPORTED);

    register_clipboard(OSVC_CLIPBOARD_FLAG_READ | OSVC_CLIPBOARD_FLAG_WRITE,
                       OSVC_OS_LINUX);
    expect_clipboard_config(1U);
    assert(osvc_session_clipboard_available(&s_session));
    assert(s_session.clipboard.os == OSVC_OS_LINUX);

    /* get: the request on the wire, then the answer. */
    const uint32_t get = osvc_session_clipboard_request(&s_session, NULL, 0U,
                                                        &status);
    assert(get != 0U && status == OSVC_CLIPBOARD_OK);
    fake_message_t message;
    assert(fake_next_message(&s_fake, &message));
    assert(message.service == OSVC_SERVICE_CLIPBOARD &&
           message.kind == OSVC_CLIPBOARD_GET && message.length == 4U &&
           osvc_get_u32(message.payload) == get);
    answer_content(get, OSVC_CLIPBOARD_OK, 0U, "héllo, wörld");
    assert(s_fake.clipboard_count == 1U);
    assert(s_fake.clipboard[0].request_id == get);
    assert(s_fake.clipboard[0].answered);
    assert(s_fake.clipboard[0].status == OSVC_CLIPBOARD_OK);
    assert(strcmp(s_fake.clipboard[0].text, "héllo, wörld") == 0);
    /* A repeated answer for the finished request is ignored. */
    answer_content(get, OSVC_CLIPBOARD_OK, 0U, "again");
    assert(s_fake.clipboard_count == 1U);

    /* set, with the largest text the wire allows. */
    static uint8_t text[OSVC_CLIPBOARD_MAX_TEXT];
    memset(text, 'a', sizeof(text));
    const uint32_t set = osvc_session_clipboard_request(&s_session, text,
                                                        sizeof(text), &status);
    assert(set != 0U && set != get);
    assert(fake_next_message(&s_fake, &message));
    assert(message.kind == OSVC_CLIPBOARD_SET &&
           message.length == 4U + sizeof(text) &&
           osvc_get_u32(message.payload) == set &&
           memcmp(message.payload + 4, text, sizeof(text)) == 0);
    uint8_t answer[5];
    osvc_put_u32(answer, set);
    answer[4] = OSVC_CLIPBOARD_OK;
    send_message(OSVC_SERVICE_CLIPBOARD, OSVC_CLIPBOARD_STATUS, answer, 5U);
    assert(s_fake.clipboard_count == 2U && s_fake.clipboard[1].answered);
    /* One byte over the limit, or invalid UTF-8, never reaches the wire. */
    static uint8_t too_long[OSVC_CLIPBOARD_MAX_TEXT + 1U];
    memset(too_long, 'b', sizeof(too_long));
    assert(osvc_session_clipboard_request(&s_session, too_long,
                                          sizeof(too_long), &status) == 0U);
    const uint8_t bad_utf8[] = { 0xc0, 0x80 };
    assert(osvc_session_clipboard_request(&s_session, bad_utf8, 2U, &status) ==
           0U);
    assert(!fake_next_message(&s_fake, &message));

    /* The KVM abandons a request after 2 seconds; a late answer is ignored. */
    const uint32_t slow = osvc_session_clipboard_request(&s_session, NULL, 0U,
                                                         &status);
    s_fake.now += OSVC_CLIPBOARD_TIMEOUT_MS - 1;
    osvc_session_tick(&s_session);
    assert(s_fake.clipboard_count == 2U);
    s_fake.now += 1;
    osvc_session_tick(&s_session);
    assert(s_fake.clipboard_count == 3U);
    assert(s_fake.clipboard[2].request_id == slow &&
           !s_fake.clipboard[2].answered &&
           s_fake.clipboard[2].status == OSVC_CLIPBOARD_FAILED);
    answer_content(slow, OSVC_CLIPBOARD_OK, 0U, "late");
    assert(s_fake.clipboard_count == 3U);
    assert(osvc_session_clipboard_available(&s_session));

    /* At most four requests are open. */
    fake_clear_output(&s_fake);
    uint32_t open[4];
    for (int i = 0; i < 4; ++i) {
        open[i] = osvc_session_clipboard_request(&s_session, NULL, 0U, &status);
        assert(open[i] != 0U);
    }
    assert(osvc_session_clipboard_request(&s_session, NULL, 0U, &status) == 0U);
    assert(status == OSVC_CLIPBOARD_FAILED);
    /* Answers in any order. */
    answer_content(open[2], OSVC_CLIPBOARD_EMPTY, 0U, NULL);
    assert(s_fake.clipboard[3].request_id == open[2] &&
           s_fake.clipboard[3].status == OSVC_CLIPBOARD_EMPTY);
    /* Withdrawal fails every open request. */
    register_clipboard(0U, OSVC_OS_LINUX);
    assert(s_fake.clipboard_count == 7U);
    assert(!osvc_session_clipboard_available(&s_session));
}

static void test_clipboard_faults_and_settings(void)
{
    setup();
    uint8_t token[OSVC_TOKEN_LEN];
    open_session(token);
    register_clipboard(OSVC_CLIPBOARD_FLAG_READ, OSVC_OS_MACOS);
    expect_clipboard_config(1U);
    osvc_clipboard_status_t status;
    /* Read access only: a set is unsupported. */
    assert(osvc_session_clipboard_request(&s_session, (const uint8_t *)"x", 1U,
                                          &status) == 0U &&
           status == OSVC_CLIPBOARD_UNSUPPORTED);
    const uint32_t get = osvc_session_clipboard_request(&s_session, NULL, 0U,
                                                        &status);
    fake_clear_output(&s_fake);
    /* Text with a non-OK status is invalid: the clipboard service faults,
     * the session stays. */
    answer_content(get, OSVC_CLIPBOARD_EMPTY, 0U, "x");
    assert(osvc_session_open(&s_session));
    assert(!osvc_session_clipboard_available(&s_session));
    assert(s_fake.clipboard_count == 1U && !s_fake.clipboard[0].answered);
    assert(s_session.counters.service_faults == 1U);
    /* Until it registers again its messages are ignored. */
    answer_content(get, OSVC_CLIPBOARD_OK, 0U, "y");
    assert(s_fake.clipboard_count == 1U);
    register_clipboard(OSVC_CLIPBOARD_FLAG_READ | OSVC_CLIPBOARD_FLAG_WRITE,
                       OSVC_OS_WINDOWS);
    expect_clipboard_config(1U);
    assert(osvc_session_clipboard_available(&s_session));

    /* Invalid capabilities fault the service. */
    register_clipboard(4U, OSVC_OS_WINDOWS);
    assert(!osvc_session_clipboard_available(&s_session));
    register_clipboard(1U, (osvc_target_os_t)4);
    assert(!osvc_session_clipboard_available(&s_session));
    register_clipboard(1U, OSVC_OS_WINDOWS);
    expect_clipboard_config(1U);

    /* Sharing off: the sender is told, and no request is sent. */
    osvc_settings_t settings = default_settings();
    settings.clipboard_enabled = false;
    fake_clear_output(&s_fake);
    osvc_session_apply_settings(&s_session, &settings);
    expect_clipboard_config(0U);
    fake_message_t message;
    assert(!fake_next_message(&s_fake, &message));
    assert(osvc_session_clipboard_request(&s_session, NULL, 0U, &status) == 0U &&
           status == OSVC_CLIPBOARD_UNSUPPORTED);
    /* A new registration is answered with the current setting. */
    register_clipboard(1U, OSVC_OS_WINDOWS);
    expect_clipboard_config(0U);

    /* The session policy goes out when it changes, and only then. */
    settings.terminal_enabled = false;
    snprintf(settings.capture_display, sizeof(settings.capture_display),
             "DP-2 \"left\"");
    osvc_session_apply_settings(&s_session, &settings);
    assert(fake_next_message(&s_fake, &message));
    check_config(&message, false, false, "DP-2 \"left\"");
    osvc_session_apply_settings(&s_session, &settings);
    assert(!fake_next_message(&s_fake, &message));
}

static void test_video_registration_is_answered(void)
{
    setup();
    uint8_t token[OSVC_TOKEN_LEN];
    open_session(token);
    uint8_t caps[16];
    osvc_put_u32(caps, OSVC_VIDEO_CODEC_FLAG_H264 | OSVC_VIDEO_CODEC_FLAG_H265);
    osvc_put_u32(caps + 4, 3840U);
    osvc_put_u32(caps + 8, 2160U);
    osvc_put_u32(caps + 12, 60U);
    send_message(OSVC_SERVICE_VIDEO, OSVC_VIDEO_CAPABILITIES, caps, 16U);
    fake_message_t config;
    assert(fake_next_message(&s_fake, &config));
    assert(config.service == OSVC_SERVICE_VIDEO &&
           config.kind == OSVC_VIDEO_CONFIG && config.length == 20U);
    assert(config.payload[0] == 0U); /* not shown yet: disabled */
    assert(config.payload[1] == OSVC_LAYOUT_EXTEND);
    assert(config.payload[2] == OSVC_CODEC_H264);
    assert(osvc_get_u32(config.payload + 4) == 2560U);
    assert(osvc_get_u32(config.payload + 8) == 1440U);
    /* An enabled status without a switch faults video only. */
    uint8_t status[32] = {0};
    status[0] = 1U;
    osvc_put_u32(status + 4, 2560U);
    osvc_put_u32(status + 8, 1440U);
    osvc_put_u32(status + 12, 60U);
    send_message(OSVC_SERVICE_VIDEO, OSVC_VIDEO_STATUS, status, 32U);
    assert(osvc_session_open(&s_session));
    assert(!s_session.video_registered);
    assert(fake_next_message(&s_fake, &config));
    assert(config.kind == OSVC_VIDEO_CONFIG && config.payload[0] == 0U);
    /* Missing H.264 is invalid. */
    osvc_put_u32(caps, OSVC_VIDEO_CODEC_FLAG_H265);
    send_message(OSVC_SERVICE_VIDEO, OSVC_VIDEO_CAPABILITIES, caps, 16U);
    assert(!s_session.video_registered);
    assert(!fake_next_message(&s_fake, &config));
    /* Withdrawal: 16 zero bytes. */
    osvc_put_u32(caps, OSVC_VIDEO_CODEC_FLAG_H264);
    send_message(OSVC_SERVICE_VIDEO, OSVC_VIDEO_CAPABILITIES, caps, 16U);
    assert(s_session.video_registered);
    assert(fake_next_message(&s_fake, &config));
    memset(caps, 0, sizeof(caps));
    send_message(OSVC_SERVICE_VIDEO, OSVC_VIDEO_CAPABILITIES, caps, 16U);
    assert(!s_session.video_registered);
}

static void test_other_registrations(void)
{
    setup();
    uint8_t token[OSVC_TOKEN_LEN];
    open_session(token);
    uint8_t flags[4];
    osvc_put_u32(flags, 1U);
    send_message(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_CAPABILITIES, flags, 4U);
    send_message(OSVC_SERVICE_FILE, OSVC_FILE_CAPABILITIES, flags, 4U);
    uint8_t update[4 + 21];
    osvc_put_u32(update, 1U);
    memcpy(update + 4, "linux-x86_64-appimage", 21);
    send_message(OSVC_SERVICE_UPDATE, OSVC_UPDATE_CAPABILITIES, update,
                 sizeof(update));
    assert(s_session.terminal_shell && s_session.file_receive &&
           s_session.update_install);
    assert(strcmp(s_session.update_package, "linux-x86_64-appimage") == 0);
    osvc_put_u32(flags, 3U);
    send_message(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_CAPABILITIES, flags, 4U);
    assert(!s_session.terminal_shell);
    assert(osvc_session_open(&s_session));
    /* None of these is answered (Service registration). */
    fake_message_t message;
    assert(!fake_next_message(&s_fake, &message));
}

/* A STATUS before the first RESET has a non-zero token, because the
 * protocol has no all-zero token. */
static void test_status_before_reset_has_a_token(void)
{
    setup();
    uint8_t usb_status[OSVC_USB_STATUS_LEN];
    osvc_session_usb_status(&s_session, usb_status);
    assert(usb_status[4] == OSVC_PHASE_RESETTING);
    static const uint8_t zero[OSVC_TOKEN_LEN];
    assert(memcmp(usb_status + 8, zero, OSVC_TOKEN_LEN) != 0);
}

static void test_reconfiguration_ends_session(void)
{
    setup();
    uint8_t token[OSVC_TOKEN_LEN];
    open_session(token);
    register_clipboard(1U, OSVC_OS_LINUX);
    osvc_clipboard_status_t status;
    assert(osvc_session_clipboard_request(&s_session, NULL, 0U, &status) != 0U);
    osvc_session_usb_reconfigured(&s_session, false);
    assert(!osvc_session_open(&s_session));
    assert(s_fake.clipboard_count == 1U && !s_fake.clipboard[0].answered);
    uint8_t usb_status[OSVC_USB_STATUS_LEN];
    osvc_session_usb_status(&s_session, usb_status);
    assert(usb_status[4] == OSVC_PHASE_RESETTING);
    /* The sender reads a new non-zero token as superseded; the protocol
     * has no all-zero token. */
    static const uint8_t zero[OSVC_TOKEN_LEN];
    assert(memcmp(usb_status + 8, zero, OSVC_TOKEN_LEN) != 0);
    assert(memcmp(usb_status + 8, token, OSVC_TOKEN_LEN) != 0);
    /* Bytes after the end are not read. */
    const uint8_t ping[8] = { 0, 0, 3, 0, 0, 0, 0, 0 };
    assert(osvc_session_receive(&s_session, ping, sizeof(ping)) == 0U);
    osvc_session_set_faulted(&s_session, true);
    osvc_session_usb_status(&s_session, usb_status);
    assert(usb_status[4] == OSVC_PHASE_FAULTED);
}

/* The device transport answers RESET itself and hands the token over; a
 * hello must carry exactly that token, and the end hook covers a stuck IN
 * endpoint. */
static void test_reset_with_token_and_end(void)
{
    setup();
    uint8_t token[OSVC_TOKEN_LEN];
    for (size_t i = 0U; i < sizeof(token); ++i) {
        token[i] = (uint8_t)(0xa0U + i);
    }
    uint8_t status[OSVC_USB_STATUS_LEN];
    osvc_session_usb_reset_with_token(&s_session, token, status);
    assert(status[4] == OSVC_PHASE_RESETTING);
    assert(memcmp(status + 8, token, OSVC_TOKEN_LEN) == 0);
    assert(s_fake.resets == 1U);
    osvc_session_transport_drained(&s_session);

    uint8_t hello[OSVC_HELLO_LEN];
    uint8_t other[OSVC_TOKEN_LEN];
    memcpy(other, token, sizeof(other));
    other[15] ^= 1U;
    make_hello(hello, 5U, 0U, other, 1U);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_HELLO, hello,
                 sizeof(hello));
    assert(!osvc_session_open(&s_session));
    make_hello(hello, 5U, 0U, token, 1U);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_HELLO, hello,
                 sizeof(hello));
    assert(osvc_session_open(&s_session));
    fake_message_t ready;
    assert(fake_next_message(&s_fake, &ready));
    check_ready(&ready, hello, token);

    osvc_session_end(&s_session, "the IN endpoint did not drain");
    assert(!osvc_session_open(&s_session));
    assert(s_fake.ended == 1U);
    assert(strcmp(s_fake.end_reason, "the IN endpoint did not drain") == 0);
    osvc_session_usb_status(&s_session, status);
    assert(status[4] == OSVC_PHASE_RESETTING);
}

/*
 * Terminal and file services. The sender's side is written here from the
 * OS Services protocol: it reads the KVM's messages field by field and
 * answers with its own encodings.
 */

static fake_message_t expect_message(uint16_t service, uint16_t kind)
{
    fake_message_t message;
    assert(fake_next_message(&s_fake, &message));
    assert(message.service == service && message.kind == kind);
    return message;
}

static void expect_no_message(void)
{
    fake_message_t message;
    assert(!fake_next_message(&s_fake, &message));
}

static void register_service(uint16_t service, uint32_t flags)
{
    uint8_t caps[4];
    osvc_put_u32(caps, flags);
    send_message(service, 1U, caps, sizeof(caps));
}

/* An open session with the given policy, the sender's shell and file
 * reception registered, and nothing written after session.config. */
static void open_services(bool terminal, bool files)
{
    fake_platform_init(&s_fake, &s_platform);
    osvc_settings_t settings = default_settings();
    settings.terminal_enabled = terminal;
    settings.files_enabled = files;
    osvc_session_init(&s_session, &s_platform, &settings, s_payload_buffer,
                      sizeof(s_payload_buffer));
    uint8_t token[OSVC_TOKEN_LEN];
    reset_and_admit(token);
    uint8_t hello[OSVC_HELLO_LEN];
    make_hello(hello, 5U, 0U, token, 7U);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_HELLO, hello, sizeof(hello));
    fake_message_t ready = expect_message(OSVC_SERVICE_SESSION,
                                          OSVC_SESSION_READY);
    check_ready(&ready, hello, token);
    fake_message_t config = expect_message(OSVC_SERVICE_SESSION,
                                           OSVC_SESSION_CONFIG);
    check_config(&config, terminal, files, "");
    register_service(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_FLAG_SHELL);
    register_service(OSVC_SERVICE_FILE, OSVC_FILE_FLAG_RECEIVE);
    /* Neither registration is answered (Service registration). */
    expect_no_message();
}

static void sender_terminal_status(uint16_t kind, uint32_t terminal_id,
                                   uint8_t status)
{
    uint8_t payload[5];
    osvc_put_u32(payload, terminal_id);
    payload[4] = status;
    send_message(OSVC_SERVICE_TERMINAL, kind, payload, sizeof(payload));
}

static void sender_output(uint32_t terminal_id, const void *data, size_t length)
{
    static uint8_t payload[OSVC_MAX_CONTROL_PAYLOAD];
    osvc_put_u32(payload, terminal_id);
    memcpy(payload + 4, data, length);
    send_message(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_OUTPUT, payload,
                 4U + length);
}

/* terminal.open and terminal.resize: session_id u32, rows u16, cols u16. */
static void check_terminal_size(const fake_message_t *message, uint16_t kind,
                                uint32_t terminal_id, uint16_t rows,
                                uint16_t cols)
{
    assert(message->service == OSVC_SERVICE_TERMINAL && message->kind == kind);
    assert(message->length == 8U);
    assert(osvc_get_u32(message->payload) == terminal_id);
    assert(osvc_get_u16(message->payload + 4) == rows);
    assert(osvc_get_u16(message->payload + 6) == cols);
}

/* The browser's channel opens; the sender sees terminal.open with size 0 by
 * 0, then the browser's size. */
static uint32_t browser_opens_terminal(uint32_t rows, uint32_t cols)
{
    uint32_t terminal_id = 77U;
    assert(osvc_session_terminal_open(&s_session, rows, cols, &terminal_id) ==
           NULL);
    assert(terminal_id != 0U);
    fake_message_t message = expect_message(OSVC_SERVICE_TERMINAL,
                                            OSVC_TERMINAL_OPEN);
    check_terminal_size(&message, OSVC_TERMINAL_OPEN, terminal_id, 0U, 0U);
    if (rows != 0U && cols != 0U) {
        message = expect_message(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_RESIZE);
        check_terminal_size(&message, OSVC_TERMINAL_RESIZE, terminal_id,
                            (uint16_t)rows, (uint16_t)cols);
    }
    expect_no_message();
    return terminal_id;
}

static uint32_t open_running_terminal(void)
{
    const uint32_t terminal_id = browser_opens_terminal(24U, 80U);
    sender_terminal_status(OSVC_TERMINAL_OPENED, terminal_id,
                           OSVC_TERMINAL_OPENED_OK);
    return terminal_id;
}

static void expect_terminal_closed(size_t index, uint32_t terminal_id,
                                   const char *reason, bool exited)
{
    assert(s_fake.terminal_closed_count > index);
    const fake_terminal_closed_t *event = &s_fake.terminal_closed[index];
    assert(event->terminal_id == terminal_id);
    if (strcmp(event->reason, reason) != 0) {
        fprintf(stderr, "terminal %u closed with \"%s\", expected \"%s\"\n",
                terminal_id, event->reason, reason);
        abort();
    }
    assert(event->exited == exited);
}

static void expect_terminal_close_sent(uint32_t terminal_id)
{
    const fake_message_t message = expect_message(OSVC_SERVICE_TERMINAL,
                                                  OSVC_TERMINAL_CLOSE);
    assert(message.length == 4U && osvc_get_u32(message.payload) == terminal_id);
}

static void test_terminal_offer_follows_registration_and_settings(void)
{
    /* No session, then a session without the shell registered. */
    setup();
    uint32_t terminal_id = 5U;
    assert(!osvc_session_terminal_available(&s_session));
    assert(strcmp(osvc_session_terminal_open(&s_session, 24U, 80U,
                                             &terminal_id),
                  "no OS Services terminal is connected") == 0);
    assert(terminal_id == 0U);
    uint8_t token[OSVC_TOKEN_LEN];
    open_session(token);
    assert(!osvc_session_terminal_available(&s_session));
    assert(osvc_session_terminal_open(&s_session, 0U, 0U, &terminal_id) != NULL);
    expect_no_message();
    /* getVideoState changes, so the platform hears of it. */
    unsigned changes = s_fake.state_changes;
    register_service(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_FLAG_SHELL);
    assert(osvc_session_terminal_available(&s_session));
    assert(s_fake.state_changes > changes);
    changes = s_fake.state_changes;

    /* Turning the setting off withdraws the offer and tells the sender;
     * turning it on tells the sender again, which registers again. */
    osvc_settings_t settings = default_settings();
    settings.terminal_enabled = false;
    osvc_session_apply_settings(&s_session, &settings);
    fake_message_t config = expect_message(OSVC_SERVICE_SESSION,
                                           OSVC_SESSION_CONFIG);
    check_config(&config, false, false, "");
    expect_no_message();
    assert(!osvc_session_terminal_available(&s_session));
    assert(s_fake.state_changes > changes);
    changes = s_fake.state_changes;
    assert(strcmp(osvc_session_terminal_open(&s_session, 0U, 0U, &terminal_id),
                  "no OS Services terminal is connected") == 0);
    expect_no_message();
    settings.terminal_enabled = true;
    osvc_session_apply_settings(&s_session, &settings);
    config = expect_message(OSVC_SERVICE_SESSION, OSVC_SESSION_CONFIG);
    check_config(&config, true, false, "");
    assert(osvc_session_terminal_available(&s_session));

    /* A withdrawal, and a sender that withdraws on its own. */
    changes = s_fake.state_changes;
    register_service(OSVC_SERVICE_TERMINAL, 0U);
    assert(!osvc_session_terminal_available(&s_session));
    assert(s_fake.state_changes > changes);
    assert(osvc_session_open(&s_session));
    register_service(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_FLAG_SHELL);
    assert(osvc_session_terminal_available(&s_session));
    expect_no_message();

    /* A platform that takes no output offers no terminal. */
    fake_platform_init(&s_fake, &s_platform);
    s_platform.terminal_output = NULL;
    osvc_session_init(&s_session, &s_platform, &settings, s_payload_buffer,
                      sizeof(s_payload_buffer));
    open_session(token);
    register_service(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_FLAG_SHELL);
    assert(s_session.terminal_shell);
    assert(!osvc_session_terminal_available(&s_session));
    assert(osvc_session_terminal_open(&s_session, 0U, 0U, &terminal_id) != NULL);
    expect_no_message();
}

static void test_terminal_round_trip(void)
{
    open_services(true, false);
    const uint32_t terminal_id = browser_opens_terminal(40U, 132U);
    sender_terminal_status(OSVC_TERMINAL_OPENED, terminal_id,
                           OSVC_TERMINAL_OPENED_OK);
    assert(s_fake.terminal_closed_count == 0U);

    /* A keystroke goes out at once, as one terminal.input. */
    assert(osvc_session_terminal_input(&s_session, terminal_id,
                                       (const uint8_t *)"ls\r", 3U));
    fake_message_t message = expect_message(OSVC_SERVICE_TERMINAL,
                                            OSVC_TERMINAL_INPUT);
    assert(message.length == 7U && osvc_get_u32(message.payload) == terminal_id &&
           memcmp(message.payload + 4, "ls\r", 3U) == 0);
    /* Nothing to send is not a message. */
    assert(osvc_session_terminal_input(&s_session, terminal_id, NULL, 0U));
    expect_no_message();

    /* Output reaches the browser in order, byte for byte. */
    sender_output(terminal_id, "file1  file2\r\n", 14U);
    sender_output(terminal_id, "$ ", 2U);
    assert(s_fake.terminal_output_length == 16U &&
           memcmp(s_fake.terminal_output, "file1  file2\r\n$ ", 16U) == 0);
    assert(s_fake.terminal_output_id == terminal_id);

    /* Resizes within 1 to 65535; anything else sends nothing. */
    assert(osvc_session_terminal_resize(&s_session, terminal_id, 50U, 200U));
    message = expect_message(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_RESIZE);
    check_terminal_size(&message, OSVC_TERMINAL_RESIZE, terminal_id, 50U, 200U);
    assert(osvc_session_terminal_resize(&s_session, terminal_id, 65535U,
                                        65535U));
    message = expect_message(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_RESIZE);
    check_terminal_size(&message, OSVC_TERMINAL_RESIZE, terminal_id, 65535U,
                        65535U);
    assert(!osvc_session_terminal_resize(&s_session, terminal_id, 0U, 80U));
    assert(!osvc_session_terminal_resize(&s_session, terminal_id, 24U, 0U));
    assert(!osvc_session_terminal_resize(&s_session, terminal_id, 65536U, 80U));
    assert(!osvc_session_terminal_resize(&s_session, terminal_id + 1U, 24U,
                                         80U));
    expect_no_message();

    /* The shell exits by itself: the one closing line the browser shows as
     * a success. */
    sender_terminal_status(OSVC_TERMINAL_EXITED, terminal_id,
                           OSVC_TERMINAL_EXIT_EXITED);
    assert(s_fake.terminal_closed_count == 1U);
    expect_terminal_closed(0U, terminal_id, "The shell exited", true);
    /* terminal.exited is the last message: nothing is sent, and late
     * messages for the ID are ignored. */
    expect_no_message();
    assert(!osvc_session_terminal_input(&s_session, terminal_id,
                                        (const uint8_t *)"x", 1U));
    sender_output(terminal_id, "late", 4U);
    sender_terminal_status(OSVC_TERMINAL_EXITED, terminal_id,
                           OSVC_TERMINAL_EXIT_FAILED);
    assert(s_fake.terminal_output_length == 16U);
    assert(s_fake.terminal_closed_count == 1U);
    assert(s_session.counters.service_faults == 0U);
    expect_no_message();

    /* Opened without a size: only terminal.open, which selects 24 by 80. */
    const uint32_t second = browser_opens_terminal(0U, 0U);
    assert(second != terminal_id);
    /* One of the two sizes zero: no resize either. */
    const uint32_t third = browser_opens_terminal(24U, 0U);
    assert(third != second && third != terminal_id);
}

static void test_terminal_close_and_exit_reasons(void)
{
    open_services(true, false);
    /* The browser closes: terminal.close, and no report back. */
    const uint32_t first = open_running_terminal();
    expect_no_message();
    osvc_session_terminal_close(&s_session, first);
    expect_terminal_close_sent(first);
    assert(s_fake.terminal_closed_count == 0U);
    /* The sender's answer arrives after the session ended here. */
    sender_terminal_status(OSVC_TERMINAL_EXITED, first,
                           OSVC_TERMINAL_EXIT_CLOSED);
    assert(s_fake.terminal_closed_count == 0U);
    osvc_session_terminal_close(&s_session, first);
    expect_no_message();

    /* The sender loses the shell, or ends it after a withdrawal. */
    const uint32_t lost = open_running_terminal();
    const uint32_t closed = open_running_terminal();
    assert(lost != first && closed != lost);
    sender_terminal_status(OSVC_TERMINAL_EXITED, lost, OSVC_TERMINAL_EXIT_FAILED);
    sender_terminal_status(OSVC_TERMINAL_EXITED, closed,
                           OSVC_TERMINAL_EXIT_CLOSED);
    expect_terminal_closed(0U, lost, "The target computer lost its shell", false);
    expect_terminal_closed(1U, closed, "", false);
    expect_no_message();

    /* Refusals end the session without a close: only OK creates one. */
    const uint8_t refusals[3] = { OSVC_TERMINAL_OPENED_UNSUPPORTED,
                                  OSVC_TERMINAL_OPENED_BUSY,
                                  OSVC_TERMINAL_OPENED_FAILED };
    const char *lines[3] = {
        "Terminal unavailable: the target computer allows no remote terminal",
        ("Terminal unavailable: the target computer already runs its maximum "
         "number of terminals"),
        "Terminal unavailable: the target computer could not start a shell",
    };
    for (size_t i = 0U; i < 3U; ++i) {
        const uint32_t terminal_id = browser_opens_terminal(24U, 80U);
        sender_terminal_status(OSVC_TERMINAL_OPENED, terminal_id, refusals[i]);
        expect_terminal_closed(2U + i, terminal_id, lines[i], false);
        expect_no_message();
    }
}

static void test_terminal_open_timeout_and_limit(void)
{
    open_services(true, false);
    const uint32_t slow = browser_opens_terminal(24U, 80U);
    s_fake.now += OSVC_TERMINAL_OPEN_TIMEOUT_MS - 1;
    osvc_session_tick(&s_session);
    expect_no_message();
    assert(s_fake.terminal_closed_count == 0U);
    s_fake.now += 1;
    osvc_session_tick(&s_session);
    /* The answer may still be on its way with a shell started: close it. */
    expect_terminal_close_sent(slow);
    expect_terminal_closed(0U, slow,
                           "Terminal unavailable: the target computer did not answer",
                           false);
    sender_terminal_status(OSVC_TERMINAL_OPENED, slow, OSVC_TERMINAL_OPENED_OK);
    sender_output(slow, "x", 1U);
    assert(s_fake.terminal_output_calls == 0U);
    osvc_session_tick(&s_session);
    expect_no_message();
    assert(s_fake.terminal_closed_count == 1U);

    /* An answered session has no deadline. */
    uint32_t ids[OSVC_TERMINAL_SESSIONS_MAX];
    for (size_t i = 0U; i < OSVC_TERMINAL_SESSIONS_MAX; ++i) {
        ids[i] = open_running_terminal();
    }
    s_fake.now += 60000;
    for (size_t i = 0U; i < OSVC_TERMINAL_SESSIONS_MAX; ++i) {
        send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_PING, NULL, 0U);
        osvc_session_tick(&s_session);
    }
    assert(s_fake.terminal_closed_count == 1U);
    /* A fifth terminal is refused here, with the sender's own busy line. */
    uint32_t fifth = 9U;
    assert(strcmp(osvc_session_terminal_open(&s_session, 24U, 80U, &fifth),
                  "Terminal unavailable: the target computer already runs its "
                  "maximum number of terminals") == 0);
    assert(fifth == 0U);
    expect_no_message();
    osvc_session_terminal_close(&s_session, ids[2]);
    expect_terminal_close_sent(ids[2]);
    fifth = browser_opens_terminal(24U, 80U);
    for (size_t i = 0U; i < OSVC_TERMINAL_SESSIONS_MAX; ++i) {
        assert(fifth != ids[i]);
    }
}

static void test_terminal_data_limits(void)
{
    open_services(true, false);
    const uint32_t terminal_id = open_running_terminal();
    /* 65532 bytes of keystrokes fill one payload of exactly 64 KiB; one
     * byte more needs a second message. */
    static uint8_t keys[2U * OSVC_TERMINAL_MAX_DATA + 5U];
    for (size_t i = 0U; i < sizeof(keys); ++i) {
        keys[i] = (uint8_t)(i * 7U + 1U);
    }
    assert(osvc_session_terminal_input(&s_session, terminal_id, keys,
                                       OSVC_TERMINAL_MAX_DATA));
    fake_message_t message = expect_message(OSVC_SERVICE_TERMINAL,
                                            OSVC_TERMINAL_INPUT);
    assert(message.length == OSVC_MAX_CONTROL_PAYLOAD);
    assert(memcmp(message.payload + 4, keys, OSVC_TERMINAL_MAX_DATA) == 0);
    expect_no_message();
    assert(osvc_session_terminal_input(&s_session, terminal_id, keys,
                                       sizeof(keys)));
    size_t received = 0U;
    for (int i = 0; i < 3; ++i) {
        message = expect_message(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_INPUT);
        assert(message.length > 4U && message.length <= OSVC_MAX_CONTROL_PAYLOAD);
        assert(osvc_get_u32(message.payload) == terminal_id);
        assert(memcmp(message.payload + 4, keys + received,
                      message.length - 4U) == 0);
        received += message.length - 4U;
    }
    assert(received == sizeof(keys));
    expect_no_message();
    /* One byte under and one byte over a full message. */
    assert(osvc_session_terminal_input(&s_session, terminal_id, keys,
                                       OSVC_TERMINAL_MAX_DATA - 1U));
    message = expect_message(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_INPUT);
    assert(message.length == OSVC_MAX_CONTROL_PAYLOAD - 1U);
    assert(memcmp(message.payload + 4, keys, OSVC_TERMINAL_MAX_DATA - 1U) == 0);
    expect_no_message();
    assert(osvc_session_terminal_input(&s_session, terminal_id, keys,
                                       OSVC_TERMINAL_MAX_DATA + 1U));
    message = expect_message(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_INPUT);
    assert(message.length == OSVC_MAX_CONTROL_PAYLOAD);
    message = expect_message(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_INPUT);
    assert(message.length == 5U && osvc_get_u32(message.payload) == terminal_id &&
           message.payload[4] == keys[OSVC_TERMINAL_MAX_DATA]);
    expect_no_message();

    /* Output of the largest size is delivered whole. */
    static uint8_t screen[OSVC_TERMINAL_MAX_DATA];
    memset(screen, 'o', sizeof(screen));
    sender_output(terminal_id, screen, sizeof(screen));
    assert(s_fake.terminal_output_length == sizeof(screen));
    assert(s_session.counters.service_faults == 0U);
    /* A payload_size one over 64 KiB ends the session at the envelope. */
    uint8_t envelope[OSVC_ENVELOPE_LEN];
    osvc_put_envelope(envelope, OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_OUTPUT,
                      OSVC_MAX_CONTROL_PAYLOAD + 1U);
    feed(envelope, sizeof(envelope));
    assert(!osvc_session_open(&s_session));
    expect_terminal_closed(0U, terminal_id, "OS Services disconnected", false);
}

/* Each invalid sender message faults the terminal service alone: every
 * terminal ends here and on the sender, the session and the clipboard
 * stay, and the service's messages count only after a new registration. */
static void test_terminal_invalid_messages_fault_the_service(void)
{
    struct {
        uint16_t kind;
        uint8_t payload[8];
        size_t length;
        const char *reason;
    } cases[] = {
        { OSVC_TERMINAL_OPENED, { 1, 0, 0, 0, 0, 0 }, 6U,
          "invalid terminal.opened" },
        { OSVC_TERMINAL_OPENED, { 1, 0, 0, 0, 4 }, 5U,
          "invalid terminal.opened" },
        { OSVC_TERMINAL_OPENED, { 0, 0, 0, 0, 0 }, 5U,
          "invalid terminal.opened" },
        { OSVC_TERMINAL_OUTPUT, { 1, 0, 0, 0 }, 4U, "invalid terminal.output" },
        { OSVC_TERMINAL_OUTPUT, { 0, 0, 0, 0, 'x' }, 5U,
          "invalid terminal.output" },
        { OSVC_TERMINAL_EXITED, { 1, 0, 0, 0, 3 }, 5U,
          "invalid terminal.exited" },
        { OSVC_TERMINAL_EXITED, { 1, 0, 0 }, 3U, "invalid terminal.exited" },
        { OSVC_TERMINAL_CAPABILITIES, { 2, 0, 0, 0 }, 4U,
          "invalid capabilities" },
        { OSVC_TERMINAL_CAPABILITIES, { 1, 0, 0, 0, 0 }, 5U,
          "invalid capabilities" },
        { OSVC_TERMINAL_OPEN, { 1, 0, 0, 0, 0, 0, 0, 0 }, 8U,
          "a KVM-to-sender terminal message from the sender" },
        { OSVC_TERMINAL_INPUT, { 1, 0, 0, 0, 'x' }, 5U,
          "a KVM-to-sender terminal message from the sender" },
        { OSVC_TERMINAL_RESIZE, { 1, 0, 0, 0, 24, 0, 80, 0 }, 8U,
          "a KVM-to-sender terminal message from the sender" },
        { OSVC_TERMINAL_CLOSE, { 1, 0, 0, 0 }, 4U,
          "a KVM-to-sender terminal message from the sender" },
    };
    for (size_t c = 0U; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        open_services(true, false);
        register_clipboard(OSVC_CLIPBOARD_FLAG_READ, OSVC_OS_LINUX);
        expect_clipboard_config(1U);
        const uint32_t a = open_running_terminal();
        const uint32_t b = browser_opens_terminal(24U, 80U);
        send_message(OSVC_SERVICE_TERMINAL, cases[c].kind, cases[c].payload,
                     cases[c].length);
        assert(osvc_session_open(&s_session));
        assert(s_session.counters.service_faults == 1U);
        assert(!osvc_session_terminal_available(&s_session));
        assert(osvc_session_clipboard_available(&s_session));
        char line[160];
        snprintf(line, sizeof(line), "Terminal faulted: %s",
                 cases[c].reason);
        assert(s_fake.terminal_closed_count == 2U);
        expect_terminal_closed(0U, a, line, false);
        expect_terminal_closed(1U, b, line, false);
        expect_terminal_close_sent(a);
        expect_terminal_close_sent(b);
        expect_no_message();
        /* Ignored until it registers again, even a valid message. */
        sender_output(a, "x", 1U);
        send_message(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_OPENED,
                     cases[c].payload, cases[c].length);
        assert(s_session.counters.service_faults == 1U);
        register_service(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_FLAG_SHELL);
        assert(osvc_session_terminal_available(&s_session));
        const uint32_t again = open_running_terminal();
        sender_output(again, "ok", 2U);
        assert(s_fake.terminal_output_length == 2U);
    }
}

/* terminal.output for terminal_id, as the sender frames it, into out. */
static size_t encode_output(uint8_t *out, uint32_t terminal_id,
                            const char *text)
{
    uint8_t payload[64];
    const size_t length = strlen(text);
    osvc_put_u32(payload, terminal_id);
    memcpy(payload + 4, text, length);
    return encode(out, OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_OUTPUT, payload,
                  4U + length);
}

static void test_terminal_output_waits_for_the_browser(void)
{
    open_services(true, false);
    const uint32_t a = open_running_terminal();
    const uint32_t b = open_running_terminal();
    /* One OUT read with output for both terminals and a ping. */
    uint8_t read[256];
    const size_t first = encode_output(read, a, "slow");
    size_t length = first + encode_output(read + first, b, "fine");
    length += encode(read + length, OSVC_SERVICE_SESSION, OSVC_SESSION_PING,
                     NULL, 0U);

    /* The browser's channel is full: the session takes the first message
     * and nothing after it, and offers it again on each tick. */
    s_fake.backed_up = true;
    assert(osvc_session_receive(&s_session, read, length) == first);
    assert(osvc_session_holding(&s_session));
    assert(osvc_session_receive(&s_session, read + first, length - first) ==
           0U);
    s_fake.now += OSVC_TERMINAL_OUTPUT_HOLD_MS - 1;
    osvc_session_tick(&s_session);
    assert(s_fake.terminal_output_calls == 3U);
    assert(osvc_session_holding(&s_session));

    /* It has room: the held output goes first, then the rest of the read. */
    s_fake.backed_up = false;
    feed(read + first, length - first);
    assert(!osvc_session_holding(&s_session));
    assert(s_fake.terminal_output_length == 8U &&
           memcmp(s_fake.terminal_output, "slowfine", 8U) == 0);
    assert(s_fake.terminal_closed_count == 0U);
    expect_no_message();

    /* The last message of a read waits for the tick alone. */
    s_fake.backed_up = true;
    feed(read, first);
    assert(osvc_session_holding(&s_session));
    s_fake.backed_up = false;
    osvc_session_tick(&s_session);
    assert(!osvc_session_holding(&s_session));
    assert(s_fake.terminal_output_length == 12U);

    /* A browser that takes nothing for OSVC_TERMINAL_OUTPUT_HOLD_MS: the
     * terminal ends on both sides and the session reads on. */
    s_fake.backed_up = true;
    assert(osvc_session_receive(&s_session, read, length) == first);
    s_fake.now += OSVC_TERMINAL_OUTPUT_HOLD_MS;
    osvc_session_tick(&s_session);
    assert(!osvc_session_holding(&s_session));
    expect_terminal_close_sent(a);
    expect_terminal_closed(0U, a,
                           "Terminal closed: the browser did not keep up",
                           false);
    s_fake.backed_up = false;
    feed(read + first, length - first);
    assert(s_fake.terminal_output_length == 16U);
    assert(osvc_session_open(&s_session));
    expect_no_message();
}

/* A reply behind held output is read once the output goes, before any
 * request it answers times out. */
static void test_requests_wait_while_output_is_held(void)
{
    open_services(true, false);
    register_clipboard(OSVC_CLIPBOARD_FLAG_READ, OSVC_OS_LINUX);
    expect_clipboard_config(1U);
    const uint32_t a = open_running_terminal();
    osvc_clipboard_status_t status;
    const uint32_t get = osvc_session_clipboard_request(&s_session, NULL, 0U,
                                                        &status);
    assert(get != 0U);
    (void)expect_message(OSVC_SERVICE_CLIPBOARD, OSVC_CLIPBOARD_GET);
    s_fake.now += 100;
    uint8_t read[128];
    const size_t first = encode_output(read, a, "slow");
    uint8_t content[8 + 2] = {0};
    osvc_put_u32(content, get);
    content[4] = OSVC_CLIPBOARD_OK;
    memcpy(content + 8, "hi", 2U);
    const size_t length = first + encode(read + first, OSVC_SERVICE_CLIPBOARD,
                                         OSVC_CLIPBOARD_CONTENT, content,
                                         sizeof(content));
    s_fake.backed_up = true;
    assert(osvc_session_receive(&s_session, read, length) == first);
    /* The request's deadline passes while its answer waits behind the
     * output. */
    s_fake.now += OSVC_TERMINAL_OUTPUT_HOLD_MS - 1;
    osvc_session_tick(&s_session);
    s_fake.backed_up = false;
    osvc_session_tick(&s_session);
    assert(!osvc_session_holding(&s_session));
    feed(read + first, length - first);
    osvc_session_tick(&s_session);
    assert(s_fake.clipboard_count == 1U && s_fake.clipboard[0].answered &&
           s_fake.clipboard[0].status == OSVC_CLIPBOARD_OK &&
           strcmp(s_fake.clipboard[0].text, "hi") == 0);
    assert(s_fake.terminal_closed_count == 0U);
    expect_no_message();
}

static void test_terminal_ends_with_browser_settings_and_session(void)
{
    /* A browser that cannot keep up: the terminal ends on both sides. */
    open_services(true, false);
    const uint32_t slow = open_running_terminal();
    const uint32_t other = open_running_terminal();
    s_fake.backed_up = true;
    sender_output(slow, "flood", 5U);
    s_fake.now += OSVC_TERMINAL_OUTPUT_HOLD_MS;
    osvc_session_tick(&s_session);
    expect_terminal_close_sent(slow);
    expect_terminal_closed(0U, slow,
                           "Terminal closed: the browser did not keep up",
                           false);
    s_fake.backed_up = false;
    sender_output(other, "fine", 4U);
    assert(s_fake.terminal_output_length == 4U);

    /* The setting goes off: each terminal ends with its line, no close (the
     * sender ends its shells on session.config), then the config. */
    const uint32_t third = open_running_terminal();
    osvc_settings_t settings = default_settings();
    settings.terminal_enabled = false;
    osvc_session_apply_settings(&s_session, &settings);
    assert(s_fake.terminal_closed_count == 3U);
    /* In slot order: the third terminal took the slow one's slot. */
    const bool other_first = s_fake.terminal_closed[1].terminal_id == other;
    expect_terminal_closed(other_first ? 1U : 2U, other,
                           "Terminal disabled", false);
    expect_terminal_closed(other_first ? 2U : 1U, third,
                           "Terminal disabled", false);
    fake_message_t config = expect_message(OSVC_SERVICE_SESSION,
                                           OSVC_SESSION_CONFIG);
    check_config(&config, false, false, "");
    expect_no_message();
    sender_terminal_status(OSVC_TERMINAL_EXITED, other,
                           OSVC_TERMINAL_EXIT_CLOSED);
    register_service(OSVC_SERVICE_TERMINAL, 0U);
    assert(s_fake.terminal_closed_count == 3U);
    expect_no_message();

    /* The session ends: every terminal ends once, nothing is written. */
    open_services(true, false);
    const uint32_t opening = browser_opens_terminal(24U, 80U);
    const uint32_t running = open_running_terminal();
    osvc_session_end(&s_session, "the IN endpoint did not drain");
    assert(s_fake.terminal_closed_count == 2U);
    expect_terminal_closed(0U, opening, "OS Services disconnected", false);
    expect_terminal_closed(1U, running, "OS Services disconnected", false);
    expect_no_message();
    osvc_session_end(&s_session, "again");
    osvc_session_tick(&s_session);
    assert(s_fake.terminal_closed_count == 2U);
    assert(!osvc_session_terminal_input(&s_session, running,
                                        (const uint8_t *)"x", 1U));

    /* Keystrokes that cannot be queued end their terminal with its own line
     * and then the session, which ends the others. */
    open_services(true, false);
    const uint32_t typing = open_running_terminal();
    const uint32_t idle = open_running_terminal();
    s_fake.fail_writes = true;
    assert(!osvc_session_terminal_input(&s_session, typing,
                                        (const uint8_t *)"rm -rf build\r", 13U));
    assert(!osvc_session_open(&s_session));
    assert(s_fake.terminal_closed_count == 2U);
    expect_terminal_closed(0U, typing,
                           "Terminal closed: keystrokes were not delivered",
                           false);
    expect_terminal_closed(1U, idle, "OS Services disconnected", false);

    /* A withdrawal that finds a terminal still opening here. */
    open_services(true, false);
    const uint32_t racing = browser_opens_terminal(24U, 80U);
    register_service(OSVC_SERVICE_TERMINAL, 0U);
    expect_terminal_closed(0U, racing,
                           "Terminal closed: the target computer withdrew its shell",
                           false);
    sender_terminal_status(OSVC_TERMINAL_OPENED, racing,
                           OSVC_TERMINAL_OPENED_UNSUPPORTED);
    assert(s_fake.terminal_closed_count == 1U);
    expect_no_message();
}

static osvc_file_entry_t file_entry(const char *name, uint64_t size)
{
    return (osvc_file_entry_t) { .name = name, .name_length = strlen(name),
                                 .size = size };
}

/* The sender reads file.drop as the protocol lays it out: file_count, then
 * per file transfer_id, size, name_length and name, and nothing after. */
static void check_drop(const osvc_file_entry_t *files, size_t count,
                       const uint32_t *transfer_ids)
{
    const fake_message_t message = expect_message(OSVC_SERVICE_FILE,
                                                  OSVC_FILE_DROP);
    assert(message.length >= 4U && osvc_get_u32(message.payload) == count);
    size_t o = 4U;
    for (size_t i = 0U; i < count; ++i) {
        assert(o + 14U <= message.length);
        const uint32_t id = osvc_get_u32(message.payload + o);
        assert(id != 0U && id == transfer_ids[i]);
        for (size_t k = 0U; k < i; ++k) {
            assert(transfer_ids[k] != id);
        }
        assert(osvc_get_u64(message.payload + o + 4) == files[i].size);
        const size_t name_length = osvc_get_u16(message.payload + o + 12);
        assert(name_length == files[i].name_length);
        assert(o + 14U + name_length <= message.length);
        assert(memcmp(message.payload + o + 14, files[i].name, name_length) ==
               0);
        o += 14U + name_length;
    }
    assert(o == message.length);
}

static uint32_t browser_drops(const osvc_file_entry_t *files, size_t count,
                              uint32_t *transfer_ids)
{
    uint32_t drop_id = 0U;
    const char *refused = osvc_session_file_drop(&s_session, files, count,
                                                 &drop_id, transfer_ids);
    if (refused != NULL) {
        fprintf(stderr, "drop refused: %s\n", refused);
        abort();
    }
    assert(drop_id != 0U);
    check_drop(files, count, transfer_ids);
    for (size_t i = 0U; i < count; ++i) {
        assert(transfer_ids[i] != drop_id);
    }
    expect_no_message();
    return drop_id;
}

static void sender_credit(uint16_t kind, uint32_t transfer_id, uint32_t bytes)
{
    uint8_t payload[8];
    osvc_put_u32(payload, transfer_id);
    osvc_put_u32(payload + 4, bytes);
    send_message(OSVC_SERVICE_FILE, kind, payload, sizeof(payload));
}

static void sender_done(uint32_t transfer_id, uint8_t status, const char *path)
{
    uint8_t payload[8 + 128] = {0};
    osvc_put_u32(payload, transfer_id);
    payload[4] = status;
    const size_t length = path != NULL ? strlen(path) : 0U;
    memcpy(payload + 8, path != NULL ? path : "", length);
    send_message(OSVC_SERVICE_FILE, OSVC_FILE_DONE, payload, 8U + length);
}

static void expect_chunk(uint32_t transfer_id, uint64_t offset,
                         const uint8_t *data, size_t length)
{
    const fake_message_t message = expect_message(OSVC_SERVICE_FILE,
                                                  OSVC_FILE_CHUNK);
    assert(message.length == 12U + length);
    assert(osvc_get_u32(message.payload) == transfer_id);
    assert(osvc_get_u64(message.payload + 4) == offset);
    assert(memcmp(message.payload + 12, data, length) == 0);
}

static void expect_cancel_sent(uint32_t transfer_id)
{
    const fake_message_t message = expect_message(OSVC_SERVICE_FILE,
                                                  OSVC_FILE_CANCEL);
    assert(message.length == 4U && osvc_get_u32(message.payload) == transfer_id);
}

static void expect_credit_event(size_t index, uint32_t drop_id,
                                uint32_t transfer_id, uint64_t bytes)
{
    assert(s_fake.file_count > index);
    const fake_file_event_t *event = &s_fake.file[index];
    assert(!event->done && event->drop_id == drop_id &&
           event->transfer_id == transfer_id && event->bytes == bytes);
}

static void expect_done_event(size_t index, uint32_t drop_id,
                              uint32_t transfer_id, osvc_file_status_t status,
                              const char *path, bool last)
{
    assert(s_fake.file_count > index);
    const fake_file_event_t *event = &s_fake.file[index];
    assert(event->done && event->drop_id == drop_id &&
           event->transfer_id == transfer_id && event->status == status &&
           event->last == last);
    assert(event->path_length == strlen(path) &&
           memcmp(event->path, path, event->path_length) == 0);
}

static void test_file_offer_follows_registration_and_settings(void)
{
    /* The default policy has files off: registered or not, no drop. */
    setup();
    uint8_t token[OSVC_TOKEN_LEN];
    open_session(token);
    register_service(OSVC_SERVICE_FILE, OSVC_FILE_FLAG_RECEIVE);
    assert(s_session.file_receive);
    assert(!osvc_session_file_available(&s_session));
    const osvc_file_entry_t one = file_entry("a.txt", 3U);
    uint32_t drop_id = 1U;
    uint32_t ids[OSVC_FILE_DROP_MAX + 1U];
    assert(strcmp(osvc_session_file_drop(&s_session, &one, 1U, &drop_id, ids),
                  "no OS Services file transfer is connected") == 0);
    assert(drop_id == 0U);
    expect_no_message();

    /* Turned on: the sender hears it, and the offer follows. */
    osvc_settings_t settings = default_settings();
    settings.files_enabled = true;
    osvc_session_apply_settings(&s_session, &settings);
    fake_message_t config = expect_message(OSVC_SERVICE_SESSION,
                                           OSVC_SESSION_CONFIG);
    check_config(&config, true, true, "");
    assert(osvc_session_file_available(&s_session));
    /* Withdrawn and registered again. */
    register_service(OSVC_SERVICE_FILE, 0U);
    assert(!osvc_session_file_available(&s_session));
    register_service(OSVC_SERVICE_FILE, OSVC_FILE_FLAG_RECEIVE);
    assert(osvc_session_file_available(&s_session));
    expect_no_message();
    /* Turned off again. */
    settings.files_enabled = false;
    osvc_session_apply_settings(&s_session, &settings);
    config = expect_message(OSVC_SERVICE_SESSION, OSVC_SESSION_CONFIG);
    check_config(&config, true, false, "");
    assert(!osvc_session_file_available(&s_session));
    expect_no_message();

    /* A platform that relays no outcomes offers no file reception. */
    fake_platform_init(&s_fake, &s_platform);
    s_platform.file_done = NULL;
    settings.files_enabled = true;
    osvc_session_init(&s_session, &s_platform, &settings, s_payload_buffer,
                      sizeof(s_payload_buffer));
    reset_and_admit(token);
    uint8_t hello[OSVC_HELLO_LEN];
    make_hello(hello, 5U, 0U, token, 1U);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_HELLO, hello, sizeof(hello));
    register_service(OSVC_SERVICE_FILE, OSVC_FILE_FLAG_RECEIVE);
    assert(s_session.file_receive && !osvc_session_file_available(&s_session));
}

static void test_file_drop_limits(void)
{
    open_services(false, true);
    static uint32_t ids[OSVC_FILE_DROP_MAX + 1U];
    static osvc_file_entry_t files[OSVC_FILE_DROP_MAX + 1U];
    uint32_t drop_id = 0U;

    /* Names: one path component without a control character. */
    const char *invalid[] = {
        "", ".", "..", "a/b", "a\\b", "/", "tab\tname", "line\nbreak",
        "nul\x01", "del\x7f", "c1\xc2\x85", "c1\xc2\x9f", "bad\xff",
        "overlong\xc0\xae", "surrogate\xed\xa0\x80", "cut\xe2\x82",
    };
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        files[0] = file_entry(invalid[i], 1U);
        const char *refused = osvc_session_file_drop(&s_session, files, 1U,
                                                     &drop_id, ids);
        assert(refused != NULL && strncmp(refused, "invalid file name \"",
                                          19U) == 0);
        assert(drop_id == 0U);
    }
    /* The text names the file, escaped, so it stays one line of UTF-8. */
    files[0] = file_entry("a\nb\"\xff", 1U);
    assert(strcmp(osvc_session_file_drop(&s_session, files, 1U, &drop_id, ids),
                  "invalid file name \"a\\x0ab\\\"\\xff\"") == 0);
    expect_no_message();
    assert(strcmp(osvc_session_file_drop(&s_session, files, 0U, &drop_id, ids),
                  "file.drop needs at least one file") == 0);

    /* Names that look odd but are one component. */
    const char *valid[] = { "...", ".env", "a..b", "r\xc3\xa9sum\xc3\xa9.pdf",
                            "a b", "\xf0\x9f\x93\x84", "x\xc2\xa0y" };
    for (size_t i = 0U; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        files[i] = file_entry(valid[i], (uint64_t)i);
    }
    const size_t valid_count = sizeof(valid) / sizeof(valid[0]);
    drop_id = browser_drops(files, valid_count, ids);
    /* One drop at a time. */
    uint32_t other = 0U;
    assert(strcmp(osvc_session_file_drop(&s_session, files, 1U, &other, ids),
                  "a file transfer is already in progress") == 0);
    expect_no_message();
    osvc_session_file_channel_closed(&s_session, drop_id);
    for (size_t i = 0U; i < valid_count; ++i) {
        expect_cancel_sent(drop_id + 1U + (uint32_t)i);
    }
    expect_no_message();

    /* The largest drop the KVM takes, and one file more. */
    char names[OSVC_FILE_DROP_MAX + 1U][8];
    for (size_t i = 0U; i <= OSVC_FILE_DROP_MAX; ++i) {
        snprintf(names[i], sizeof(names[i]), "f%zu", i);
        files[i] = file_entry(names[i], (uint64_t)i);
    }
    assert(strcmp(osvc_session_file_drop(&s_session, files,
                                         OSVC_FILE_DROP_MAX + 1U, &drop_id,
                                         ids),
                  "too many files for one drop: 257 (at most 256)") == 0);
    expect_no_message();
    drop_id = browser_drops(files, OSVC_FILE_DROP_MAX, ids);
    osvc_session_file_channel_closed(&s_session, drop_id);
    fake_clear_output(&s_fake);

    /* The file.drop payload: one name filling 64 KiB exactly, then one
     * byte more. */
    static char long_name[OSVC_MAX_CONTROL_PAYLOAD];
    memset(long_name, 'n', sizeof(long_name));
    const size_t longest = OSVC_MAX_CONTROL_PAYLOAD - 4U - 14U;
    files[0] = (osvc_file_entry_t) { .name = long_name,
                                     .name_length = longest + 1U,
                                     .size = 1U };
    assert(strcmp(osvc_session_file_drop(&s_session, files, 1U, &drop_id, ids),
                  "too many file names for one drop: 65537 bytes") == 0);
    expect_no_message();
    files[0].name_length = longest;
    drop_id = browser_drops(files, 1U, ids);
    osvc_session_file_channel_closed(&s_session, drop_id);
    fake_clear_output(&s_fake);
    /* Spread over two files, the same limit. */
    files[0] = (osvc_file_entry_t) { .name = long_name, .name_length = 30000U,
                                     .size = 0U };
    files[1] = (osvc_file_entry_t) { .name = long_name,
                                     .name_length = longest - 30000U - 14U + 1U,
                                     .size = UINT64_MAX };
    assert(osvc_session_file_drop(&s_session, files, 2U, &drop_id, ids) != NULL);
    files[1].name_length--;
    drop_id = browser_drops(files, 2U, ids);
    expect_no_message();
}

static void test_file_transfer_and_credit(void)
{
    open_services(false, true);
    static uint8_t data[300000];
    for (size_t i = 0U; i < sizeof(data); ++i) {
        data[i] = (uint8_t)(i * 31U + 7U);
    }
    const osvc_file_entry_t files[2] = { file_entry("big.bin", 200000U),
                                         file_entry("small.txt", 5U) };
    uint32_t ids[2];
    const uint32_t drop_id = browser_drops(files, 2U, ids);
    /* Before the channel opens, the sender's credit waits. */
    sender_credit(OSVC_FILE_READY, ids[0], 131072U);
    assert(s_fake.file_count == 0U);
    assert(!osvc_session_file_attach(&s_session, drop_id + 1000U));
    assert(osvc_session_file_attach(&s_session, drop_id));
    assert(!osvc_session_file_attach(&s_session, drop_id));
    expect_credit_event(0U, drop_id, ids[0], 131072U);
    assert(s_fake.file_count == 1U);

    /* Bytes within the credit go out at once, at the next offset. */
    assert(osvc_session_file_data(&s_session, drop_id, ids[0], data, 32768U));
    expect_chunk(ids[0], 0U, data, 32768U);
    assert(osvc_session_file_data(&s_session, drop_id, ids[0], data + 32768,
                                  98304U));
    /* 98304 bytes: one chunk of 65524 and the rest. */
    expect_chunk(ids[0], 32768U, data + 32768, OSVC_CHUNK_MAX_DATA);
    expect_chunk(ids[0], 32768U + OSVC_CHUNK_MAX_DATA,
                 data + 32768 + OSVC_CHUNK_MAX_DATA,
                 98304U - OSVC_CHUNK_MAX_DATA);
    expect_no_message();
    /* The credit is used up exactly. */
    assert(!osvc_session_file_data(&s_session, drop_id, 0U, data, 1U));
    assert(!osvc_session_file_data(&s_session, drop_id, ids[0], data, 0U));
    assert(!osvc_session_file_data(&s_session, drop_id + 1U, ids[0], data, 1U));
    assert(!osvc_session_file_data(&s_session, drop_id, ids[1] + 1U, data, 1U));
    expect_no_message();
    /* The sender returns credit; it reaches the browser one to one, and
     * the file's last byte may be sent. */
    sender_credit(OSVC_FILE_CREDIT, ids[0], 65536U);
    sender_credit(OSVC_FILE_CREDIT, ids[0], 4096U);
    expect_credit_event(1U, drop_id, ids[0], 65536U);
    expect_credit_event(2U, drop_id, ids[0], 4096U);
    assert(osvc_session_file_data(&s_session, drop_id, ids[0], data + 131072,
                                  200000U - 131072U));
    expect_chunk(ids[0], 131072U, data + 131072, OSVC_CHUNK_MAX_DATA);
    expect_chunk(ids[0], 131072U + OSVC_CHUNK_MAX_DATA,
                 data + 131072 + OSVC_CHUNK_MAX_DATA,
                 200000U - 131072U - OSVC_CHUNK_MAX_DATA);
    sender_done(ids[0], OSVC_FILE_DONE_OK, "/home/u/Downloads/big.bin");
    expect_done_event(3U, drop_id, ids[0], OSVC_FILE_DONE_OK,
                      "/home/u/Downloads/big.bin", false);
    /* Credit for a transfer that ended is not passed on. */
    sender_credit(OSVC_FILE_CREDIT, ids[0], 1U);
    assert(s_fake.file_count == 4U);

    /* The second file: its size bounds the bytes whatever the credit. */
    sender_credit(OSVC_FILE_READY, ids[1], 524288U);
    expect_credit_event(4U, drop_id, ids[1], 524288U);
    assert(!osvc_session_file_data(&s_session, drop_id, ids[1], data, 6U));
    /* That failed the drop: both sides learn of it exactly once. */
    expect_cancel_sent(ids[1]);
    expect_no_message();
    assert(s_fake.file_count == 6U);
    expect_done_event(5U, drop_id, ids[1], OSVC_FILE_DONE_FAILED, "", true);
    assert(!osvc_session_file_data(&s_session, drop_id, ids[1], data, 5U));
    assert(!osvc_session_file_cancel(&s_session, drop_id, ids[1]));
    sender_done(ids[1], OSVC_FILE_DONE_CANCELLED, NULL);
    assert(s_fake.file_count == 6U);
    assert(s_session.counters.service_faults == 0U);
    assert(!osvc_session_file_attach(&s_session, drop_id));

    /* The next drop takes new IDs. */
    uint32_t next[1];
    const uint32_t next_drop = browser_drops(&files[1], 1U, next);
    assert(next_drop > ids[1] && next[0] > next_drop);
}

/* Browser data is cut into file.chunk messages of at most 65524 bytes: a
 * payload of at most 64 KiB with the 12-byte chunk header. */
static void test_file_chunk_boundaries(void)
{
    open_services(false, true);
    enum { MAX = OSVC_CHUNK_MAX_DATA };
    static uint8_t data[3U * MAX];
    for (size_t i = 0U; i < sizeof(data); ++i) {
        data[i] = (uint8_t)(i * 13U + 5U);
    }
    const osvc_file_entry_t file = file_entry("cuts.bin", sizeof(data));
    uint32_t id;
    const uint32_t drop_id = browser_drops(&file, 1U, &id);
    assert(osvc_session_file_attach(&s_session, drop_id));
    sender_credit(OSVC_FILE_READY, id, (uint32_t)sizeof(data));
    /* One under, exactly, and one over the largest chunk. */
    assert(osvc_session_file_data(&s_session, drop_id, id, data, MAX - 1U));
    expect_chunk(id, 0U, data, MAX - 1U);
    expect_no_message();
    assert(osvc_session_file_data(&s_session, drop_id, id, data + MAX - 1U,
                                  MAX));
    expect_chunk(id, MAX - 1U, data + MAX - 1U, MAX);
    expect_no_message();
    assert(osvc_session_file_data(&s_session, drop_id, id, data + 2U * MAX - 1U,
                                  MAX + 1U));
    expect_chunk(id, 2U * MAX - 1U, data + 2U * MAX - 1U, MAX);
    expect_chunk(id, 3U * MAX - 1U, data + 3U * MAX - 1U, 1U);
    expect_no_message();
    /* The largest chunk is a payload of exactly 64 KiB. */
    assert(OSVC_CHUNK_HEADER_LEN + MAX == OSVC_MAX_CONTROL_PAYLOAD);
    sender_done(id, OSVC_FILE_DONE_OK, "/h/cuts.bin");
    expect_done_event(1U, drop_id, id, OSVC_FILE_DONE_OK, "/h/cuts.bin", true);
}

static void test_file_credit_boundary_and_overrun(void)
{
    open_services(false, true);
    static uint8_t data[4096];
    memset(data, 'd', sizeof(data));
    const osvc_file_entry_t files[3] = { file_entry("a", 10000U),
                                         file_entry("b", 10U),
                                         file_entry("c", 10U) };
    uint32_t ids[3];
    const uint32_t drop_id = browser_drops(files, 3U, ids);
    assert(osvc_session_file_attach(&s_session, drop_id));
    /* No credit before file.ready: a single byte is beyond it. */
    assert(!osvc_session_file_data(&s_session, drop_id, ids[0], data, 1U));
    for (size_t i = 0U; i < 3U; ++i) {
        expect_cancel_sent(ids[i]);
        expect_done_event(i, drop_id, ids[i], OSVC_FILE_DONE_FAILED, "",
                          i == 2U);
    }
    expect_no_message();

    open_services(false, true);
    const uint32_t second = browser_drops(files, 3U, ids);
    assert(osvc_session_file_attach(&s_session, second));
    sender_credit(OSVC_FILE_READY, ids[0], 4096U);
    /* Exactly the credit is fine; one byte more is not. */
    assert(osvc_session_file_data(&s_session, second, ids[0], data, 4000U));
    assert(osvc_session_file_data(&s_session, second, ids[0], data, 96U));
    expect_chunk(ids[0], 0U, data, 4000U);
    expect_chunk(ids[0], 4000U, data, 96U);
    sender_credit(OSVC_FILE_CREDIT, ids[0], 100U);
    assert(!osvc_session_file_data(&s_session, second, ids[0], data, 101U));
    expect_cancel_sent(ids[0]);
    expect_cancel_sent(ids[1]);
    expect_cancel_sent(ids[2]);
    expect_no_message();
    assert(s_fake.file_count == 5U);
    expect_done_event(2U, second, ids[0], OSVC_FILE_DONE_FAILED, "", false);
    expect_done_event(4U, second, ids[2], OSVC_FILE_DONE_FAILED, "", true);
}

static void test_file_zero_byte_files_and_held_outcomes(void)
{
    open_services(false, true);
    const osvc_file_entry_t files[3] = { file_entry("empty1", 0U),
                                         file_entry("empty2", 0U),
                                         file_entry("gone", 7U) };
    uint32_t ids[3];
    const uint32_t drop_id = browser_drops(files, 3U, ids);
    /* An empty file is complete as created: file.ready, then file.done. */
    sender_credit(OSVC_FILE_READY, ids[0], 524288U);
    sender_done(ids[0], OSVC_FILE_DONE_OK, "/home/u/empty1");
    sender_credit(OSVC_FILE_READY, ids[1], 524288U);
    sender_done(ids[1], OSVC_FILE_DONE_OK, "/home/u/empty2");
    /* The sender could not create the third. */
    sender_done(ids[2], OSVC_FILE_DONE_FAILED, NULL);
    assert(s_fake.file_count == 0U);
    expect_no_message();
    /* The drop is over; a new one may start while these outcomes wait. */
    assert(osvc_session_file_attach(&s_session, drop_id));
    assert(s_fake.file_count == 3U);
    expect_done_event(0U, drop_id, ids[0], OSVC_FILE_DONE_OK, "", false);
    expect_done_event(1U, drop_id, ids[1], OSVC_FILE_DONE_OK, "", false);
    expect_done_event(2U, drop_id, ids[2], OSVC_FILE_DONE_FAILED, "", true);
    assert(!osvc_session_file_attach(&s_session, drop_id));

    /* Attached first: each empty file's outcome comes as it happens, with
     * its path. */
    uint32_t one[1];
    const uint32_t live = browser_drops(files, 1U, one);
    assert(osvc_session_file_attach(&s_session, live));
    sender_credit(OSVC_FILE_READY, one[0], 524288U);
    expect_credit_event(3U, live, one[0], 524288U);
    sender_done(one[0], OSVC_FILE_DONE_OK, "/home/u/empty1 (2)");
    expect_done_event(4U, live, one[0], OSVC_FILE_DONE_OK,
                      "/home/u/empty1 (2)", true);
    expect_no_message();

    /* A finished drop whose channel never opens: a new drop replaces it,
     * and after 30 seconds it is forgotten anyway. */
    const uint32_t lonely = browser_drops(files, 1U, one);
    sender_done(one[0], OSVC_FILE_DONE_FAILED, NULL);
    uint32_t again[1];
    const uint32_t replacing = browser_drops(files, 1U, again);
    assert(!osvc_session_file_attach(&s_session, lonely));
    sender_done(again[0], OSVC_FILE_DONE_FAILED, NULL);
    s_fake.now += OSVC_FILE_IDLE_TIMEOUT_MS;
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_PING, NULL, 0U);
    osvc_session_tick(&s_session);
    assert(!osvc_session_file_attach(&s_session, replacing));
    assert(s_fake.file_count == 5U);
    expect_no_message();
}

static void test_file_cancels(void)
{
    open_services(false, true);
    static uint8_t data[64];
    const osvc_file_entry_t files[3] = { file_entry("a", 64U),
                                         file_entry("b", 64U),
                                         file_entry("c", 64U) };
    uint32_t ids[3];
    const uint32_t drop_id = browser_drops(files, 3U, ids);
    assert(osvc_session_file_attach(&s_session, drop_id));
    sender_credit(OSVC_FILE_READY, ids[0], 524288U);
    assert(osvc_session_file_data(&s_session, drop_id, ids[0], data, 10U));
    expect_chunk(ids[0], 0U, data, 10U);
    /* The browser stops the active file and a queued one. */
    assert(osvc_session_file_cancel(&s_session, drop_id, ids[0]));
    expect_cancel_sent(ids[0]);
    assert(osvc_session_file_cancel(&s_session, drop_id, ids[2]));
    expect_cancel_sent(ids[2]);
    assert(!osvc_session_file_cancel(&s_session, drop_id, ids[2]));
    assert(!osvc_session_file_cancel(&s_session, drop_id + 7U, ids[1]));
    assert(!osvc_session_file_data(&s_session, drop_id, ids[0], data, 1U));
    expect_no_message();
    expect_credit_event(0U, drop_id, ids[0], 524288U);
    expect_done_event(1U, drop_id, ids[0], OSVC_FILE_DONE_CANCELLED, "", false);
    expect_done_event(2U, drop_id, ids[2], OSVC_FILE_DONE_CANCELLED, "", false);
    /* The sender's own answers to those cancels are not reported again. */
    sender_done(ids[0], OSVC_FILE_DONE_CANCELLED, NULL);
    sender_done(ids[2], OSVC_FILE_DONE_CANCELLED, NULL);
    assert(s_fake.file_count == 3U);
    /* The sender cancels the last one itself (reception turned off there). */
    sender_credit(OSVC_FILE_READY, ids[1], 524288U);
    sender_done(ids[1], OSVC_FILE_DONE_CANCELLED, NULL);
    expect_credit_event(3U, drop_id, ids[1], 524288U);
    expect_done_event(4U, drop_id, ids[1], OSVC_FILE_DONE_CANCELLED, "", true);
    expect_no_message();

    /* The channel closes mid-drop: everything open is cancelled on the
     * sender, and nobody is told. */
    const uint32_t closing = browser_drops(files, 3U, ids);
    assert(osvc_session_file_attach(&s_session, closing));
    sender_credit(OSVC_FILE_READY, ids[0], 1024U);
    sender_done(ids[0], OSVC_FILE_DONE_FAILED, NULL);
    const size_t before = s_fake.file_count;
    osvc_session_file_channel_closed(&s_session, closing);
    expect_cancel_sent(ids[1]);
    expect_cancel_sent(ids[2]);
    expect_no_message();
    assert(s_fake.file_count == before);
    assert(!osvc_session_file_attach(&s_session, closing));
    osvc_session_file_channel_closed(&s_session, closing);
    expect_no_message();
}

static void test_file_idle_timeout(void)
{
    open_services(false, true);
    static uint8_t data[16];
    const osvc_file_entry_t files[2] = { file_entry("a", 16U),
                                         file_entry("b", 16U) };
    uint32_t ids[2];
    const uint32_t drop_id = browser_drops(files, 2U, ids);
    assert(osvc_session_file_attach(&s_session, drop_id));
    /* Progress on either side restarts the 30 seconds. */
    s_fake.now += 20000;
    sender_credit(OSVC_FILE_READY, ids[0], 16U);
    s_fake.now += 20000;
    assert(osvc_session_file_data(&s_session, drop_id, ids[0], data, 8U));
    expect_chunk(ids[0], 0U, data, 8U);
    s_fake.now += OSVC_FILE_IDLE_TIMEOUT_MS - 1;
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_PING, NULL, 0U);
    osvc_session_tick(&s_session);
    expect_no_message();
    assert(s_fake.file_count == 1U);
    s_fake.now += 1;
    osvc_session_tick(&s_session);
    expect_cancel_sent(ids[0]);
    expect_cancel_sent(ids[1]);
    expect_no_message();
    expect_done_event(1U, drop_id, ids[0], OSVC_FILE_DONE_FAILED, "", false);
    expect_done_event(2U, drop_id, ids[1], OSVC_FILE_DONE_FAILED, "", true);
    osvc_session_tick(&s_session);
    assert(s_fake.file_count == 3U);

    /* A drop whose channel never opens fails sooner, even while the
     * sender makes progress, and no longer refuses the next drop; its
     * outcomes then wait 30 seconds for the channel. */
    const uint32_t unattached = browser_drops(files, 2U, ids);
    s_fake.now += OSVC_FILE_ATTACH_TIMEOUT_MS - 1;
    sender_credit(OSVC_FILE_READY, ids[0], 16U);
    osvc_session_tick(&s_session);
    expect_no_message();
    uint32_t refused[2];
    assert(osvc_session_file_drop(&s_session, files, 2U, &(uint32_t){ 0U },
                                  refused) != NULL);
    s_fake.now += 1;
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_PING, NULL, 0U);
    osvc_session_tick(&s_session);
    expect_cancel_sent(ids[0]);
    expect_cancel_sent(ids[1]);
    expect_no_message();
    assert(s_fake.file_count == 3U);
    s_fake.now += OSVC_FILE_IDLE_TIMEOUT_MS - 1;
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_PING, NULL, 0U);
    osvc_session_tick(&s_session);
    assert(osvc_session_file_attach(&s_session, unattached));
    expect_done_event(3U, unattached, ids[0], OSVC_FILE_DONE_FAILED, "", false);
    expect_done_event(4U, unattached, ids[1], OSVC_FILE_DONE_FAILED, "", true);
    expect_no_message();
}

/* A browser that leaves between the drop and its channel: the next drop is
 * accepted once the attach timeout has failed the first, and an attached
 * drop never hits that timeout. */
static void test_file_attach_timeout_frees_next_drop(void)
{
    open_services(false, true);
    const osvc_file_entry_t files[1] = { file_entry("a", 16U) };
    uint32_t ids[1];
    (void)browser_drops(files, 1U, ids);
    s_fake.now += OSVC_FILE_ATTACH_TIMEOUT_MS;
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_PING, NULL, 0U);
    osvc_session_tick(&s_session);
    expect_cancel_sent(ids[0]);
    uint32_t next[1];
    const uint32_t next_id = browser_drops(files, 1U, next);
    assert(osvc_session_file_attach(&s_session, next_id));
    s_fake.now += OSVC_FILE_ATTACH_TIMEOUT_MS;
    osvc_session_tick(&s_session);
    expect_no_message();
}

/* Each invalid sender message faults the file service alone: the drop
 * fails on both sides, the session and the terminal stay, and the
 * service's messages count only after a new registration. */
static void test_file_invalid_messages_fault_the_service(void)
{
    struct {
        uint16_t kind;
        uint8_t payload[12];
        size_t length;
        bool after_ready;
    } cases[] = {
        { OSVC_FILE_READY, { 0 }, 7U, false },
        { OSVC_FILE_READY, { 0 }, 9U, false },
        { OSVC_FILE_CREDIT, { 0 }, 4U, true },
        { OSVC_FILE_DONE, { 0 }, 7U, false },
        { OSVC_FILE_DONE, { 0, 0, 0, 0, 3 }, 8U, false },
        { OSVC_FILE_DONE, { 0, 0, 0, 0, 1, 0, 1 }, 8U, false },
        { OSVC_FILE_DONE, { 0, 0, 0, 0, 0, 0, 0, 0, 0xff }, 9U, false },
        { OSVC_FILE_DONE, { 0, 0, 0, 0, 1, 0, 0, 0, '/', 'x' }, 10U, false },
        { OSVC_FILE_DONE, { 0, 0, 0, 0, 2, 0, 0, 0, '/', 'x' }, 10U, false },
        /* OK while the KVM has not sent the bytes. */
        { OSVC_FILE_DONE, { 0, 0, 0, 0, 0, 0, 0, 0, '/', 'x' }, 10U, true },
        /* Credit before file.ready, and file.ready twice. */
        { OSVC_FILE_CREDIT, { 0, 0, 0, 0, 1 }, 8U, false },
        { OSVC_FILE_READY, { 0, 0, 0, 0, 1 }, 8U, true },
        { OSVC_FILE_CAPABILITIES, { 3 }, 4U, false },
        { OSVC_FILE_CAPABILITIES, { 1 }, 3U, false },
        { OSVC_FILE_DROP, { 1, 0, 0, 0 }, 4U, false },
        { OSVC_FILE_CHUNK, { 0 }, 12U, false },
        { OSVC_FILE_CANCEL, { 0 }, 4U, false },
    };
    for (size_t c = 0U; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        open_services(true, true);
        const uint32_t terminal_id = open_running_terminal();
        const osvc_file_entry_t files[2] = { file_entry("a", 5U),
                                             file_entry("b", 5U) };
        uint32_t ids[2];
        const uint32_t drop_id = browser_drops(files, 2U, ids);
        assert(osvc_session_file_attach(&s_session, drop_id));
        if (cases[c].after_ready) {
            sender_credit(OSVC_FILE_READY, ids[0], 1U);
            assert(s_fake.file_count == 1U);
        }
        const size_t before = s_fake.file_count;
        if (cases[c].kind != OSVC_FILE_CAPABILITIES &&
            cases[c].kind != OSVC_FILE_DROP && cases[c].length >= 4U) {
            osvc_put_u32(cases[c].payload, ids[0]);
        }
        const unsigned changes = s_fake.state_changes;
        send_message(OSVC_SERVICE_FILE, cases[c].kind, cases[c].payload,
                     cases[c].length);
        assert(osvc_session_open(&s_session));
        assert(s_session.counters.service_faults == 1U);
        assert(!osvc_session_file_available(&s_session));
        assert(s_fake.state_changes > changes);
        assert(osvc_session_terminal_available(&s_session));
        expect_cancel_sent(ids[0]);
        expect_cancel_sent(ids[1]);
        expect_no_message();
        assert(s_fake.file_count == before + 2U);
        expect_done_event(before, drop_id, ids[0], OSVC_FILE_DONE_FAILED, "",
                          false);
        expect_done_event(before + 1U, drop_id, ids[1], OSVC_FILE_DONE_FAILED,
                          "", true);
        /* Ignored until it registers again. */
        sender_credit(OSVC_FILE_READY, ids[0], 1U);
        register_service(OSVC_SERVICE_FILE, OSVC_FILE_FLAG_RECEIVE);
        assert(osvc_session_file_available(&s_session));
        assert(s_session.counters.service_faults == 1U);
        sender_output(terminal_id, "still here", 10U);
        assert(s_fake.terminal_output_length == 10U);
        uint32_t next[1];
        (void)browser_drops(files, 1U, next);
    }
}

static void test_file_ends_with_settings_withdrawal_and_session(void)
{
    const osvc_file_entry_t files[2] = { file_entry("a", 5U),
                                         file_entry("b", 5U) };
    uint32_t ids[2];
    static const uint8_t data[5] = { 1, 2, 3, 4, 5 };

    /* The setting goes off: transfers fail and are cancelled, then the
     * sender hears the policy. */
    open_services(false, true);
    uint32_t drop_id = browser_drops(files, 2U, ids);
    assert(osvc_session_file_attach(&s_session, drop_id));
    sender_credit(OSVC_FILE_READY, ids[0], 5U);
    assert(osvc_session_file_data(&s_session, drop_id, ids[0], data, 5U));
    expect_chunk(ids[0], 0U, data, 5U);
    sender_done(ids[0], OSVC_FILE_DONE_OK, "/h/a");
    expect_done_event(1U, drop_id, ids[0], OSVC_FILE_DONE_OK, "/h/a", false);
    osvc_settings_t settings = default_settings();
    settings.files_enabled = false;
    osvc_session_apply_settings(&s_session, &settings);
    expect_cancel_sent(ids[1]);
    fake_message_t config = expect_message(OSVC_SERVICE_SESSION,
                                           OSVC_SESSION_CONFIG);
    check_config(&config, true, false, "");
    expect_no_message();
    assert(s_fake.file_count == 3U);
    expect_done_event(2U, drop_id, ids[1], OSVC_FILE_DONE_FAILED, "", true);

    /* The sender withdraws mid-transfer. */
    open_services(false, true);
    drop_id = browser_drops(files, 2U, ids);
    assert(osvc_session_file_attach(&s_session, drop_id));
    sender_credit(OSVC_FILE_READY, ids[0], 5U);
    assert(osvc_session_file_data(&s_session, drop_id, ids[0], data, 2U));
    expect_chunk(ids[0], 0U, data, 2U);
    register_service(OSVC_SERVICE_FILE, 0U);
    expect_cancel_sent(ids[0]);
    expect_cancel_sent(ids[1]);
    expect_no_message();
    expect_done_event(1U, drop_id, ids[0], OSVC_FILE_DONE_FAILED, "", false);
    expect_done_event(2U, drop_id, ids[1], OSVC_FILE_DONE_FAILED, "", true);
    assert(s_session.counters.service_faults == 0U);

    /* The session ends mid-transfer: every open transfer fails exactly
     * once, the finished one is not reported again, nothing is written. */
    open_services(false, true);
    const osvc_file_entry_t three[3] = { file_entry("a", 0U),
                                         file_entry("b", 5U),
                                         file_entry("c", 5U) };
    uint32_t three_ids[3];
    drop_id = browser_drops(three, 3U, three_ids);
    assert(osvc_session_file_attach(&s_session, drop_id));
    sender_credit(OSVC_FILE_READY, three_ids[0], 524288U);
    sender_done(three_ids[0], OSVC_FILE_DONE_OK, "/h/a");
    sender_credit(OSVC_FILE_READY, three_ids[1], 524288U);
    assert(osvc_session_file_data(&s_session, drop_id, three_ids[1], data, 3U));
    expect_chunk(three_ids[1], 0U, data, 3U);
    const size_t before = s_fake.file_count;
    osvc_session_end(&s_session, "the IN endpoint did not drain");
    expect_no_message();
    assert(s_fake.file_count == before + 2U);
    expect_done_event(before, drop_id, three_ids[1], OSVC_FILE_DONE_FAILED, "",
                      false);
    expect_done_event(before + 1U, drop_id, three_ids[2], OSVC_FILE_DONE_FAILED,
                      "", true);
    osvc_session_end(&s_session, "again");
    osvc_session_file_channel_closed(&s_session, drop_id);
    assert(!osvc_session_file_data(&s_session, drop_id, three_ids[1], data, 1U));
    assert(s_fake.file_count == before + 2U);

    /* Ended before its channel opened: the failures wait for it. */
    open_services(false, true);
    drop_id = browser_drops(files, 2U, ids);
    uint8_t usb_status[OSVC_USB_STATUS_LEN];
    osvc_session_usb_reset(&s_session, usb_status);
    assert(s_fake.file_count == 0U);
    assert(osvc_session_file_attach(&s_session, drop_id));
    assert(s_fake.file_count == 2U);
    expect_done_event(0U, drop_id, ids[0], OSVC_FILE_DONE_FAILED, "", false);
    expect_done_event(1U, drop_id, ids[1], OSVC_FILE_DONE_FAILED, "", true);

    /* A chunk that cannot be queued ends the session. */
    open_services(false, true);
    drop_id = browser_drops(files, 2U, ids);
    assert(osvc_session_file_attach(&s_session, drop_id));
    sender_credit(OSVC_FILE_READY, ids[0], 5U);
    s_fake.fail_writes = true;
    assert(!osvc_session_file_data(&s_session, drop_id, ids[0], data, 5U));
    assert(!osvc_session_open(&s_session));
    expect_done_event(1U, drop_id, ids[0], OSVC_FILE_DONE_FAILED, "", false);
    expect_done_event(2U, drop_id, ids[1], OSVC_FILE_DONE_FAILED, "", true);
    assert(s_fake.file_count == 3U);
}

/*
 * A 10 MiB file under the sender's credit window (File service: 512 KiB
 * offered, returned as it writes, here in 128 KiB steps like the sender),
 * with the browser sending 32 KiB messages. Each simulated round, the
 * browser sends what its credit allows and the sender takes every chunk.
 * The relay must not add messages, bytes or round trips: one chunk per
 * browser message, 20 bytes of framing per chunk, a full window per round,
 * and never a byte beyond the credit.
 */
static void test_file_throughput_under_the_credit_window(void)
{
    enum { WINDOW = 512 * 1024, STEP = 128 * 1024, FRAME = 32 * 1024 };
    const uint64_t size = 10U * 1024U * 1024U;
    static uint8_t frame[FRAME];
    open_services(false, true);
    const osvc_file_entry_t file = file_entry("large.iso", size);
    uint32_t id;
    const uint32_t drop_id = browser_drops(&file, 1U, &id);
    assert(osvc_session_file_attach(&s_session, drop_id));
    const uint64_t bytes_out_before = s_session.counters.bytes_out;

    /* Sender state. */
    uint64_t granted = WINDOW;
    uint64_t received = 0U;
    uint64_t unreturned = 0U;
    unsigned chunks = 0U;
    unsigned credits_sent = 1U;
    sender_credit(OSVC_FILE_READY, id, WINDOW);
    /* Browser state. */
    uint64_t browser_credit = 0U;
    uint64_t browser_sent = 0U;
    size_t events_read = 0U;
    unsigned rounds = 0U;
    bool done = false;
    while (!done) {
        assert(++rounds <= 64U);
        for (; events_read < s_fake.file_count; ++events_read) {
            const fake_file_event_t *event = &s_fake.file[events_read];
            if (event->done) {
                assert(event->status == OSVC_FILE_DONE_OK && event->last);
                done = true;
            } else {
                browser_credit += event->bytes;
            }
        }
        if (done) {
            break;
        }
        while (browser_credit > 0U && browser_sent < size) {
            uint64_t n = browser_credit < FRAME ? browser_credit : FRAME;
            n = n < size - browser_sent ? n : size - browser_sent;
            memset(frame, (int)(browser_sent / FRAME), (size_t)n);
            assert(osvc_session_file_data(&s_session, drop_id, id, frame,
                                          (size_t)n));
            browser_credit -= n;
            browser_sent += n;
        }
        fake_message_t message;
        while (fake_next_message(&s_fake, &message)) {
            assert(message.service == OSVC_SERVICE_FILE &&
                   message.kind == OSVC_FILE_CHUNK);
            const uint64_t offset = osvc_get_u64(message.payload + 4);
            const uint64_t length = message.length - 12U;
            assert(osvc_get_u32(message.payload) == id && offset == received);
            assert(offset + length <= granted && offset + length <= size);
            assert(message.payload[12] == (uint8_t)(offset / FRAME));
            received += length;
            unreturned += length;
            chunks++;
            if (received == size) {
                sender_done(id, OSVC_FILE_DONE_OK, "/h/large.iso");
            } else if (unreturned >= STEP) {
                sender_credit(OSVC_FILE_CREDIT, id, (uint32_t)unreturned);
                granted += unreturned;
                unreturned = 0U;
                credits_sent++;
            }
        }
        fake_clear_output(&s_fake);
    }
    assert(received == size && browser_sent == size);
    /* One chunk per browser message, and no other message. */
    assert(chunks == size / FRAME);
    const uint64_t wire = s_session.counters.bytes_out - bytes_out_before;
    assert(wire == size + (uint64_t)chunks * (OSVC_ENVELOPE_LEN + 12U));
    assert(wire * 1000U < size * 1001U);
    /* Every credit reached the browser, one event per sender message. */
    assert(s_fake.file_count == credits_sent + 1U);
    /* A full window moves per round: 20 rounds of 512 KiB, then the
     * outcome. */
    assert(rounds == size / WINDOW + 1U);
    printf("file relay: %u chunks of %u bytes, %.4f%% framing, %u rounds\n",
           chunks, (unsigned)FRAME,
           100.0 * (double)(wire - size) / (double)size, rounds);
}

/* The new hooks are optional: a platform written before them keeps its
 * session, and one that takes output but not closings still ends cleanly. */
static void test_platform_without_new_hooks(void)
{
    open_services(true, true);
    s_platform.terminal_output = NULL;
    s_platform.terminal_closed = NULL;
    s_platform.file_credit = NULL;
    s_platform.file_done = NULL;
    osvc_settings_t settings = default_settings();
    settings.files_enabled = true;
    osvc_session_init(&s_session, &s_platform, &settings, s_payload_buffer,
                      sizeof(s_payload_buffer));
    uint8_t token[OSVC_TOKEN_LEN];
    reset_and_admit(token);
    uint8_t hello[OSVC_HELLO_LEN];
    make_hello(hello, 5U, 0U, token, 9U);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_HELLO, hello, sizeof(hello));
    assert(osvc_session_open(&s_session));
    fake_clear_output(&s_fake);
    register_service(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_FLAG_SHELL);
    register_service(OSVC_SERVICE_FILE, OSVC_FILE_FLAG_RECEIVE);
    assert(!osvc_session_terminal_available(&s_session));
    assert(!osvc_session_file_available(&s_session));
    /* Stray messages for IDs never handed out are ignored. */
    sender_output(1U, "x", 1U);
    sender_terminal_status(OSVC_TERMINAL_EXITED, 1U, OSVC_TERMINAL_EXIT_EXITED);
    sender_credit(OSVC_FILE_READY, 2U, 1U);
    sender_done(2U, OSVC_FILE_DONE_FAILED, NULL);
    assert(osvc_session_open(&s_session));
    assert(s_session.counters.service_faults == 0U);
    expect_no_message();

    /* Output but no closing hook. */
    fake_platform_init(&s_fake, &s_platform);
    s_platform.terminal_closed = NULL;
    osvc_session_init(&s_session, &s_platform, &settings, s_payload_buffer,
                      sizeof(s_payload_buffer));
    reset_and_admit(token);
    make_hello(hello, 5U, 0U, token, 11U);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_HELLO, hello, sizeof(hello));
    assert(osvc_session_open(&s_session));
    fake_clear_output(&s_fake);
    register_service(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_FLAG_SHELL);
    const uint32_t exited = open_running_terminal();
    sender_terminal_status(OSVC_TERMINAL_EXITED, exited,
                           OSVC_TERMINAL_EXIT_EXITED);
    (void)open_running_terminal();
    const uint32_t unanswered = browser_opens_terminal(24U, 80U);
    s_fake.now += OSVC_TERMINAL_OPEN_TIMEOUT_MS;
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_PING, NULL, 0U);
    osvc_session_tick(&s_session);
    expect_terminal_close_sent(unanswered);
    expect_no_message();
    osvc_session_end(&s_session, "done");
    assert(s_fake.terminal_closed_count == 0U);
    assert(!osvc_session_open(&s_session));
}

static void test_wire_helpers(void)
{
    assert(osvc_utf8_valid((const uint8_t *)"", 0U));
    assert(osvc_utf8_valid((const uint8_t *)"\xf0\x9f\x98\x80", 4U));
    assert(!osvc_utf8_valid((const uint8_t *)"\xed\xa0\x80", 3U));
    assert(!osvc_utf8_valid((const uint8_t *)"\xe0\x80\xaf", 3U));
    assert(!osvc_utf8_valid((const uint8_t *)"\xf4\x90\x80\x80", 4U));
    assert(!osvc_utf8_valid((const uint8_t *)"\xc3", 1U));
    uint8_t digest[SHA256_DIGEST_LENGTH];
    const uint8_t key[32] = { 1, 2, 3 };
    SHA256(key, sizeof(key), digest);
    char text[OSVC_FINGERPRINT_TEXT_CAPACITY];
    osvc_format_fingerprint(digest, text);
    char expected[OSVC_FINGERPRINT_TEXT_CAPACITY];
    snprintf(expected, sizeof(expected),
             "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X", digest[0], digest[1],
             digest[2], digest[3], digest[4], digest[5], digest[6], digest[7]);
    assert(strcmp(text, expected) == 0);

    /* H.264: SPS, PPS, IDR with 3- and 4-byte start codes. */
    const uint8_t idr[] = { 0, 0, 0, 1, 0x67, 1, 2, 0, 0, 1, 0x68, 3,
                            0, 0, 1, 0x65, 0x88, 0x84 };
    assert(osvc_annexb_is_keyframe(OSVC_CODEC_H264, idr, sizeof(idr)));
    assert(osvc_annexb_has_parameter_sets(OSVC_CODEC_H264, idr, sizeof(idr)));
    const uint8_t delta[] = { 0, 0, 0, 1, 0x09, 0xf0, 0, 0, 1, 0x41, 0x9a };
    assert(!osvc_annexb_is_keyframe(OSVC_CODEC_H264, delta, sizeof(delta)));
    assert(!osvc_annexb_has_parameter_sets(OSVC_CODEC_H264, delta,
                                           sizeof(delta)));
    /* H.265: VPS 32, SPS 33, PPS 34, IDR_W_RADL 19. */
    const uint8_t hevc[] = { 0, 0, 0, 1, 0x40, 1, 0, 0, 1, 0x42, 1,
                             0, 0, 1, 0x44, 1, 0, 0, 1, 0x26, 1, 0xaf };
    assert(osvc_annexb_is_keyframe(OSVC_CODEC_H265, hevc, sizeof(hevc)));
    assert(osvc_annexb_has_parameter_sets(OSVC_CODEC_H265, hevc, sizeof(hevc)));
    const uint8_t hevc_trail[] = { 0, 0, 1, 0x02, 1, 0xd0 };
    assert(!osvc_annexb_is_keyframe(OSVC_CODEC_H265, hevc_trail,
                                    sizeof(hevc_trail)));
}


/*
 * Update service. The sender's side is written here from the protocol; the
 * relay's side is osvc_relay_package() over a package GnuPG signed
 * (fixtures/), with OpenSSL's hashes.
 */

#define UPDATE_PACKAGE "linux-x86_64-appimage"

static void register_update(void)
{
    uint8_t caps[4 + sizeof(UPDATE_PACKAGE) - 1U];
    osvc_put_u32(caps, OSVC_UPDATE_FLAG_INSTALL);
    memcpy(caps + 4, UPDATE_PACKAGE, sizeof(caps) - 4U);
    send_message(OSVC_SERVICE_UPDATE, OSVC_UPDATE_CAPABILITIES, caps,
                 sizeof(caps));
}

static void sender_update_credit(uint16_t kind, uint32_t update_id,
                                 uint32_t bytes)
{
    uint8_t payload[8];
    osvc_put_u32(payload, update_id);
    osvc_put_u32(payload + 4, bytes);
    send_message(OSVC_SERVICE_UPDATE, kind, payload, sizeof(payload));
}

static void sender_update_status(uint32_t update_id, uint8_t status,
                                 const char *message)
{
    uint8_t payload[8 + 256] = {0};
    osvc_put_u32(payload, update_id);
    payload[4] = status;
    memcpy(payload + 8, message, strlen(message));
    send_message(OSVC_SERVICE_UPDATE, OSVC_UPDATE_STATUS, payload,
                 8U + strlen(message));
}

/* An open session whose sender installs packages, with update.begin sent
 * for a package of size bytes. */
static uint32_t begin_update(uint64_t size)
{
    open_services(false, false);
    assert(!osvc_session_update_available(&s_session));
    register_update();
    assert(osvc_session_update_available(&s_session));
    static const uint8_t sha[32] = {1, 2, 3};
    uint32_t id = 0U;
    assert(osvc_session_update_begin(&s_session, size, sha, "0.4.3", &id) ==
           NULL);
    const fake_message_t begin = expect_message(OSVC_SERVICE_UPDATE,
                                                OSVC_UPDATE_BEGIN);
    assert(begin.length == OSVC_UPDATE_BEGIN_FIXED_LEN + 5U && id != 0U);
    assert(osvc_get_u32(begin.payload) == id);
    assert(osvc_get_u64(begin.payload + 4) == size);
    assert(memcmp(begin.payload + 12, sha, 32U) == 0);
    assert(memcmp(begin.payload + 44, "0.4.3", 5U) == 0);
    return id;
}

/* Reads the update.chunk messages the KVM wrote; returns the bytes. */
static uint64_t read_update_chunks(uint32_t update_id, uint64_t offset,
                                   uint8_t *into)
{
    fake_message_t message;
    uint64_t total = 0U;
    while (fake_next_message(&s_fake, &message)) {
        assert(message.service == OSVC_SERVICE_UPDATE &&
               message.kind == OSVC_UPDATE_CHUNK);
        assert(message.length > 12U &&
               message.length - 12U <= OSVC_CHUNK_MAX_DATA);
        assert(osvc_get_u32(message.payload) == update_id);
        assert(osvc_get_u64(message.payload + 4) == offset + total);
        if (into != NULL) {
            memcpy(into + offset + total, message.payload + 12,
                   message.length - 12U);
        }
        total += message.length - 12U;
    }
    return total;
}

static void test_update_credit_and_installing(void)
{
    static uint8_t data[150000];
    for (size_t i = 0U; i < sizeof(data); ++i) {
        data[i] = (uint8_t)(i * 7U);
    }
    const uint32_t id = begin_update(sizeof(data));
    uint32_t again = 0U;
    static const uint8_t sha[32];
    assert(strcmp(osvc_session_update_begin(&s_session, 1U, sha, "1", &again),
                  "an OS Services update is already in progress") == 0);
    /* Nothing goes before update.ready, then only within the credit. */
    assert(osvc_session_update_send(&s_session, id, data, sizeof(data)) == 0U);
    sender_update_credit(OSVC_UPDATE_READY, id, 100000U);
    assert(osvc_session_update_send(&s_session, id, data, sizeof(data)) ==
           100000U);
    static uint8_t received[sizeof(data)];
    assert(read_update_chunks(id, 0U, received) == 100000U);
    /* A stall: no credit, nothing sent, until the sender returns some. */
    assert(osvc_session_update_send(&s_session, id, data + 100000U, 50000U) == 0U);
    expect_no_message();
    sender_update_credit(OSVC_UPDATE_CREDIT, id, 1U << 20);
    /* Credit beyond the package does not allow bytes beyond it. */
    assert(osvc_session_update_send(&s_session, id, data + 100000U, 60000U) ==
           50000U);
    assert(read_update_chunks(id, 100000U, received) == 50000U);
    assert(memcmp(received, data, sizeof(data)) == 0);
    /* Messages for another update are in transit, and ignored. */
    sender_update_status(id + 1U, OSVC_UPDATE_STATUS_FAILED, "old");
    sender_update_credit(OSVC_UPDATE_CREDIT, id + 1U, 5U);
    assert(!s_session.update.ended);
    sender_update_status(id, OSVC_UPDATE_STATUS_INSTALLING, "");
    assert(s_session.update.ended &&
           s_session.update.status == OSVC_UPDATE_STATUS_INSTALLING);
    expect_no_message();
    assert(osvc_session_update_send(&s_session, id, data, 1U) == 0U);

    /* The next update gets a new ID. */
    uint32_t next = 0U;
    assert(osvc_session_update_begin(&s_session, 1U, sha, "", &next) == NULL);
    assert(next != id);
}

static void test_update_ends(void)
{
    static const uint8_t data[64];
    /* The browser's cancel. */
    uint32_t id = begin_update(sizeof(data));
    osvc_session_update_cancel(&s_session, id, "stopped");
    fake_message_t cancel = expect_message(OSVC_SERVICE_UPDATE, OSVC_UPDATE_CANCEL);
    assert(cancel.length == 4U && osvc_get_u32(cancel.payload) == id);
    assert(s_session.update.status == OSVC_UPDATE_STATUS_CANCELLED &&
           strcmp(s_session.update.message, "stopped") == 0);
    sender_update_status(id, OSVC_UPDATE_STATUS_CANCELLED, "");
    expect_no_message();

    /* 30 seconds without progress on either side. */
    id = begin_update(sizeof(data));
    s_fake.now += 20000;
    sender_update_credit(OSVC_UPDATE_READY, id, 32U);
    s_fake.now += 20000;
    assert(osvc_session_update_send(&s_session, id, data, sizeof(data)) == 32U);
    assert(read_update_chunks(id, 0U, NULL) == 32U);
    s_fake.now += OSVC_UPDATE_IDLE_TIMEOUT_MS - 1;
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_PING, NULL, 0U);
    osvc_session_tick(&s_session);
    assert(!s_session.update.ended);
    s_fake.now += 1;
    osvc_session_tick(&s_session);
    expect_message(OSVC_SERVICE_UPDATE, OSVC_UPDATE_CANCEL);
    assert(s_session.update.ended &&
           s_session.update.status == OSVC_UPDATE_STATUS_FAILED);

    /* The sender's failure, cut to 64 bytes on a character boundary. */
    id = begin_update(sizeof(data));
    char message[80];
    memset(message, 'x', 63U);
    strcpy(message + 63, "\xc3\xa9 and more");
    sender_update_status(id, OSVC_UPDATE_STATUS_FAILED, message);
    assert(strlen(s_session.update.message) == 63U);
    expect_no_message();
    /* What JSON would escape becomes '?', so the browser's event shows the
     * message at its own size. */
    id = begin_update(sizeof(data));
    sender_update_status(id, OSVC_UPDATE_STATUS_FAILED, "a\x01\"b\\c\n\xc3\xa9");
    assert(strcmp(s_session.update.message, "a??b?c?\xc3\xa9") == 0);
    expect_no_message();

    /* A withdrawal, and the session's end. */
    id = begin_update(sizeof(data));
    register_service(OSVC_SERVICE_UPDATE, 0U);
    expect_message(OSVC_SERVICE_UPDATE, OSVC_UPDATE_CANCEL);
    assert(!osvc_session_update_available(&s_session));
    assert(strcmp(s_session.update.message,
                  "the sender withdrew the update service") == 0);
    id = begin_update(sizeof(data));
    osvc_session_end(&s_session, "test");
    assert(s_session.update.ended &&
           strcmp(s_session.update.message, "the OS Services session ended") == 0);
    assert(osvc_session_update_send(&s_session, id, data, 1U) == 0U);
}

static void test_update_invalid_messages_fault_the_service(void)
{
    static const uint8_t data[64];
    struct {
        uint16_t kind;
        uint8_t payload[16];
        size_t length;
        bool ready;
    } cases[] = {
        {OSVC_UPDATE_READY, {0}, 7U, false},
        {OSVC_UPDATE_CREDIT, {0}, 8U, false}, /* before update.ready */
        {OSVC_UPDATE_READY, {0}, 8U, true},   /* a second update.ready */
        {OSVC_UPDATE_STATUS, {0, 0, 0, 0, 3}, 8U, false},
        {OSVC_UPDATE_STATUS, {0, 0, 0, 0, 1, 0, 1}, 8U, false},
        {OSVC_UPDATE_STATUS, {0, 0, 0, 0, 2, 0, 0, 0, 'x'}, 9U, false},
        /* Installing before every byte was sent. */
        {OSVC_UPDATE_STATUS, {0, 0, 0, 0, 0}, 8U, true},
        {OSVC_UPDATE_CHUNK, {0}, 12U, false},
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const uint32_t id = begin_update(sizeof(data));
        if (cases[i].ready) {
            sender_update_credit(OSVC_UPDATE_READY, id, 8U);
        }
        osvc_put_u32(cases[i].payload, id);
        send_message(OSVC_SERVICE_UPDATE, cases[i].kind, cases[i].payload,
                     cases[i].length);
        expect_message(OSVC_SERVICE_UPDATE, OSVC_UPDATE_CANCEL);
        assert(s_session.update.ended && osvc_session_open(&s_session));
        assert(s_session.service_faulted[OSVC_SERVICE_UPDATE]);
        assert(!osvc_session_update_available(&s_session));
    }
}

/* The relay's link to the sender model: send() relays what the credit
 * allows; when the sender has none left, it waits stall_ms and the sender
 * returns credit_step bytes, as a sender writing its staging file does. */
typedef struct {
    const uint8_t *package;
    size_t corrupt_at; /* SIZE_MAX: none */
    size_t read;
    uint32_t update_id;
    int64_t stall_ms;
    uint32_t credit_step;
    unsigned stalls;
    uint64_t delivered;
    uint8_t received[80000];
} relay_model_t;

static bool model_read(void *ctx, uint8_t *data, size_t length)
{
    relay_model_t *model = ctx;
    memcpy(data, model->package + model->read, length);
    if (model->corrupt_at - model->read < length) {
        data[model->corrupt_at - model->read] ^= 0x01U;
    }
    model->read += length;
    return true;
}

static bool model_send(void *ctx, const uint8_t *data, size_t length)
{
    relay_model_t *model = ctx;
    for (size_t done = 0U; done < length;) {
        if (s_session.update.ended) {
            return false;
        }
        const size_t n = osvc_session_update_send(&s_session, model->update_id,
                                                  data + done, length - done);
        model->delivered += read_update_chunks(model->update_id,
                                               model->delivered,
                                               model->received);
        done += n;
        if (n == 0U) {
            model->stalls++;
            s_fake.now += model->stall_ms;
            send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_PING, NULL, 0U);
            osvc_session_tick(&s_session);
            if (!s_session.update.ended) {
                sender_update_credit(OSVC_UPDATE_CREDIT, model->update_id,
                                     model->credit_step);
            }
        }
    }
    return true;
}

/* The signing keys of the test release key, read from the key as a
 * keyserver serves it (fixtures/test_release_key.asc). */
static size_t fixture_keys(osvc_pgp_key_t keys[4])
{
    FILE *f = fopen(FIXTURES_DIR "/test_release_key.asc", "rb");
    static uint8_t text[4096];
    assert(f != NULL);
    const size_t length = fread(text, 1U, sizeof(text), f);
    fclose(f);
    uint8_t root[OSVC_PGP_FINGERPRINT_LEN];
    for (size_t i = 0U; i < OSVC_PGP_FINGERPRINT_LEN; ++i) {
        unsigned value;
        assert(sscanf(OSVC_TEST_ROOT_FINGERPRINT + 2U * i, "%2x", &value) == 1);
        root[i] = (uint8_t)value;
    }
    size_t count = 0U;
    assert(osvc_pgp_read_keyring(text, length, root, &host_hash_ops, 0U, keys,
                                 4U, &count) == NULL);
    return count;
}

/* Relays fixtures/package.bin as listed in the index (sha) with its
 * signature, through credit stalls; returns the relay's verdict. */
static const char *relay_fixture(relay_model_t *model, const uint8_t sha[32])
{
    FILE *f = fopen(FIXTURES_DIR "/package.bin", "rb");
    static uint8_t package[70000];
    assert(f != NULL && fread(package, 1U, sizeof(package), f) == sizeof(package));
    fclose(f);
    f = fopen(FIXTURES_DIR "/package.bin.sig", "rb");
    uint8_t signature[512];
    const size_t signature_length = fread(signature, 1U, sizeof(signature), f);
    fclose(f);
    model->package = package;
    model->update_id = begin_update(sizeof(package));
    sender_update_credit(OSVC_UPDATE_READY, model->update_id, 40000U);
    osvc_pgp_key_t keys[4];
    const size_t key_count = fixture_keys(keys);
    osvc_package_check_t check;
    assert(osvc_package_check_begin(&check, &host_hash_ops, sizeof(package),
                                    sha, signature, signature_length, keys,
                                    key_count) == NULL);
    static uint8_t buffer_a[OSVC_CHUNK_MAX_DATA];
    static uint8_t buffer_b[OSVC_CHUNK_MAX_DATA];
    uint8_t *const buffers[2] = {buffer_a, buffer_b};
    const osvc_relay_io_t io = {model, model_read, model_send};
    const char *error = osvc_relay_package(&io, &check, buffers, 0U);
    if (error != NULL) {
        osvc_session_update_cancel(&s_session, model->update_id, error);
    }
    assert(host_hashes_open == 0);
    return error;
}

static void test_update_relay_holds_back_the_last_chunk(void)
{
    uint8_t sha[32];
    FILE *f = fopen(FIXTURES_DIR "/package.bin", "rb");
    static uint8_t package[70000];
    assert(f != NULL && fread(package, 1U, sizeof(package), f) == sizeof(package));
    fclose(f);
    SHA256(package, sizeof(package), sha);

    /* Through two stalls of 10 seconds: every byte, in order. */
    static relay_model_t model;
    model = (relay_model_t){.corrupt_at = SIZE_MAX, .stall_ms = 10000,
                            .credit_step = 16384U};
    assert(relay_fixture(&model, sha) == NULL);
    assert(model.delivered == sizeof(package) && model.stalls == 2U);
    assert(memcmp(model.received, package, sizeof(package)) == 0);
    sender_update_status(model.update_id, OSVC_UPDATE_STATUS_INSTALLING, "");
    assert(s_session.update.status == OSVC_UPDATE_STATUS_INSTALLING);

    /* A corrupted byte in the first chunk: that chunk went out, the last
     * never does, and update.cancel tells the sender. */
    model = (relay_model_t){.corrupt_at = 1000U, .stall_ms = 10000,
                            .credit_step = 65536U};
    assert(strcmp(relay_fixture(&model, sha),
                  "the package does not match the release index") == 0);
    assert(model.delivered == OSVC_CHUNK_MAX_DATA);
    expect_message(OSVC_SERVICE_UPDATE, OSVC_UPDATE_CANCEL);
    expect_no_message();

    /* An index that lists the corrupted package: the signature still
     * refuses it. */
    package[69999] ^= 0x01U;
    SHA256(package, sizeof(package), sha);
    model = (relay_model_t){.corrupt_at = 69999U, .stall_ms = 10000,
                            .credit_step = 65536U};
    assert(strcmp(relay_fixture(&model, sha),
                  "the package is not signed with the JetKVM release key") == 0);
    assert(model.delivered == OSVC_CHUNK_MAX_DATA);
    expect_message(OSVC_SERVICE_UPDATE, OSVC_UPDATE_CANCEL);
    package[69999] ^= 0x01U;
    SHA256(package, sizeof(package), sha);

    /* A sender that stops returning credit: the session gives up after 30
     * seconds and the relay stops. */
    model = (relay_model_t){.corrupt_at = SIZE_MAX, .stall_ms = 31000,
                            .credit_step = 1U};
    assert(strcmp(relay_fixture(&model, sha), "the update ended") == 0);
    assert(model.delivered == 40000U);
    expect_message(OSVC_SERVICE_UPDATE, OSVC_UPDATE_CANCEL);
    assert(strcmp(s_session.update.message,
                  "the update made no progress for 30 seconds") == 0);
}

/* --- Service video ------------------------------------------------------- */

#define KEY OSVC_VIDEO_FRAME_KEYFRAME
#define SETS OSVC_VIDEO_FRAME_PARAMETER_SETS

static const uint8_t s_sps[] = {0U, 0U, 0U, 1U, 0x67U, 0x42U, 0x00U, 0x1fU,
                                0xe9U, 0x02U};
/* With a 3-byte start code; the KVM keeps it behind a 4-byte one. */
static const uint8_t s_pps[] = {0U, 0U, 1U, 0x68U, 0xceU, 0x3cU, 0x80U};

/* An H.264 access unit: SPS and PPS when sets, then one IDR or non-IDR
 * slice with size bytes of data. */
static size_t make_unit(uint8_t *out, bool idr, bool sets, size_t size,
                        uint8_t seed)
{
    size_t n = 0U;
    if (sets) {
        memcpy(out, s_sps, sizeof(s_sps));
        memcpy(out + sizeof(s_sps), s_pps, sizeof(s_pps));
        n = sizeof(s_sps) + sizeof(s_pps);
    }
    const uint8_t slice[] = {0U, 0U, 0U, 1U, idr ? 0x65U : 0x41U};
    memcpy(out + n, slice, sizeof(slice));
    n += sizeof(slice);
    for (size_t i = 0U; i < size; ++i) {
        out[n++] = (uint8_t)(0x80U | ((seed + i) & 0x7fU));
    }
    return n;
}

static size_t encode_frame(uint8_t *out, uint64_t capture_us, uint32_t flags,
                           const uint8_t *unit, size_t length)
{
    osvc_put_envelope(out, OSVC_SERVICE_VIDEO, OSVC_VIDEO_FRAME,
                      (uint32_t)(OSVC_VIDEO_FRAME_HEADER_LEN + length));
    osvc_put_u64(out + OSVC_ENVELOPE_LEN, capture_us);
    osvc_put_u32(out + OSVC_ENVELOPE_LEN + 8U, flags);
    memcpy(out + OSVC_ENVELOPE_LEN + OSVC_VIDEO_FRAME_HEADER_LEN, unit, length);
    return OSVC_ENVELOPE_LEN + OSVC_VIDEO_FRAME_HEADER_LEN + length;
}

/* Sends one frame of a slice of size bytes; returns the hash of its unit. */
static uint32_t send_frame(uint64_t capture_us, uint32_t flags, bool idr,
                           bool sets, size_t size)
{
    static uint8_t unit[64U * 1024U];
    static uint8_t message[sizeof(unit) + 32U];
    const size_t length = make_unit(unit, idr, sets, size, (uint8_t)capture_us);
    feed(message, encode_frame(message, capture_us, flags, unit, length));
    return fake_video_hash(unit, length);
}

static void video_register(uint32_t width, uint32_t height, uint32_t fps)
{
    uint8_t caps[OSVC_VIDEO_CAPABILITIES_LEN];
    osvc_put_u32(caps, s_video_flags);
    osvc_put_u32(caps + 4, width);
    osvc_put_u32(caps + 8, height);
    osvc_put_u32(caps + 12, fps);
    send_message(OSVC_SERVICE_VIDEO, OSVC_VIDEO_CAPABILITIES, caps,
                 sizeof(caps));
}

static void video_status(bool enabled, bool reset, uint32_t width,
                         uint32_t height, uint32_t fps, uint32_t error)
{
    uint8_t status[OSVC_VIDEO_STATUS_MIN_LEN] = {0};
    status[0] = enabled ? 1U : 0U;
    status[3] = reset ? 1U : 0U;
    osvc_put_u32(status + 4, width);
    osvc_put_u32(status + 8, height);
    osvc_put_u32(status + 12, fps);
    osvc_put_u32(status + 28, error);
    send_message(OSVC_SERVICE_VIDEO, OSVC_VIDEO_STATUS, status, sizeof(status));
}

/* video.config: enabled, size, and the targets. */
static void expect_video_config(bool enabled, uint32_t width, uint32_t height,
                                uint32_t kbps, uint32_t fps)
{
    const fake_message_t config =
        expect_message(OSVC_SERVICE_VIDEO, OSVC_VIDEO_CONFIG);
    assert(config.length == OSVC_VIDEO_CONFIG_LEN);
    assert(config.payload[0] == (enabled ? 1U : 0U));
    assert(config.payload[2] == OSVC_CODEC_H264);
    assert(osvc_get_u32(config.payload + 4) == width);
    assert(osvc_get_u32(config.payload + 8) == height);
    assert(osvc_get_u32(config.payload + 12) == kbps);
    assert(osvc_get_u32(config.payload + 16) == fps);
}

static osvc_settings_t settings_1080p(void)
{
    osvc_settings_t settings = default_settings();
    settings.width = 1920U;
    settings.height = 1080U;
    return settings;
}

/* An open session on the video platform, nothing written since. */
static void video_open(const osvc_settings_t *settings)
{
    fake_platform_init(&s_fake, &s_platform);
    fake_video_attach(&s_platform);
    osvc_session_init(&s_session, &s_platform, settings, s_payload_buffer,
                      sizeof(s_payload_buffer));
    uint8_t token[OSVC_TOKEN_LEN];
    open_session(token);
}

/* Registered, presented and showing a 1920x1080 stream at 30 fps, after its
 * first keyframe; nothing written since. */
static void video_show(void)
{
    const osvc_settings_t settings = settings_1080p();
    video_open(&settings);
    video_register(3840U, 2160U, 60U);
    osvc_session_display_status(&s_session, true, true);
    expect_video_config(true, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    assert(s_session.video_source == OSVC_SOURCE_GUEST);
    (void)send_frame(1000000U, KEY | SETS, true, true, 100U);
    assert(g_fake_video.frame_count == 1U);
    expect_no_message();
}

/* Time passes with a ping from the sender every second and a tick every
 * 100 ms. */
static void video_wait(int64_t ms)
{
    for (int64_t t = 0; t < ms; t += 100) {
        s_fake.now += 100;
        if (s_fake.now % 1000 == 0) {
            send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_PING, NULL, 0U);
        }
        osvc_session_tick(&s_session);
    }
}

/* Switch to service video: capture stops and the display is presented
 * first; video.config with enabled 1 waits for it, frames wait for the
 * enabled status. */
static void test_video_switch_waits_for_the_display(void)
{
    const osvc_settings_t settings = default_settings();
    video_open(&settings);
    video_register(3840U, 2160U, 60U);
    assert(g_fake_video.presents == 1U && g_fake_video.present_width == 2560U &&
           g_fake_video.present_height == 1440U);
    assert(s_session.video_source == OSVC_SOURCE_SWITCHING_TO_GUEST);
    osvc_session_display_status(&s_session, false, true);
    expect_no_message();
    osvc_session_display_status(&s_session, true, true);
    expect_video_config(true, 2560U, 1440U, OSVC_VIDEO_KBPS_1440P, 60U);
    (void)send_frame(1U, KEY | SETS, true, true, 10U);
    assert(g_fake_video.reserves == 0U &&
           s_session.video_counters.frames_discarded == 1U);
    video_status(true, true, 2560U, 1440U, 30U, 0U);
    assert(s_session.video_source == OSVC_SOURCE_GUEST);
    assert(g_fake_video.streams == 1U && g_fake_video.stream_width == 2560U &&
           g_fake_video.stream_fps == 30U);
    /* The effective rate is the cap when it is lower than the native one. */
    uint8_t status[OSVC_VIDEO_STATUS_MIN_LEN] = {1U, 1U, 0U, 1U};
    osvc_put_u32(status + 4, 2560U);
    osvc_put_u32(status + 8, 1440U);
    osvc_put_u32(status + 12, 60U);
    osvc_put_u32(status + 24, 24U);
    send_message(OSVC_SERVICE_VIDEO, OSVC_VIDEO_STATUS, status, sizeof(status));
    assert(g_fake_video.streams == 2U && g_fake_video.stream_fps == 24U);
    expect_no_message();
}

/* Frames split across USB reads at every boundary reach the slot intact. */
static void test_video_frames_across_usb_reads(void)
{
    static const size_t chunks[] = {1U, 5U, 12U, 13U, 511U, 4096U, 65536U};
    static uint8_t stream[64U * 1024U];
    static uint8_t unit[8U * 1024U];
    for (size_t c = 0U; c < sizeof(chunks) / sizeof(chunks[0]); ++c) {
        video_show();
        uint32_t hashes[6];
        size_t n = 0U;
        for (size_t i = 0U; i < 6U; ++i) {
            const bool idr = i == 3U;
            const size_t length =
                make_unit(unit, idr, idr, 1000U + i * 517U, (uint8_t)i);
            hashes[i] = fake_video_hash(unit, length);
            n += encode_frame(stream + n, 1033333U + i * 33333U,
                              idr ? KEY | SETS : 0U, unit, length);
        }
        feed_chunked(stream, n, chunks[c]);
        assert(g_fake_video.frame_count == 7U);
        for (size_t i = 0U; i < 6U; ++i) {
            const fake_video_frame_t *frame = &g_fake_video.frames[1U + i];
            assert(frame->hash == hashes[i]);
            assert(frame->meta.keyframe == (i == 3U));
            assert(frame->meta.width == 1920U && frame->meta.fps == 30U);
        }
        assert(s_session.video_counters.frames == 7U &&
               s_session.video_counters.keyframes == 2U);
        assert(osvc_session_open(&s_session));
    }
}

static void expect_video_fault(const char *reason)
{
    assert(strcmp(s_session.video_error, reason) == 0);
    assert(!s_session.video_registered);
    assert(s_session.video_source == OSVC_SOURCE_SWITCHING_TO_PHYSICAL);
    assert(g_fake_video.restores == 1U && g_fake_video.stopped == 1U);
    expect_video_config(false, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);
    assert(osvc_session_open(&s_session));
}

/* After stream_reset 1 the next frame is a keyframe with SPS and PPS. */
static void test_video_stream_reset(void)
{
    video_show();
    (void)send_frame(1033333U, 0U, false, false, 100U);
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    /* The platform discards what is queued from before. */
    assert(g_fake_video.streams == 2U && s_session.parameter_sets_length == 0U);
    (void)send_frame(1066666U, 0U, false, false, 100U);
    expect_video_fault("the first frame of a stream is not a keyframe with "
                       "parameter sets");
    assert(g_fake_video.reserves == 2U);

    video_show();
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    (void)send_frame(1066666U, KEY | SETS, true, false, 100U);
    expect_video_fault("a video.frame flagged with parameter sets has no SPS "
                       "and PPS");
    /* The slot went back with what was written to it. */
    assert(g_fake_video.cancels == 1U &&
           g_fake_video.cancel_written == 5U + 100U);

    /* A size change without stream_reset is a new stream too, without the
     * SPS and PPS of the previous size. */
    video_show();
    assert(s_session.parameter_sets_length != 0U);
    video_status(true, false, 1280U, 720U, 30U, 0U);
    assert(g_fake_video.streams == 2U && g_fake_video.stream_width == 1280U);
    assert(s_session.parameter_sets_length == 0U);
    (void)send_frame(1066666U, 0U, false, false, 100U);
    assert(g_fake_video.frame_count == 2U &&
           g_fake_video.frames[1].meta.width == 1280U);
}

/* The rule holds for what the sender sends: a first keyframe that no viewer
 * takes, or that is dropped, still starts the stream, and the deltas after
 * it are read past the same way. */
static void test_video_stream_reset_without_viewer(void)
{
    video_show();
    g_fake_video.no_viewer = true;
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    (void)send_frame(1033333U, KEY | SETS, true, true, 100U);
    (void)send_frame(1066666U, 0U, false, false, 100U);
    assert(s_session.video_registered && s_session.video_error[0] == '\0');
    assert(s_session.video_counters.frames_discarded == 2U);
    /* With a viewer again, frames reach it. */
    g_fake_video.no_viewer = false;
    (void)send_frame(1099999U, KEY | SETS, true, true, 100U);
    assert(g_fake_video.frame_count == 2U && g_fake_video.frames[1].meta.keyframe);
}

/* A stream's first keyframe that no viewer takes still gives its SPS and
 * PPS to a later keyframe without them; it is larger than the payload
 * buffer and comes over several reads. */
static void test_video_parameter_sets_from_a_discarded_keyframe(void)
{
    video_show();
    g_fake_video.no_viewer = true;
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    static uint8_t first[2U * OSVC_MAX_CONTROL_PAYLOAD];
    static uint8_t message[sizeof(first) + 32U];
    const size_t first_length =
        make_unit(first, true, true, OSVC_MAX_CONTROL_PAYLOAD + 100U, 3U);
    feed_chunked(message,
                 encode_frame(message, 1033333U, KEY | SETS, first, first_length),
                 16384U);
    assert(s_session.video_counters.frames_discarded == 1U);
    g_fake_video.no_viewer = false;
    const uint32_t hash = send_frame(1066666U, KEY, true, false, 100U);
    const fake_video_frame_t *frame = &g_fake_video.frames[1];
    assert(g_fake_video.frame_count == 2U && frame->meta.parameter_sets_added);
    static const uint8_t pps4[] = {0U, 0U, 0U, 1U, 0x68U, 0xceU, 0x3cU, 0x80U};
    assert(memcmp(g_fake_video.slot, s_sps, sizeof(s_sps)) == 0);
    assert(memcmp(g_fake_video.slot + sizeof(s_sps), pps4, sizeof(pps4)) == 0);
    const size_t sets = sizeof(s_sps) + sizeof(pps4);
    assert(fake_video_hash(g_fake_video.slot + sets, frame->meta.length - sets) ==
           hash);
}

/* A discarded keyframe whose PPS ends past the payload buffer gives nothing:
 * a cut PPS is never put in front of a keyframe. */
static void test_video_parameter_sets_cut_short_not_kept(void)
{
    video_show();
    g_fake_video.no_viewer = true;
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    /* An SEI that puts the end of the buffer inside the PPS. */
    static uint8_t first[2U * OSVC_MAX_CONTROL_PAYLOAD];
    const size_t sei = OSVC_MAX_CONTROL_PAYLOAD - sizeof(s_sps) - 5U;
    memcpy(first, (const uint8_t[]){0U, 0U, 0U, 1U, 0x06U}, 5U);
    memset(first + 5U, 0x80U, sei - 5U);
    const size_t first_length = sei + make_unit(first + sei, true, true, 100U, 3U);
    static uint8_t message[sizeof(first) + 32U];
    feed(message, encode_frame(message, 1033333U, KEY | SETS, first, first_length));
    g_fake_video.no_viewer = false;
    (void)send_frame(1066666U, KEY, true, false, 100U);
    assert(g_fake_video.frame_count == 2U &&
           !g_fake_video.frames[1].meta.parameter_sets_added);
}

/* A keyframe without SPS and PPS gets the stream's, from the cache. */
static void test_video_parameter_sets_prepended(void)
{
    video_show();
    const size_t cached = s_session.parameter_sets_length;
    assert(cached == sizeof(s_sps) + 1U + sizeof(s_pps));
    assert(memcmp(s_session.parameter_sets, s_sps, sizeof(s_sps)) == 0);
    static const uint8_t pps4[] = {0U, 0U, 0U, 1U, 0x68U, 0xceU, 0x3cU, 0x80U};
    assert(memcmp(s_session.parameter_sets + sizeof(s_sps), pps4,
                  sizeof(pps4)) == 0);
    static uint8_t unit[1024];
    const size_t length = make_unit(unit, true, false, 300U, 9U);
    static uint8_t message[2048];
    feed(message, encode_frame(message, 1033333U, KEY, unit, length));
    const fake_video_frame_t *frame = &g_fake_video.frames[1];
    assert(frame->meta.keyframe && frame->meta.parameter_sets_added);
    assert(frame->meta.length == cached + length);
    assert(memcmp(g_fake_video.slot, s_session.parameter_sets, cached) == 0);
    assert(memcmp(g_fake_video.slot + cached, unit, length) == 0);
    assert(s_session.video_counters.parameter_sets_added == 1U);
    /* Delta frames and keyframes that carry their own get nothing. */
    (void)send_frame(1066666U, 0U, false, false, 50U);
    (void)send_frame(1099999U, KEY | SETS, true, true, 50U);
    assert(!g_fake_video.frames[2].meta.parameter_sets_added &&
           !g_fake_video.frames[3].meta.parameter_sets_added);
    assert(s_session.video_counters.parameter_sets_added == 1U);
}

/* Sends one frame of unit; true when the viewer got it. */
static bool send_unit(uint64_t capture_us, uint32_t flags, const uint8_t *unit,
                      size_t length)
{
    static uint8_t message[8192];
    const size_t frames = g_fake_video.frame_count;
    feed(message, encode_frame(message, capture_us, flags, unit, length));
    return g_fake_video.frame_count == frames + 1U;
}

/* video_stream() says what each slot of the stream takes, and that is the
 * limit: a keyframe that does not fit, with the cached SPS and PPS it would
 * get, faults video; a larger delta frame is dropped as a gap. */
static void test_video_frame_limits_follow_the_stream(void)
{
    static uint8_t key[4096];
    static uint8_t bare_key[4096];
    static uint8_t delta[4096];
    const size_t key_length = make_unit(key, true, true, 2000U, 1U);
    const size_t bare_length = make_unit(bare_key, true, false, 2000U, 2U);
    const size_t delta_length = make_unit(delta, false, false, 2000U, 3U);
    char reason[64];

    video_show();
    const size_t cached = s_session.parameter_sets_length;
    g_fake_video.capacity = bare_length + cached;
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    assert(g_fake_video.streams == 2U);
    assert(send_unit(1033333U, KEY | SETS, key, key_length));
    assert(send_unit(1066666U, KEY, bare_key, bare_length));
    assert(g_fake_video.frames[2].meta.length == g_fake_video.capacity);
    g_fake_video.capacity = delta_length - 1U;
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    (void)send_frame(1099999U, KEY | SETS, true, true, 100U);
    assert(!send_unit(1133332U, 0U, delta, delta_length));
    assert(s_session.video_counters.frames_dropped == 1U &&
           g_fake_video.gaps == 1U && s_session.video_registered);

    video_show();
    g_fake_video.capacity = bare_length + cached - 1U;
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    (void)send_frame(1033333U, KEY | SETS, true, true, 100U);
    (void)send_unit(1066666U, KEY, bare_key, bare_length);
    snprintf(reason, sizeof(reason), "a keyframe above %zu bytes",
             g_fake_video.capacity);
    expect_video_fault(reason);
}

static uint32_t pts_90k(uint64_t us)
{
    return (uint32_t)(us * 9U / 100U);
}

/* pts follows the sender's capture spacing from 1 us to 1 s, else one frame
 * at the stream's rate; a new stream starts at the KVM's clock, never before
 * the frame before it. */
static void test_video_pts(void)
{
    video_show();
    uint64_t pts_us = (uint64_t)g_fake_video.frames[0].meta.arrival_us;
    assert(g_fake_video.frames[0].meta.pts_90k == pts_90k(pts_us));
    static const struct {
        uint64_t capture_us;
        uint64_t step_us;
    } steps[] = {
        {1033333U, 33333U},  /* the capture spacing */
        {1033333U, 33333U},  /* no spacing: one frame at 30 fps */
        {3033333U, 33333U},  /* above 1 s */
        {3000000U, 33333U},  /* backwards */
        {3000001U, 1U},      /* 1 us */
        {4000001U, 1000000U}, /* 1 s */
    };
    for (size_t i = 0U; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        (void)send_frame(steps[i].capture_us, 0U, false, false, 20U);
        pts_us += steps[i].step_us;
        assert(g_fake_video.frames[1U + i].meta.pts_90k == pts_90k(pts_us));
    }
    /* A new stream right away continues one frame after the last. */
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    (void)send_frame(7U, KEY | SETS, true, true, 20U);
    pts_us += 33333U;
    assert(g_fake_video.frames[7].meta.pts_90k == pts_90k(pts_us));
    /* Later, at the KVM's clock. */
    s_fake.now += 10000;
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_PING, NULL, 0U);
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    g_fake_video.us_offset = 250;
    (void)send_frame(9U, KEY | SETS, true, true, 20U);
    assert(g_fake_video.frames[8].meta.pts_90k ==
           pts_90k((uint64_t)s_fake.now * 1000U + 250U));
    assert(g_fake_video.frames[8].meta.arrival_us == s_fake.now * 1000 + 250);
}

/* The frames the sender left out at the stream's rate, from the capture
 * times of the frames it sent: a gap of 1.5 frame periods or more is one,
 * of 3 periods two. Frames the KVM did not deliver were still sent; gaps
 * above 1 s, backwards and across a stream's start do not count. */
static void test_video_sender_skipped_frames(void)
{
    video_show();
    const uint32_t *skipped = &s_session.video_counters.sender_skipped_frames;
    static const struct {
        uint64_t capture_us;
        uint32_t skipped;
    } steps[] = {
        {1033333U, 0U}, /* one period at 30 fps */
        {1083333U, 1U}, /* 1.5 periods */
        {1133332U, 1U}, /* just under 1.5 periods */
        {1233332U, 3U}, /* 3 periods */
        {2233333U, 3U}, /* above 1 s */
        {2200000U, 3U}, /* backwards */
        {2266666U, 4U}, /* 2 periods after the frame that went backwards */
    };
    for (size_t i = 0U; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        (void)send_frame(steps[i].capture_us, 0U, false, false, 20U);
        assert(*skipped == steps[i].skipped);
    }
    g_fake_video.no_viewer = true;
    (void)send_frame(2300000U, 0U, false, false, 20U);
    (void)send_frame(2366666U, 0U, false, false, 20U);
    g_fake_video.no_viewer = false;
    (void)send_frame(2400000U, 0U, false, false, 20U);
    assert(*skipped == 5U && s_session.video_counters.frames_discarded == 2U);

    /* A new stream, at 60 fps: its first frame comes 0.5 s after the last
     * one of the stream before. */
    video_status(true, true, 1920U, 1080U, 60U, 0U);
    (void)send_frame(2900000U, KEY | SETS, true, true, 20U);
    assert(*skipped == 5U);
    (void)send_frame(2916667U, 0U, false, false, 20U);
    (void)send_frame(2950000U, 0U, false, false, 20U);
    assert(*skipped == 6U);
    /* Frames read past while no stream is shown start the count again. */
    video_status(false, false, 0U, 0U, 0U, 0U);
    (void)send_frame(3000000U, 0U, false, false, 20U);
    assert(*skipped == 6U);
}

static unsigned keyframe_requests(void)
{
    unsigned count = 0U;
    fake_message_t message;
    while (fake_next_message(&s_fake, &message)) {
        assert(message.service == OSVC_SERVICE_VIDEO &&
               message.kind == OSVC_VIDEO_REQUEST_KEYFRAME &&
               message.length == 0U);
        count++;
    }
    return count;
}

/* The platform's keyframe request is asked of the sender at most every 600
 * ms, given back undelivered when no keyframe came in time, and answered by
 * the next keyframe, which carries its generation. */
static void test_video_keyframe_request_rate(void)
{
    video_show();
    g_fake_video.keyframe_pending = 7U;
    osvc_session_tick(&s_session);
    assert(keyframe_requests() == 1U);
    video_wait(500);
    assert(keyframe_requests() == 0U && g_fake_video.keyframe_done_count == 0U);
    video_wait(100);
    assert(keyframe_requests() == 1U);
    assert(g_fake_video.keyframe_done_count == 1U &&
           g_fake_video.keyframe_done[0].generation == 7U &&
           !g_fake_video.keyframe_done[0].delivered);
    video_wait(3000);
    assert(keyframe_requests() == 5U);
    /* Deltas carry no generation; the keyframe answers the request. */
    (void)send_frame(1033333U, 0U, false, false, 20U);
    assert(g_fake_video.frames[1].meta.keyframe_generation == 0U);
    (void)send_frame(1066666U, KEY | SETS, true, true, 20U);
    assert(g_fake_video.frames[2].meta.keyframe_generation == 7U);
    const fake_keyframe_done_t *done =
        &g_fake_video.keyframe_done[g_fake_video.keyframe_done_count - 1U];
    assert(done->generation == 7U && done->delivered);
    video_wait(3000);
    assert(keyframe_requests() == 0U);
    /* A keyframe that comes first answers a request not yet asked. */
    g_fake_video.keyframe_pending = 8U;
    (void)send_frame(1099999U, KEY | SETS, true, true, 20U);
    assert(g_fake_video.frames[3].meta.keyframe_generation == 8U);
    video_wait(1000);
    assert(keyframe_requests() == 0U);
    /* A keyframe the platform could not take answers nothing. */
    g_fake_video.keyframe_pending = 9U;
    g_fake_video.refuse_commit = true;
    (void)send_frame(1133332U, KEY | SETS, true, true, 20U);
    done = &g_fake_video.keyframe_done[g_fake_video.keyframe_done_count - 1U];
    assert(done->generation == 9U && !done->delivered);
    assert(g_fake_video.keyframe_pending == 9U);
}

/* No free slot holds the read (backpressure), also for as long as a weak
 * Wi-Fi link takes to send a keyframe; after
 * OSVC_VIDEO_SLOT_WAIT_MS the frame is dropped and the platform hears of the
 * gap. */
static void test_video_backpressure_and_gap(void)
{
    video_show();
    static uint8_t unit[4096];
    static uint8_t message[4096 + 32];
    const size_t length = make_unit(unit, false, false, 2000U, 3U);
    const size_t total = encode_frame(message, 1033333U, 0U, unit, length);
    const size_t head = OSVC_ENVELOPE_LEN + OSVC_VIDEO_FRAME_HEADER_LEN;
    g_fake_video.busy = true;
    assert(osvc_session_receive(&s_session, message, total) == head);
    assert(osvc_session_frame_waiting(&s_session));
    s_fake.now += 800;
    assert(osvc_session_receive(&s_session, message + head, total - head) == 0U);
    g_fake_video.busy = false;
    feed(message + head, total - head);
    assert(g_fake_video.frame_count == 2U &&
           g_fake_video.frames[1].hash == fake_video_hash(unit, length));
    assert(s_session.video_counters.slot_waits == 1U &&
           s_session.video_counters.slot_wait_max_ms == 800U);
    assert(g_fake_video.gaps == 0U);

    g_fake_video.busy = true;
    assert(osvc_session_receive(&s_session, message, total) == head);
    s_fake.now += OSVC_VIDEO_SLOT_WAIT_MS - 1;
    assert(osvc_session_receive(&s_session, message + head, total - head) == 0U);
    s_fake.now += 1;
    feed(message + head, total - head);
    assert(g_fake_video.frame_count == 2U && g_fake_video.gaps == 1U);
    assert(s_session.video_counters.frames_dropped == 1U);
    /* The stream stays aligned. */
    g_fake_video.busy = false;
    (void)send_frame(1100000U, 0U, false, false, 30U);
    assert(g_fake_video.frame_count == 3U);
    /* Without a viewer, frames are read past at once. */
    g_fake_video.no_viewer = true;
    (void)send_frame(1133333U, 0U, false, false, 30U);
    assert(g_fake_video.frame_count == 3U && g_fake_video.gaps == 1U);
    assert(s_session.video_counters.frames_discarded == 1U);
}

/* 45 s for the first enabled status; 15 s for the disabled status. */
static void test_video_timers(void)
{
    const osvc_settings_t settings = settings_1080p();
    video_open(&settings);
    video_register(1920U, 1080U, 60U);
    osvc_session_display_status(&s_session, true, true);
    expect_video_config(true, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);
    video_wait(OSVC_VIDEO_SWITCH_TIMEOUT_MS - 100);
    assert(s_session.video_source == OSVC_SOURCE_SWITCHING_TO_GUEST);
    video_wait(100);
    assert(s_session.video_source == OSVC_SOURCE_SWITCHING_TO_PHYSICAL);
    assert(g_fake_video.restores == 1U &&
           s_session.video_counters.switch_failures == 1U);
    assert(strcmp(s_session.video_error, "the sender did not start video within 45 seconds") == 0);
    expect_video_config(false, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);
    osvc_session_display_status(&s_session, false, false);
    assert(s_session.video_source == OSVC_SOURCE_PHYSICAL);
    /* The status the sender sent before it saw enabled 0 is stale, not a
     * fault; after its disabled status, an enabled one is. */
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    assert(s_session.video_registered && g_fake_video.streams == 0U);
    video_status(false, false, 0U, 0U, 0U, 0U);
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    assert(!s_session.video_registered);

    /* The user turns video off: the display stays until the disabled
     * status, or 15 seconds. */
    for (int acknowledged = 0; acknowledged < 2; ++acknowledged) {
        video_show();
        osvc_settings_t off = settings_1080p();
        off.video_enabled = false;
        osvc_session_apply_settings(&s_session, &off);
        expect_video_config(false, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);
        assert(s_session.video_source == OSVC_SOURCE_SWITCHING_TO_PHYSICAL);
        assert(g_fake_video.stopped == 1U && g_fake_video.restores == 0U);
        (void)send_frame(1033333U, 0U, false, false, 30U);
        assert(g_fake_video.frame_count == 1U);
        if (acknowledged) {
            video_status(false, false, 0U, 0U, 0U, 0U);
        } else {
            video_wait(OSVC_VIDEO_DISABLE_TIMEOUT_MS - 100);
            assert(g_fake_video.restores == 0U);
            video_wait(100);
        }
        assert(g_fake_video.restores == 1U);
        osvc_session_display_status(&s_session, false, true);
        assert(s_session.video_source == OSVC_SOURCE_SWITCHING_TO_PHYSICAL);
        osvc_session_display_status(&s_session, false, false);
        assert(s_session.video_source == OSVC_SOURCE_PHYSICAL);
        expect_no_message();
    }
}

/* A firmware install holds display handovers. A registration meanwhile is
 * answered at once with video disabled, the switch outlasts the install,
 * its timeout starts again when the hold ends, and video starts once the
 * display is presented. */
static void test_video_registration_while_display_held(void)
{
    const osvc_settings_t settings = settings_1080p();
    video_open(&settings);
    osvc_session_display_held(&s_session, true);
    video_register(1920U, 1080U, 60U);
    assert(s_session.video_source == OSVC_SOURCE_SWITCHING_TO_GUEST);
    osvc_session_display_held(&s_session, true);
    expect_video_config(false, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);
    video_status(false, false, 0U, 0U, 0U, 0U);
    video_wait(2 * OSVC_VIDEO_SWITCH_TIMEOUT_MS);
    assert(s_session.video_source == OSVC_SOURCE_SWITCHING_TO_GUEST &&
           g_fake_video.restores == 0U);
    expect_no_message();
    osvc_session_display_held(&s_session, false);
    video_wait(OSVC_VIDEO_SWITCH_TIMEOUT_MS - 100);
    assert(s_session.video_source == OSVC_SOURCE_SWITCHING_TO_GUEST);
    osvc_session_display_status(&s_session, true, true);
    expect_video_config(true, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    assert(s_session.video_source == OSVC_SOURCE_GUEST &&
           g_fake_video.streams == 1U);
}

/* A new session after a restore; the display is back. */
static void video_reconnect(void)
{
    uint8_t token[OSVC_TOKEN_LEN];
    open_session(token);
    osvc_session_display_status(&s_session, false, false);
    assert(s_session.video_source == OSVC_SOURCE_PHYSICAL);
}

/* Three switches in two minutes, across sessions; the fourth is refused and
 * automatic switches stay blocked until the user turns video off and on. */
static void test_video_retry_budget(void)
{
    const osvc_settings_t settings = settings_1080p();
    video_open(&settings);
    for (unsigned i = 0U; i < OSVC_VIDEO_RETRY_SWITCHES; ++i) {
        video_register(1920U, 1080U, 60U);
        assert(g_fake_video.presents == i + 1U);
        s_fake.now += 30000;
        video_reconnect();
    }
    video_register(1920U, 1080U, 60U);
    assert(g_fake_video.presents == 3U && s_session.video_recovery.blocked);
    expect_video_config(false, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);
    assert(strstr(s_session.video_error, "too often") != NULL);
    /* The block outlives the window and the session. */
    s_fake.now += OSVC_VIDEO_RETRY_WINDOW_MS;
    video_reconnect();
    video_register(1920U, 1080U, 60U);
    assert(g_fake_video.presents == 3U);
    expect_video_config(false, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);
    osvc_settings_t off = settings;
    off.video_enabled = false;
    osvc_session_apply_settings(&s_session, &off);
    expect_video_config(false, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);
    osvc_session_apply_settings(&s_session, &settings);
    assert(g_fake_video.presents == 4U && !s_session.video_recovery.blocked);

    /* An inactive display gives the current switch back to the budget. */
    video_open(&settings);
    for (unsigned i = 0U; i < 5U; ++i) {
        video_register(1920U, 1080U, 60U);
        assert(g_fake_video.presents == i + 1U);
        osvc_session_display_status(&s_session, true, true);
        expect_video_config(true, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);
        video_status(false, false, 0U, 0U, 0U,
                     OSVC_VIDEO_ERROR_DISPLAY_INACTIVE);
        assert(g_fake_video.restores == i + 1U);
        osvc_session_display_status(&s_session, false, false);
        uint8_t withdraw[OSVC_VIDEO_CAPABILITIES_LEN] = {0};
        send_message(OSVC_SERVICE_VIDEO, OSVC_VIDEO_CAPABILITIES, withdraw,
                     sizeof(withdraw));
    }
    assert(!s_session.video_recovery.blocked);

    /* The KVM's own USB restarts (mounts, unmounts) give the current switch
     * back; the host's reconfigurations count. */
    video_open(&settings);
    for (unsigned i = 0U; i < 5U; ++i) {
        video_register(1920U, 1080U, 60U);
        assert(g_fake_video.presents == i + 1U);
        osvc_session_usb_reconfigured(&s_session, true);
        video_reconnect();
    }
    assert(!s_session.video_recovery.blocked);
    for (unsigned i = 0U; i < OSVC_VIDEO_RETRY_SWITCHES; ++i) {
        video_register(1920U, 1080U, 60U);
        osvc_session_usb_reconfigured(&s_session, false);
        video_reconnect();
    }
    video_register(1920U, 1080U, 60U);
    assert(g_fake_video.presents == 8U && s_session.video_recovery.blocked);
    expect_video_config(false, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);
}

/* A switch that reaches service video at 1920x1080. */
static void video_switch_shown(void)
{
    video_register(1920U, 1080U, 60U);
    osvc_session_display_status(&s_session, true, true);
    expect_video_config(true, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    assert(s_session.video_source == OSVC_SOURCE_GUEST);
}

/* A switch that has shown service video for OSVC_VIDEO_RETRY_SHOWN_MS has
 * succeeded: sender restarts after such switches never block video. A
 * switch whose session ends sooner still counts. */
static void test_video_retry_budget_shown(void)
{
    const osvc_settings_t settings = settings_1080p();
    video_open(&settings);
    for (unsigned i = 0U; i < 5U; ++i) {
        video_switch_shown();
        assert(g_fake_video.presents == i + 1U);
        video_wait(OSVC_VIDEO_RETRY_SHOWN_MS);
        video_reconnect();
    }
    assert(!s_session.video_recovery.blocked &&
           s_session.video_recovery.count == 0U);

    video_open(&settings);
    for (unsigned i = 0U; i < OSVC_VIDEO_RETRY_SWITCHES; ++i) {
        video_switch_shown();
        video_wait(OSVC_VIDEO_RETRY_SHOWN_MS - 1000);
        video_reconnect();
    }
    video_register(1920U, 1080U, 60U);
    assert(g_fake_video.presents == OSVC_VIDEO_RETRY_SWITCHES &&
           s_session.video_recovery.blocked);
    expect_video_config(false, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);

    /* A switch leaves the budget once: an inactive display after it has
     * been shown does not take an earlier switch out as well. */
    video_open(&settings);
    video_register(1920U, 1080U, 60U);
    video_reconnect();
    video_switch_shown();
    video_wait(OSVC_VIDEO_RETRY_SHOWN_MS);
    video_status(false, false, 0U, 0U, 0U, OSVC_VIDEO_ERROR_DISPLAY_INACTIVE);
    assert(s_session.video_recovery.count == 1U);
}

/* The KVM's update into the open session, which ends with the sender's
 * update.status. */
static void relay_update(uint8_t status)
{
    static const uint8_t data[4] = {1, 2, 3, 4};
    static const uint8_t sha[32];
    register_update();
    uint32_t id = 0U;
    assert(osvc_session_update_begin(&s_session, sizeof(data), sha, "0.4.2",
                                     &id) == NULL);
    expect_message(OSVC_SERVICE_UPDATE, OSVC_UPDATE_BEGIN);
    sender_update_credit(OSVC_UPDATE_READY, id, sizeof(data));
    assert(osvc_session_update_send(&s_session, id, data, sizeof(data)) ==
           sizeof(data));
    assert(read_update_chunks(id, 0U, NULL) == sizeof(data));
    sender_update_status(id, status, status == OSVC_UPDATE_STATUS_FAILED ?
                                         "no space" : "");
    assert(s_session.update.ended && s_session.update.status == status);
}

/* The sender exits to install the KVM's update, and the next
 * session switches back; as with the KVM's own USB restart, that switch
 * does not count. Video on, a sender restart, the update, then one more
 * sender restart within two minutes. */
static void test_video_retry_budget_after_update(void)
{
    const osvc_settings_t settings = settings_1080p();
    video_open(&settings);
    video_register(1920U, 1080U, 60U);
    s_fake.now += 17000;
    video_reconnect();
    video_register(1920U, 1080U, 60U);
    s_fake.now += 21000;
    relay_update(OSVC_UPDATE_STATUS_INSTALLING);
    video_reconnect();
    video_register(1920U, 1080U, 60U);
    s_fake.now += 18000;
    video_reconnect();
    video_register(1920U, 1080U, 60U);
    assert(g_fake_video.presents == 4U && !s_session.video_recovery.blocked);
    /* Only the update's own session end is free: the session after it
     * counts, so one more restart in the window is refused. */
    video_reconnect();
    video_register(1920U, 1080U, 60U);
    assert(g_fake_video.presents == 4U && s_session.video_recovery.blocked);

    /* A failed update leaves the sender running; its later end counts. */
    video_open(&settings);
    video_register(1920U, 1080U, 60U);
    video_reconnect();
    video_register(1920U, 1080U, 60U);
    relay_update(OSVC_UPDATE_STATUS_FAILED);
    video_reconnect();
    video_register(1920U, 1080U, 60U);
    video_reconnect();
    video_register(1920U, 1080U, 60U);
    assert(g_fake_video.presents == 3U && s_session.video_recovery.blocked);
}

/* Rule breaks fault video only: back to hardware capture, video.config with
 * enabled 0, the reason for the browser; new capabilities clear it. */
static void test_video_faults(void)
{
    uint8_t withdraw[OSVC_VIDEO_CAPABILITIES_LEN] = {0};
    video_show();
    (void)send_frame(1033333U, KEY | SETS, false, true, 100U);
    expect_video_fault("the video.frame keyframe flag does not match its "
                       "access unit");
    video_register(3840U, 2160U, 60U);
    assert(s_session.video_registered && s_session.video_error[0] == '\0');

    video_show();
    (void)send_frame(1033333U, 4U, false, false, 100U);
    expect_video_fault("a video.frame with unknown flags");
    /* The sender's disabled status goes unread while video is detached; its
     * withdrawal is no second fault. */
    video_status(false, false, 0U, 0U, 0U, 0U);
    send_message(OSVC_SERVICE_VIDEO, OSVC_VIDEO_CAPABILITIES, withdraw,
                 sizeof(withdraw));
    assert(s_session.video_counters.faults == 1U);

    /* Too large for the stream's slots: refused at its head, then read
     * past. */
    video_show();
    static uint8_t big[OSVC_ENVELOPE_LEN + OSVC_VIDEO_FRAME_HEADER_LEN +
                       FAKE_VIDEO_SLOT_BYTES + 1U];
    const size_t length = FAKE_VIDEO_SLOT_BYTES + 1U;
    osvc_put_envelope(big, OSVC_SERVICE_VIDEO, OSVC_VIDEO_FRAME,
                      (uint32_t)(OSVC_VIDEO_FRAME_HEADER_LEN + length));
    osvc_put_u32(big + OSVC_ENVELOPE_LEN + 8U, KEY | SETS);
    feed_chunked(big, sizeof(big), 16384U);
    expect_video_fault("a keyframe above 3145728 bytes");
    assert(g_fake_video.reserves == 1U);
    send_message(OSVC_SERVICE_SESSION, OSVC_SESSION_PING, NULL, 0U);
    assert(osvc_session_open(&s_session));

    video_show();
    send_message(OSVC_SERVICE_VIDEO, OSVC_VIDEO_CAPABILITIES, withdraw,
                 sizeof(withdraw));
    expect_video_fault("video withdrawn without a disabled video.status");

    video_show();
    video_status(true, true, 3840U, 2400U, 30U, 0U);
    expect_video_fault("video.status exceeds the sender's video.capabilities");

    video_show();
    g_fake_video.stream_refused = true;
    video_status(true, true, 3840U, 2160U, 30U, 0U);
    expect_video_fault("the KVM cannot show 3840x2160 at 30 fps");

    video_show();
    osvc_session_display_status(&s_session, true, false);
    expect_video_fault("the KVM display could not be presented");

    /* The session ends: back to hardware capture. */
    video_show();
    osvc_session_end(&s_session, "test");
    assert(s_session.video_source == OSVC_SOURCE_SWITCHING_TO_PHYSICAL);
    assert(g_fake_video.restores == 1U && g_fake_video.stopped == 1U);
}

/* Settings while showing: a new size presents the display again, as a
 * monitor that the target has not seen, and video.config follows once it is
 * up; other changes only send video.config. */
static void test_video_settings_while_showing(void)
{
    video_show();
    osvc_settings_t settings = settings_1080p();
    settings.layout = OSVC_LAYOUT_MIRROR;
    settings.target_kbps = 20000U;
    osvc_session_apply_settings(&s_session, &settings);
    expect_video_config(true, 1920U, 1080U, OSVC_VIDEO_KBPS_MAX, 60U);
    assert(g_fake_video.presents == 1U);
    settings.width = 1280U;
    settings.height = 720U;
    settings.target_kbps = 0U;
    osvc_session_apply_settings(&s_session, &settings);
    assert(g_fake_video.presents == 2U && g_fake_video.present_width == 1280U);
    assert(g_fake_video.serials[1] != g_fake_video.serials[0]);
    /* The sender hears of it once the new display is up. */
    osvc_session_display_status(&s_session, false, true);
    expect_no_message();
    osvc_session_display_status(&s_session, true, true);
    expect_video_config(true, 1280U, 720U, OSVC_VIDEO_KBPS_720P, 60U);
    osvc_session_display_status(&s_session, true, true);
    expect_no_message();
    assert(s_session.video_source == OSVC_SOURCE_GUEST);
    settings.width = 3840U;
    settings.height = 2400U;
    osvc_session_apply_settings(&s_session, &settings);
    assert(strcmp(s_session.video_error, "the KVM display size is above the sender's "
                  "video.capabilities") == 0);
    assert(!s_session.video_registered);
}

/* A sender that says it sets the display's mode gets one monitor per KVM
 * and size: the start of the KVM's key and the height, at each
 * presentation. */
static void test_video_display_serial(void)
{
    s_video_flags |= OSVC_VIDEO_FLAG_SETS_DISPLAY_MODE;
    video_show();
    osvc_settings_t settings = settings_1080p();
    settings.width = 1280U;
    settings.height = 720U;
    osvc_session_apply_settings(&s_session, &settings);
    settings = settings_1080p();
    osvc_session_display_status(&s_session, true, true);
    osvc_session_apply_settings(&s_session, &settings);
    const uint32_t kvm = osvc_get_u32(s_fake.public_key) & ~0xFFFU;
    assert(g_fake_video.presents == 3U);
    assert(g_fake_video.serials[0] == (kvm | 1080U) &&
           g_fake_video.serials[1] == (kvm | 720U) &&
           g_fake_video.serials[2] == (kvm | 1080U));
    s_video_flags = OSVC_VIDEO_CODEC_FLAG_H264;
}

/* A delta frame that waits ms for a slot while the clock ticks (no ping
 * can come while the frame is read). */
static void send_frame_after_wait(int64_t ms)
{
    static uint8_t unit[4096];
    static uint8_t message[4096 + 32];
    static uint64_t capture_us = 2000000U;
    capture_us += 33333U;
    const size_t length = make_unit(unit, false, false, 2000U, 3U);
    const size_t total = encode_frame(message, capture_us, 0U, unit, length);
    const size_t head = OSVC_ENVELOPE_LEN + OSVC_VIDEO_FRAME_HEADER_LEN;
    g_fake_video.busy = true;
    assert(osvc_session_receive(&s_session, message, total) == head);
    for (int64_t waited = 0; waited < ms; waited += 100) {
        s_fake.now += 100;
        osvc_session_tick(&s_session);
    }
    g_fake_video.busy = false;
    feed(message + head, total - head);
}

/* Seconds in which a frame waits 600 ms for a slot and the KVM takes this
 * many frames. */
static void video_pressure(unsigned seconds, unsigned frames)
{
    for (unsigned i = 0U; i < seconds; ++i) {
        send_frame_after_wait(600);
        for (unsigned frame = 1U; frame < frames; ++frame) {
            send_frame_after_wait(0);
        }
        video_wait(400);
    }
}

/* The target follows the network's estimate: down once the stream's
 * first 2 s are over and by half at most every 2 s, up 10 s after a change
 * and by half at most. A bitrate-only change starts no new stream. */
static void test_video_rate_follows_the_network(void)
{
    video_show();
    g_fake_video.estimate_kbps = 3000U;
    video_wait(1000);
    expect_no_message();
    video_wait(1000);
    expect_video_config(true, 1920U, 1080U, 4000U, 60U);
    video_wait(1000);
    expect_no_message();
    video_wait(1000);
    expect_video_config(true, 1920U, 1080U, 3000U, 60U);
    video_status(true, false, 1920U, 1080U, 30U, 0U);
    assert(g_fake_video.streams == 1U);
    g_fake_video.estimate_kbps = 0U;
    video_wait(9000);
    expect_no_message();
    video_wait(1000);
    expect_video_config(true, 1920U, 1080U, 4500U, 60U);
    video_wait(10000);
    expect_video_config(true, 1920U, 1080U, 6700U, 60U);
    video_wait(10000);
    expect_video_config(true, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);
    video_wait(60000);
    expect_no_message();
    assert(g_fake_video.ceiling_kbps == OSVC_VIDEO_KBPS_1080P);
}

/* A feedback target that stays low after its cut, as after one burst of
 * loss while it recovers, cuts once. */
static void test_video_rate_stale_estimate(void)
{
    video_show();
    g_fake_video.estimate_kbps = 1000U;
    video_wait(2000);
    expect_video_config(true, 1920U, 1080U, 4000U, 60U);
    g_fake_video.stale = true;
    video_wait(20000);
    expect_no_message();
}

/* Frames that wait for a slot much of the time, three intervals in a row,
 * take the target under what the transport sent; one long wait, as for a
 * keyframe, spans two intervals and does not. */
static void test_video_rate_under_pressure(void)
{
    video_show();
    g_fake_video.sent_kbps = 6000U;
    video_wait(500);
    send_frame_after_wait(900);
    video_wait(2600);
    expect_no_message();
    video_pressure(2U, 30U);
    expect_no_message();
    video_pressure(1U, 30U);
    expect_video_config(true, 1920U, 1080U, 5100U, 60U);
}

/* Seconds in which the KVM takes this many frames and none waits. */
static void video_frames(unsigned seconds, unsigned frames)
{
    for (unsigned i = 0U; i < seconds; ++i) {
        for (unsigned frame = 0U; frame < frames; ++frame) {
            send_frame_after_wait(0);
        }
        video_wait(1000);
    }
}

/* A sender that sends more than its target is cut from the target: 5800
 * kbit/s sent for a target of 5100 give 4300, which 85 % of 5800 would not
 * do. */
static void test_video_rate_under_pressure_from_a_sender_over_its_target(void)
{
    video_show();
    g_fake_video.sent_kbps = 6000U;
    video_pressure(3U, 30U);
    expect_video_config(true, 1920U, 1080U, 5100U, 60U);
    g_fake_video.sent_kbps = 5800U;
    video_pressure(1U, 30U);
    expect_no_message();
    video_pressure(1U, 30U);
    expect_video_config(true, 1920U, 1080U, 4300U, 60U);
}

/* The sender delivered 30 frames per second while none waited. With
 * frames waiting, the KVM takes 20 of them: it carries two thirds of the
 * 7000 kbit/s it sent, and the cut is half at most. At the new target the
 * sender has not shown what it delivers, so 20 frames are not counted
 * against 30. */
static void test_video_rate_under_pressure_with_fewer_frames(void)
{
    video_show();
    g_fake_video.sent_kbps = 7000U;
    video_frames(3U, 30U);
    expect_no_message();
    video_pressure(3U, 20U);
    expect_video_config(true, 1920U, 1080U, 4000U, 60U);
    g_fake_video.sent_kbps = 4400U;
    video_pressure(2U, 20U);
    expect_video_config(true, 1920U, 1080U, 3400U, 60U);
}

/* A sender that delivers 20 frames per second by itself is not taken for
 * a stream that the KVM holds back. */
static void test_video_rate_under_pressure_at_the_senders_frame_rate(void)
{
    video_show();
    g_fake_video.sent_kbps = 7000U;
    video_frames(3U, 20U);
    expect_no_message();
    video_pressure(3U, 20U);
    expect_video_config(true, 1920U, 1080U, 5900U, 60U);
}

/* Under 1.5 Mbit/s at 1080p the frame rate halves; the stream the sender
 * restarts at 15 fps keeps the adapted target. */
static void test_video_rate_halves_the_frame_rate(void)
{
    video_show();
    g_fake_video.estimate_kbps = 1000U;
    video_wait(2000);
    expect_video_config(true, 1920U, 1080U, 4000U, 60U);
    video_wait(2000);
    expect_video_config(true, 1920U, 1080U, 2000U, 60U);
    video_wait(2000);
    expect_video_config(true, 1920U, 1080U, 1000U, 30U);
    uint8_t status[OSVC_VIDEO_STATUS_MIN_LEN] = {1U, 0U, 0U, 1U};
    osvc_put_u32(status + 4, 1920U);
    osvc_put_u32(status + 8, 1080U);
    osvc_put_u32(status + 12, 30U);
    osvc_put_u32(status + 20, 1000U);
    osvc_put_u32(status + 24, 15U);
    send_message(OSVC_SERVICE_VIDEO, OSVC_VIDEO_STATUS, status, sizeof(status));
    assert(g_fake_video.streams == 2U && g_fake_video.stream_fps == 15U);
    assert(s_session.video_sender_kbps == 1000U);
    g_fake_video.estimate_kbps = 0U;
    video_wait(10000);
    expect_video_config(true, 1920U, 1080U, 1500U, 30U);
    video_wait(10000);
    expect_video_config(true, 1920U, 1080U, 2200U, 30U);
    video_wait(10000);
    expect_video_config(true, 1920U, 1080U, 3300U, 60U);
}

/* A manual video quality puts the static target back at once and does not
 * follow the receiver feedback. Frames that wait for a slot still lower
 * the target, and it returns to the static target in steps. A fixed
 * bitrate in the settings is never adapted. */
static void test_video_rate_manual_and_fixed(void)
{
    video_show();
    g_fake_video.estimate_kbps = 3000U;
    video_wait(2000);
    expect_video_config(true, 1920U, 1080U, 4000U, 60U);
    g_fake_video.manual = true;
    video_wait(100);
    expect_video_config(true, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);
    video_wait(30000);
    expect_no_message();
    g_fake_video.sent_kbps = 5000U;
    video_pressure(3U, 26U);
    expect_video_config(true, 1920U, 1080U, 4200U, 60U);
    video_wait(9000);
    expect_no_message();
    video_wait(1000);
    expect_video_config(true, 1920U, 1080U, 6300U, 60U);
    video_wait(10000);
    expect_video_config(true, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 60U);

    osvc_settings_t settings = settings_1080p();
    settings.target_kbps = 6000U;
    video_open(&settings);
    video_register(3840U, 2160U, 60U);
    osvc_session_display_status(&s_session, true, true);
    expect_video_config(true, 1920U, 1080U, 6000U, 60U);
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    g_fake_video.estimate_kbps = 2000U;
    video_wait(30000);
    expect_no_message();
    (void)send_frame(3000000U, KEY | SETS, true, true, 100U);
    g_fake_video.sent_kbps = 6600U;
    video_pressure(4U, 26U);
    expect_no_message();
}

/* The first 2 s of a stream are its own, however long the switch took;
 * new settings start again from the static targets, also at an equal
 * ceiling; a 1 fps cap is not halved to none. */
static void test_video_rate_restarts(void)
{
    const osvc_settings_t settings = default_settings();
    video_open(&settings);
    video_register(3840U, 2160U, 60U);
    osvc_session_display_status(&s_session, true, true);
    expect_video_config(true, 2560U, 1440U, OSVC_VIDEO_KBPS_1440P, 60U);
    video_wait(3000);
    video_status(true, true, 2560U, 1440U, 30U, 0U);
    g_fake_video.estimate_kbps = 1500U;
    video_wait(1000);
    expect_no_message();
    video_wait(1000);
    expect_video_config(true, 2560U, 1440U, 6000U, 60U);
    video_wait(2000);
    expect_video_config(true, 2560U, 1440U, 3000U, 60U);
    video_wait(2000);
    expect_video_config(true, 2560U, 1440U, 1500U, 30U);

    osvc_settings_t larger = settings;
    larger.width = 3840U;
    larger.height = 2160U;
    osvc_session_apply_settings(&s_session, &larger);
    osvc_session_display_status(&s_session, true, true);
    expect_video_config(true, 3840U, 2160U, OSVC_VIDEO_KBPS_1440P, 60U);

    osvc_settings_t one_fps = settings_1080p();
    one_fps.maximum_fps = 1U;
    video_open(&one_fps);
    video_register(3840U, 2160U, 60U);
    osvc_session_display_status(&s_session, true, true);
    expect_video_config(true, 1920U, 1080U, OSVC_VIDEO_KBPS_1080P, 1U);
    video_status(true, true, 1920U, 1080U, 30U, 0U);
    g_fake_video.estimate_kbps = 1000U;
    video_wait(2000);
    expect_video_config(true, 1920U, 1080U, 4000U, 1U);
    video_wait(2000);
    expect_video_config(true, 1920U, 1080U, 2000U, 1U);
    video_wait(2000);
    expect_video_config(true, 1920U, 1080U, 1000U, 1U);
}

int main(void)
{
    test_wire_helpers();
    test_reset_handshake_and_signature();
    test_hello_search_across_chunks();
    test_hello_search_gives_up_after_8_mib();
    test_wrong_major_ends_session();
    test_newer_minor_selects_ours();
    test_timers();
    test_session_errors();
    test_unknown_messages_keep_alignment();
    test_session_info_reports_changes();
    test_approval_pending();
    test_clipboard();
    test_clipboard_faults_and_settings();
    test_video_registration_is_answered();
    test_other_registrations();
    test_status_before_reset_has_a_token();
    test_reconfiguration_ends_session();
    test_reset_with_token_and_end();
    test_terminal_offer_follows_registration_and_settings();
    test_terminal_round_trip();
    test_terminal_close_and_exit_reasons();
    test_terminal_open_timeout_and_limit();
    test_terminal_data_limits();
    test_terminal_invalid_messages_fault_the_service();
    test_terminal_output_waits_for_the_browser();
    test_requests_wait_while_output_is_held();
    test_terminal_ends_with_browser_settings_and_session();
    test_file_offer_follows_registration_and_settings();
    test_file_drop_limits();
    test_file_transfer_and_credit();
    test_file_chunk_boundaries();
    test_file_credit_boundary_and_overrun();
    test_file_zero_byte_files_and_held_outcomes();
    test_file_cancels();
    test_file_idle_timeout();
    test_file_attach_timeout_frees_next_drop();
    test_file_invalid_messages_fault_the_service();
    test_file_ends_with_settings_withdrawal_and_session();
    test_file_throughput_under_the_credit_window();
    test_platform_without_new_hooks();
    test_update_credit_and_installing();
    test_update_ends();
    test_update_invalid_messages_fault_the_service();
    test_update_relay_holds_back_the_last_chunk();
    test_video_switch_waits_for_the_display();
    test_video_frames_across_usb_reads();
    test_video_stream_reset();
    test_video_stream_reset_without_viewer();
    test_video_parameter_sets_prepended();
    test_video_parameter_sets_from_a_discarded_keyframe();
    test_video_parameter_sets_cut_short_not_kept();
    test_video_frame_limits_follow_the_stream();
    test_video_pts();
    test_video_sender_skipped_frames();
    test_video_keyframe_request_rate();
    test_video_backpressure_and_gap();
    test_video_timers();
    test_video_registration_while_display_held();
    test_video_retry_budget();
    test_video_retry_budget_shown();
    test_video_retry_budget_after_update();
    test_video_faults();
    test_video_settings_while_showing();
    test_video_display_serial();
    test_video_rate_follows_the_network();
    test_video_rate_under_pressure();
    test_video_rate_under_pressure_from_a_sender_over_its_target();
    test_video_rate_under_pressure_with_fewer_frames();
    test_video_rate_under_pressure_at_the_senders_frame_rate();
    test_video_rate_stale_estimate();
    test_video_rate_halves_the_frame_rate();
    test_video_rate_manual_and_fixed();
    test_video_rate_restarts();
    printf("os_services session tests passed\n");
    return 0;
}

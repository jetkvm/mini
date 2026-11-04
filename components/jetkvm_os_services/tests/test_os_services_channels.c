/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The browser's terminal and file channels, between a model of the browser
 * (useGuestFileDrop.ts and the terminal's channel) and the real session
 * talking to a sender written here from the OS Services protocol. The
 * table's lock is checked: never taken twice, and never held while the
 * session writes to the IN endpoint or calls a hook.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "fake_platform.h"
#include "os_services_channels.h"
#include "os_services_session.h"

static uint8_t s_payload_buffer[OSVC_MAX_CONTROL_PAYLOAD];
static fake_platform_t s_fake;
static osvc_platform_t s_platform;
static osvc_session_t s_session;
static osvc_channels_t s_channels;

/* --- The table's platform ---------------------------------------------- */

static int s_lock_depth;
static size_t s_allocs;
static size_t s_frees;
static size_t s_alloc_bytes;
static bool s_fail_alloc;
static unsigned s_service_pending;
static unsigned s_output_pending;

static void table_lock(void *ctx)
{
    (void)ctx;
    assert(s_lock_depth == 0);
    s_lock_depth++;
}

static void table_unlock(void *ctx)
{
    (void)ctx;
    assert(s_lock_depth == 1);
    s_lock_depth--;
}

static void *table_alloc(void *ctx, size_t size)
{
    (void)ctx;
    if (s_fail_alloc) {
        return NULL;
    }
    void *memory = malloc(size);
    assert(memory != NULL);
    /* Garbage, as the heap would give it. */
    memset(memory, 0xa5, size);
    s_allocs++;
    s_alloc_bytes += size;
    return memory;
}

static void table_free(void *ctx, void *memory)
{
    (void)ctx;
    assert(memory != NULL);
    s_frees++;
    free(memory);
}

static int64_t table_now(void *ctx)
{
    (void)ctx;
    return s_fake.now;
}

static void table_service_pending(void *ctx)
{
    (void)ctx;
    assert(s_lock_depth == 0);
    s_service_pending++;
}

static void table_output_pending(void *ctx)
{
    (void)ctx;
    assert(s_lock_depth == 0);
    s_output_pending++;
}

/* The session's hooks go to the table, as the glue routes them. */
static bool (*s_fake_write)(void *, const uint8_t *, const uint8_t *, size_t,
                            const uint8_t *, size_t);

static unsigned pump(unsigned limit);
/* The transport's sending phase runs while the service task writes. */
static bool s_pump_in_write;

static bool checked_write(void *ctx, const uint8_t envelope[OSVC_ENVELOPE_LEN],
                          const uint8_t *head, size_t head_length,
                          const uint8_t *body, size_t body_length)
{
    /* A write can block for 2 seconds: never under the table's lock. */
    assert(s_lock_depth == 0);
    if (s_pump_in_write) {
        (void)pump(0U);
    }
    return s_fake_write(ctx, envelope, head, head_length, body, body_length);
}

static bool hook_terminal_output(void *ctx, uint32_t terminal_id,
                                 const uint8_t *data, size_t length)
{
    (void)ctx;
    return osvc_channels_terminal_output(&s_channels, terminal_id, data,
                                         length);
}

static void hook_terminal_closed(void *ctx, uint32_t terminal_id,
                                 const char *reason, bool exited)
{
    (void)ctx;
    (void)exited;
    osvc_channels_terminal_closed(&s_channels, terminal_id, reason);
}

static void hook_file_credit(void *ctx, uint32_t drop_id, uint32_t transfer_id,
                             uint64_t bytes)
{
    (void)ctx;
    osvc_channels_file_credit(&s_channels, drop_id, transfer_id, bytes);
}

static void hook_file_done(void *ctx, uint32_t drop_id, uint32_t transfer_id,
                           osvc_file_status_t status, const char *path,
                           size_t path_length, bool last)
{
    (void)ctx;
    osvc_channels_file_done(&s_channels, drop_id, transfer_id, status, path,
                            path_length, last);
}

/* --- The sender ------------------------------------------------------------ */

static void feed(const uint8_t *data, size_t length)
{
    assert(osvc_session_receive(&s_session, data, length) == length);
}

static void sender_send(uint16_t service, uint16_t kind, const void *payload,
                        size_t length)
{
    static uint8_t buffer[OSVC_ENVELOPE_LEN + OSVC_MAX_CONTROL_PAYLOAD];
    osvc_put_envelope(buffer, service, kind, (uint32_t)length);
    if (length > 0U) {
        memcpy(buffer + OSVC_ENVELOPE_LEN, payload, length);
    }
    feed(buffer, OSVC_ENVELOPE_LEN + length);
}

static fake_message_t expect_message(uint16_t service, uint16_t kind)
{
    fake_message_t message;
    assert(fake_next_message(&s_fake, &message));
    if (message.service != service || message.kind != kind) {
        fprintf(stderr, "sender got %u/%u, expected %u/%u\n", message.service,
                message.kind, service, kind);
        abort();
    }
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
    sender_send(service, 1U, caps, sizeof(caps));
}

static void sender_terminal_status(uint16_t kind, uint32_t terminal_id,
                                   uint8_t status)
{
    uint8_t payload[5];
    osvc_put_u32(payload, terminal_id);
    payload[4] = status;
    sender_send(OSVC_SERVICE_TERMINAL, kind, payload, sizeof(payload));
}

static void sender_output(uint32_t terminal_id, const uint8_t *data,
                          size_t length)
{
    static uint8_t payload[OSVC_MAX_CONTROL_PAYLOAD];
    assert(length <= OSVC_TERMINAL_MAX_DATA);
    osvc_put_u32(payload, terminal_id);
    memcpy(payload + 4, data, length);
    sender_send(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_OUTPUT, payload,
                4U + length);
}

static void sender_credit(uint16_t kind, uint32_t transfer_id, uint32_t bytes)
{
    uint8_t payload[8];
    osvc_put_u32(payload, transfer_id);
    osvc_put_u32(payload + 4, bytes);
    sender_send(OSVC_SERVICE_FILE, kind, payload, sizeof(payload));
}

static void sender_done(uint32_t transfer_id, uint8_t status, const char *path,
                        size_t path_length)
{
    static uint8_t payload[OSVC_MAX_CONTROL_PAYLOAD];
    memset(payload, 0, 8U);
    osvc_put_u32(payload, transfer_id);
    payload[4] = status;
    memcpy(payload + 8, path, path_length);
    sender_send(OSVC_SERVICE_FILE, OSVC_FILE_DONE, payload, 8U + path_length);
}

/* An open session with the shell and file reception registered, its hooks
 * routed to a fresh table, and nothing written after session.config. */
static void setup(bool terminal, bool files)
{
    fake_platform_init(&s_fake, &s_platform);
    s_fake_write = s_platform.write;
    s_platform.write = checked_write;
    s_platform.terminal_output = hook_terminal_output;
    s_platform.terminal_closed = hook_terminal_closed;
    s_platform.file_credit = hook_file_credit;
    s_platform.file_done = hook_file_done;
    const osvc_settings_t settings = {
        .terminal_enabled = terminal,
        .files_enabled = files,
        .width = 1920U,
        .height = 1080U,
    };
    osvc_session_init(&s_session, &s_platform, &settings, s_payload_buffer,
                      sizeof(s_payload_buffer));
    s_lock_depth = 0;
    s_allocs = 0U;
    s_frees = 0U;
    s_alloc_bytes = 0U;
    s_fail_alloc = false;
    s_service_pending = 0U;
    s_output_pending = 0U;
    const osvc_channels_platform_t table = {
        .lock = table_lock,
        .unlock = table_unlock,
        .alloc = table_alloc,
        .free = table_free,
        .now_ms = table_now,
        .service_pending = table_service_pending,
        .output_pending = table_output_pending,
    };
    osvc_channels_init(&s_channels, &table);

    uint8_t status[OSVC_USB_STATUS_LEN];
    osvc_session_usb_reset(&s_session, status);
    osvc_session_transport_drained(&s_session);
    uint8_t hello[OSVC_HELLO_LEN] = {0};
    osvc_put_u16(hello, 5U);
    memcpy(hello + 4, status + 8, OSVC_TOKEN_LEN);
    sender_send(OSVC_SERVICE_SESSION, OSVC_SESSION_HELLO, hello, sizeof(hello));
    (void)expect_message(OSVC_SERVICE_SESSION, OSVC_SESSION_READY);
    (void)expect_message(OSVC_SERVICE_SESSION, OSVC_SESSION_CONFIG);
    register_service(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_FLAG_SHELL);
    register_service(OSVC_SERVICE_FILE, OSVC_FILE_FLAG_RECEIVE);
    expect_no_message();
    fake_clear_output(&s_fake);
}

/* Every channel gone, and its memory with it. */
static void expect_table_empty(void)
{
    for (size_t i = 0U; i < OSVC_CHANNELS_MAX; ++i) {
        assert(s_channels.channels[i].kind == OSVC_CHANNEL_FREE);
    }
    assert(s_allocs == s_frees);
}

/* --- The browser ------------------------------------------------------------ */

#define BROWSER_BINARY_MAX (8U * 1024U * 1024U)
#define BROWSER_TEXTS_MAX 1100U

typedef struct {
    uint32_t channel;
    uint8_t *binary;
    size_t binary_length;
    char *texts[BROWSER_TEXTS_MAX];
    size_t text_count;
    size_t text_read;
    unsigned messages;
    /* The KVM closed the channel. */
    bool closed;
} browser_channel_t;

#define BROWSER_CHANNELS 16U

static browser_channel_t s_browser[BROWSER_CHANNELS];

static browser_channel_t *browser(uint32_t channel)
{
    for (size_t i = 0U; i < BROWSER_CHANNELS; ++i) {
        if (s_browser[i].channel == channel) {
            return &s_browser[i];
        }
    }
    for (size_t i = 0U; i < BROWSER_CHANNELS; ++i) {
        if (s_browser[i].channel == 0U) {
            s_browser[i].channel = channel;
            if (s_browser[i].binary == NULL) {
                s_browser[i].binary = malloc(BROWSER_BINARY_MAX);
                assert(s_browser[i].binary != NULL);
            }
            return &s_browser[i];
        }
    }
    abort();
}

static void browser_reset(void)
{
    for (size_t i = 0U; i < BROWSER_CHANNELS; ++i) {
        for (size_t k = 0U; k < s_browser[i].text_count; ++k) {
            free(s_browser[i].texts[k]);
        }
        uint8_t *binary = s_browser[i].binary;
        memset(&s_browser[i], 0, sizeof(s_browser[i]));
        s_browser[i].binary = binary;
    }
}

/* The transport's sending phase: takes at most limit messages (0: all). A
 * close goes out, then the vendor reports it, as esp_peer does inside its
 * close call. */
static unsigned pump(unsigned limit)
{
    unsigned sent = 0U;
    osvc_channel_output_t out;
    while ((limit == 0U || sent < limit) &&
           osvc_channels_next_output(&s_channels, &out)) {
        assert(s_lock_depth == 0);
        browser_channel_t *b = browser(out.channel);
        assert(!b->closed);
        if (out.close) {
            assert(out.length == 0U);
            osvc_channels_output_done(&s_channels, out.channel, true);
            b->closed = true;
            osvc_channels_closed(&s_channels, out.channel);
        } else {
            assert(out.length > 0U && out.length <= OSVC_CHANNEL_MESSAGE_MAX);
            if (out.text) {
                assert(b->text_count < BROWSER_TEXTS_MAX);
                char *text = malloc(out.length + 1U);
                assert(text != NULL);
                memcpy(text, out.data, out.length);
                text[out.length] = '\0';
                assert(strlen(text) == out.length);
                b->texts[b->text_count++] = text;
            } else {
                assert(b->binary_length + out.length <= BROWSER_BINARY_MAX);
                memcpy(b->binary + b->binary_length, out.data, out.length);
                b->binary_length += out.length;
            }
            b->messages++;
            osvc_channels_output_done(&s_channels, out.channel, true);
        }
        sent++;
    }
    return sent;
}

/* The service task's turn: until it has nothing left. */
static unsigned service(void)
{
    unsigned calls = 1U;
    while (osvc_channels_service(&s_channels, &s_session)) {
        assert(++calls < 1000U);
    }
    assert(s_lock_depth == 0);
    return calls;
}

static const char *next_text(uint32_t channel)
{
    browser_channel_t *b = browser(channel);
    if (b->text_read == b->text_count) {
        return NULL;
    }
    return b->texts[b->text_read++];
}

static void expect_text(uint32_t channel, const char *expected)
{
    const char *text = next_text(channel);
    if (text == NULL || strcmp(text, expected) != 0) {
        fprintf(stderr, "channel %u got %s, expected %s\n", channel,
                text ? text : "(nothing)", expected);
        abort();
    }
}

static void browser_text(uint32_t channel, const char *text)
{
    osvc_channels_message(&s_channels, channel, true, (const uint8_t *)text,
                          strlen(text));
}

static void browser_binary(uint32_t channel, const void *data, size_t length)
{
    osvc_channels_message(&s_channels, channel, false, data, length);
}

static uint8_t pattern(uint64_t i)
{
    return (uint8_t)(i * 131U + (i >> 9) + (i >> 17));
}

/* --- Parsers ------------------------------------------------------------------ */

static bool size_of(const char *text, uint32_t *rows, uint32_t *cols)
{
    return osvc_channels_parse_size((const uint8_t *)text, strlen(text), rows,
                                    cols);
}

static bool cancel_of(const char *text, uint32_t *transfer)
{
    return osvc_channels_parse_cancel((const uint8_t *)text, strlen(text),
                                      transfer);
}

static void test_parsers(void)
{
    uint32_t rows = 0U;
    uint32_t cols = 0U;
    assert(size_of("{\"rows\":24,\"cols\":80}", &rows, &cols));
    assert(rows == 24U && cols == 80U);
    assert(size_of(" {\n\"cols\" : 132 ,\t\"rows\":43 } ", &rows, &cols));
    assert(rows == 43U && cols == 132U);
    /* Other members of any type. */
    assert(size_of("{\"type\":\"resize\",\"x\":1.5e3,\"y\":-2,\"z\":null,"
                   "\"t\":true,\"f\":false,\"o\":{\"a\":[1]},\"rows\":2,"
                   "\"cols\":65535,"
                   "\"k\\u0041\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"}",
                   &rows, &cols));
    assert(rows == 2U && cols == 65535U);
    /* Escapes are JSON: an escaped name is the same member. */
    assert(size_of("{\"\\u0072ows\":24,\"c\\u006fls\":80}", &rows, &cols));
    assert(rows == 24U && cols == 80U);
    static const char *const bad_sizes[] = {
        "",
        "{}",
        "{\"rows\":24}",
        "{\"rows\":0,\"cols\":80}",
        "{\"rows\":24,\"cols\":65536}",
        "{\"rows\":24,\"cols\":80.5}",
        "{\"rows\":24,\"cols\":8.05e1}",
        "{\"rows\":-24,\"cols\":80}",
        "{\"rows\":\"24\",\"cols\":80}",
        "{\"rows\":24,\"cols\":true}",
        "{\"rows\":24,\"cols\":80}x",
        "{\"rows\":24,\"cols\":80,}",
        "{\"rows\":24 \"cols\":80}",
        "{\"rows\":24,\"cols\":99999999999999999999999}",
        "{\"rows\":24,\"cols\":80,\"s\":\"\\x\"}",
        "{\"rows\":24,\"cols\":80,\"s\":\"\\u12\"}",
        "{\"rows\":24,\"cols\":80,\"s\":\"open",
        "[24,80]",
        "ls -la\r",
    };
    for (size_t i = 0U; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); ++i) {
        if (size_of(bad_sizes[i], &rows, &cols)) {
            fprintf(stderr, "size accepted: %s\n", bad_sizes[i]);
            abort();
        }
    }

    uint32_t transfer = 0U;
    assert(cancel_of("{\"type\":\"cancel\",\"transfer\":7}", &transfer));
    assert(transfer == 7U);
    assert(cancel_of("{\"transfer\":4294967295,\"type\":\"cancel\",\"x\":[]}",
                     &transfer));
    assert(transfer == UINT32_MAX);
    /* An escaped value or name is the same one. */
    assert(cancel_of("{\"type\":\"c\\u0061ncel\",\"transfer\":42}", &transfer));
    assert(transfer == 42U);
    assert(cancel_of("{\"\\u0074ype\":\"cancel\",\"tr\\u0061nsfer\":43}",
                     &transfer));
    assert(transfer == 43U);
    static const char *const bad_cancels[] = {
        "{\"type\":\"cancel\"}",
        "{\"transfer\":7}",
        "{\"type\":\"credit\",\"transfer\":7}",
        "{\"type\":\"cancelled\",\"transfer\":7}",
        "{\"type\":\"cance\",\"transfer\":7}",
        "{\"type\":\"cancel\",\"transfer\":4294967296}",
        "{\"type\":\"cancel\",\"transfer\":-7}",
        "{\"type\":\"cancel\",\"transfer\":\"7\"}",
        "{\"type\":7,\"transfer\":7}",
        "{\"type\":\"cancel\",\"transfer\":7",
    };
    for (size_t i = 0U; i < sizeof(bad_cancels) / sizeof(bad_cancels[0]);
         ++i) {
        if (cancel_of(bad_cancels[i], &transfer)) {
            fprintf(stderr, "cancel accepted: %s\n", bad_cancels[i]);
            abort();
        }
    }
}

/* --- Claims ------------------------------------------------------------------ */

static void test_claims(void)
{
    setup(true, true);
    static const char *const refused[] = {
        "rpc", "hidrpc", "guest-terminal ", "guest-terminal2", "guestfile_",
        "guestfile_0", "guestfile_012", "guestfile_12a", "guestfile_-1",
        "guestfile_4294967296", "guestfile_99999999999", "Guestfile_1", "",
    };
    for (size_t i = 0U; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        assert(!osvc_channels_claim(&s_channels, 100U + (uint32_t)i,
                                    refused[i]));
    }
    assert(!osvc_channels_claim(&s_channels, 0U, OSVC_TERMINAL_CHANNEL_LABEL));
    assert(!osvc_channels_claim(&s_channels, 1U, NULL));
    assert(s_allocs == 0U);

    /* The table holds four terminals and two drop channels. */
    for (uint32_t i = 1U; i <= OSVC_TERMINAL_SESSIONS_MAX; ++i) {
        assert(osvc_channels_claim(&s_channels, i, "guest-terminal"));
    }
    assert(osvc_channels_claim(&s_channels, 11U, "guestfile_4294967295"));
    assert(!osvc_channels_claim(&s_channels, 11U, "guestfile_7"));
    assert(osvc_channels_claim(&s_channels, 12U, "guestfile_1"));
    assert(!osvc_channels_claim(&s_channels, 13U, "guest-terminal"));
    assert(s_channels.counters.refused == 2U);
    assert(s_allocs == s_frees + 6U);
    assert(s_service_pending == 6U);
    /* 64 + 128 KiB a terminal, 64 KiB a drop, each asked for before the
     * table is looked at: five terminals and three drops. */
    assert(s_alloc_bytes ==
           5U * (sizeof(osvc_terminal_channel_t) + 192U * 1024U) +
               3U * (sizeof(osvc_file_channel_t) + 64U * 1024U));

    /* The browser closes them all before the service task saw any. */
    for (uint32_t i = 1U; i <= OSVC_TERMINAL_SESSIONS_MAX; ++i) {
        osvc_channels_closed(&s_channels, i);
    }
    osvc_channels_closed(&s_channels, 11U);
    osvc_channels_closed(&s_channels, 12U);
    osvc_channels_closed(&s_channels, 12U);
    osvc_channels_closed(&s_channels, 99U);
    service();
    expect_no_message();
    assert(pump(0U) == 0U);
    expect_table_empty();

    /* No memory: refused and counted. */
    s_fail_alloc = true;
    assert(!osvc_channels_claim(&s_channels, 20U, "guest-terminal"));
    assert(s_channels.counters.no_memory == 1U);
    s_fail_alloc = false;
    expect_table_empty();

    /* Messages for a channel nobody claimed are dropped. */
    browser_binary(55U, "ls\r", 3U);
    browser_text(55U, "{\"rows\":1,\"cols\":1}");
    service();
    expect_no_message();
    browser_reset();
}

/* --- Terminals --------------------------------------------------------------- */

static uint32_t expect_terminal_open(uint16_t rows, uint16_t cols)
{
    fake_message_t open = expect_message(OSVC_SERVICE_TERMINAL,
                                         OSVC_TERMINAL_OPEN);
    assert(open.length == 8U);
    const uint32_t terminal_id = osvc_get_u32(open.payload);
    assert(terminal_id != 0U);
    assert(osvc_get_u16(open.payload + 4) == 0U &&
           osvc_get_u16(open.payload + 6) == 0U);
    if (rows != 0U) {
        fake_message_t resize = expect_message(OSVC_SERVICE_TERMINAL,
                                               OSVC_TERMINAL_RESIZE);
        assert(osvc_get_u32(resize.payload) == terminal_id);
        assert(osvc_get_u16(resize.payload + 4) == rows &&
               osvc_get_u16(resize.payload + 6) == cols);
    }
    return terminal_id;
}

/* terminal.credit the sender received, by terminal ID (IDs start at 1). */
static uint64_t s_terminal_credit[8];

/* The sender's view of terminal.input: the bytes of every message, in
 * order, for one terminal. terminal.credit adds to s_terminal_credit. */
static size_t read_terminal_input(uint32_t terminal_id, uint8_t *out,
                                  size_t capacity)
{
    size_t length = 0U;
    fake_message_t message;
    while (fake_next_message(&s_fake, &message)) {
        assert(message.service == OSVC_SERVICE_TERMINAL);
        if (message.kind == OSVC_TERMINAL_CREDIT) {
            assert(message.length == OSVC_TERMINAL_CREDIT_LEN);
            const uint32_t id = osvc_get_u32(message.payload);
            assert(id < 8U && osvc_get_u32(message.payload + 4) >=
                                  OSVC_TERMINAL_CREDIT_STEP);
            s_terminal_credit[id] += osvc_get_u32(message.payload + 4);
            continue;
        }
        assert(message.kind == OSVC_TERMINAL_INPUT);
        assert(osvc_get_u32(message.payload) == terminal_id);
        assert(message.length > 4U &&
               length + message.length - 4U <= capacity);
        memcpy(out + length, message.payload + 4, message.length - 4U);
        length += message.length - 4U;
    }
    fake_clear_output(&s_fake);
    return length;
}

static void test_terminal_round_trip(void)
{
    setup(true, false);
    /* The browser sends its size as soon as the channel opens, before the
     * service task has run: it goes with the open. */
    assert(osvc_channels_claim(&s_channels, 1U, "guest-terminal"));
    browser_text(1U, "{\"rows\":24,\"cols\":80}");
    browser_binary(1U, "echo hi\r", 8U);
    service();
    const uint32_t terminal_id = expect_terminal_open(24U, 80U);
    fake_message_t input = expect_message(OSVC_SERVICE_TERMINAL,
                                          OSVC_TERMINAL_INPUT);
    assert(input.length == 12U && osvc_get_u32(input.payload) == terminal_id &&
           memcmp(input.payload + 4, "echo hi\r", 8U) == 0);
    expect_no_message();
    sender_terminal_status(OSVC_TERMINAL_OPENED, terminal_id,
                           OSVC_TERMINAL_OPENED_OK);

    /* Output goes to the browser as binary messages. */
    sender_output(terminal_id, (const uint8_t *)"hi\r\n$ ", 6U);
    assert(s_output_pending > 0U);
    assert(pump(0U) == 1U);
    browser_channel_t *b = browser(1U);
    assert(b->binary_length == 6U && memcmp(b->binary, "hi\r\n$ ", 6U) == 0);

    /* A resize (escapes are JSON); text that is not a size is ignored, as
     * are keystrokes that look like one: they come as binary. */
    browser_text(1U, "{\"\\u0072ows\":50,\"cols\":200}");
    browser_text(1U, "hello");
    browser_text(1U, "{\"rows\":0,\"cols\":200}");
    browser_binary(1U, "{\"rows\":1,\"cols\":1}", 19U);
    service();
    fake_message_t resize = expect_message(OSVC_SERVICE_TERMINAL,
                                           OSVC_TERMINAL_RESIZE);
    assert(osvc_get_u16(resize.payload + 4) == 50U &&
           osvc_get_u16(resize.payload + 6) == 200U);
    input = expect_message(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_INPUT);
    assert(input.length == 4U + 19U);
    expect_no_message();

    /* The browser closes: terminal.close, no line, the slot freed. */
    osvc_channels_closed(&s_channels, 1U);
    service();
    fake_message_t close = expect_message(OSVC_SERVICE_TERMINAL,
                                          OSVC_TERMINAL_CLOSE);
    assert(osvc_get_u32(close.payload) == terminal_id);
    expect_no_message();
    assert(pump(0U) == 0U);
    assert(b->text_count == 0U && !b->closed);
    /* Output in transit for the closed terminal goes nowhere. */
    sender_output(terminal_id, (const uint8_t *)"late", 4U);
    assert(pump(0U) == 0U);
    expect_table_empty();
    browser_reset();
}

static void test_terminal_ends(void)
{
    setup(true, false);
    /* The shell exits: the line, then the close. */
    assert(osvc_channels_claim(&s_channels, 1U, "guest-terminal"));
    service();
    uint32_t terminal_id = expect_terminal_open(0U, 0U);
    sender_terminal_status(OSVC_TERMINAL_OPENED, terminal_id,
                           OSVC_TERMINAL_OPENED_OK);
    sender_output(terminal_id, (const uint8_t *)"bye\r\n", 5U);
    sender_terminal_status(OSVC_TERMINAL_EXITED, terminal_id,
                           OSVC_TERMINAL_EXIT_EXITED);
    /* Keystrokes after the end are dropped. */
    browser_binary(1U, "x", 1U);
    service();
    expect_no_message();
    assert(pump(0U) == 3U);
    browser_channel_t *b = browser(1U);
    assert(b->binary_length == 5U && memcmp(b->binary, "bye\r\n", 5U) == 0);
    expect_text(1U, "\r\n[The shell exited]\r\n");
    assert(b->closed);
    service();
    expect_table_empty();

    /* Closed by the sender without a reason: no line. */
    assert(osvc_channels_claim(&s_channels, 2U, "guest-terminal"));
    service();
    terminal_id = expect_terminal_open(0U, 0U);
    sender_terminal_status(OSVC_TERMINAL_EXITED, terminal_id,
                           OSVC_TERMINAL_EXIT_CLOSED);
    assert(pump(0U) == 1U);
    assert(browser(2U)->closed && browser(2U)->text_count == 0U);
    service();
    expect_table_empty();

    /* The sender refuses. */
    assert(osvc_channels_claim(&s_channels, 3U, "guest-terminal"));
    service();
    terminal_id = expect_terminal_open(0U, 0U);
    sender_terminal_status(OSVC_TERMINAL_OPENED, terminal_id,
                           OSVC_TERMINAL_OPENED_UNSUPPORTED);
    pump(0U);
    expect_text(3U, "\r\n[Terminal unavailable: the target computer allows no "
                    "remote terminal]\r\n");
    assert(browser(3U)->closed);
    service();
    expect_table_empty();

    /* A fifth terminal: the session has four. */
    for (uint32_t i = 10U; i < 14U; ++i) {
        assert(osvc_channels_claim(&s_channels, i, "guest-terminal"));
    }
    service();
    for (uint32_t i = 10U; i < 14U; ++i) {
        (void)expect_terminal_open(0U, 0U);
    }
    osvc_channels_closed(&s_channels, 13U);
    service();
    (void)expect_message(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_CLOSE);
    assert(osvc_channels_claim(&s_channels, 14U, "guest-terminal"));
    assert(osvc_channels_claim(&s_channels, 15U, "guest-terminal"));
    service();
    (void)expect_terminal_open(0U, 0U);
    expect_no_message();
    pump(0U);
    expect_text(15U, "\r\n[Terminal unavailable: the target computer already runs "
                     "its maximum number of terminals]\r\n");
    assert(browser(15U)->closed && !browser(14U)->closed);
    service();

    /* No shell registered: refused before anything is sent. */
    register_service(OSVC_SERVICE_TERMINAL, 0U);
    pump(0U);
    for (uint32_t i = 10U; i <= 14U; ++i) {
        if (i == 13U) {
            continue;
        }
        expect_text(i, "\r\n[Terminal closed: the target computer withdrew its "
                       "shell]\r\n");
        assert(browser(i)->closed);
    }
    service();
    expect_table_empty();
    assert(osvc_channels_claim(&s_channels, 16U, "guest-terminal"));
    service();
    expect_no_message();
    pump(0U);
    expect_text(16U, "\r\n[no OS Services terminal is connected]\r\n");
    assert(browser(16U)->closed);
    service();
    expect_table_empty();
    browser_reset();
}

/* Shell output beyond the ring waits in the session, which reads no
 * further, until the browser takes enough; taking nothing for
 * OSVC_TERMINAL_OUTPUT_HOLD_MS ends the terminal, which still delivers what
 * the ring holds, then the line. */
static void test_terminal_slow_browser(void)
{
    setup(true, false);
    assert(osvc_channels_claim(&s_channels, 1U, "guest-terminal"));
    service();
    const uint32_t terminal_id = expect_terminal_open(0U, 0U);
    sender_terminal_status(OSVC_TERMINAL_OPENED, terminal_id,
                           OSVC_TERMINAL_OPENED_OK);
    fake_clear_output(&s_fake);
    static uint8_t chunk[60000];
    uint64_t total = 0U;
    for (int i = 0; i < 2; ++i) {
        for (size_t k = 0U; k < sizeof(chunk); ++k) {
            chunk[k] = pattern(total + k);
        }
        sender_output(terminal_id, chunk, sizeof(chunk));
        total += sizeof(chunk);
    }
    /* 120000 bytes fit 128 KiB; the third piece waits. */
    for (size_t k = 0U; k < sizeof(chunk); ++k) {
        chunk[k] = pattern(total + k);
    }
    sender_output(terminal_id, chunk, sizeof(chunk));
    total += sizeof(chunk);
    assert(osvc_session_holding(&s_session));
    /* Each message the browser takes wakes the service task, whose tick
     * offers the held output again; three make room for it. */
    const unsigned wakes = s_service_pending;
    assert(pump(2U) == 2U);
    assert(s_service_pending == wakes + 2U);
    osvc_session_tick(&s_session);
    assert(osvc_session_holding(&s_session));
    assert(pump(1U) == 1U);
    osvc_session_tick(&s_session);
    assert(!osvc_session_holding(&s_session));
    pump(0U);
    browser_channel_t *b = browser(1U);
    assert(b->binary_length == total);
    for (uint64_t k = 0U; k < total; ++k) {
        assert(b->binary[k] == pattern(k));
    }
    expect_no_message();

    /* A browser that takes nothing more. */
    for (int i = 0; i < 3; ++i) {
        sender_output(terminal_id, chunk, sizeof(chunk));
    }
    assert(osvc_session_holding(&s_session));
    s_fake.now += OSVC_TERMINAL_OUTPUT_HOLD_MS - 1;
    osvc_session_tick(&s_session);
    assert(osvc_session_holding(&s_session));
    s_fake.now += 1;
    osvc_session_tick(&s_session);
    assert(!osvc_session_holding(&s_session));
    fake_message_t close = expect_message(OSVC_SERVICE_TERMINAL,
                                          OSVC_TERMINAL_CLOSE);
    assert(osvc_get_u32(close.payload) == terminal_id);
    pump(0U);
    assert(b->binary_length == total + 2U * sizeof(chunk));
    expect_text(1U, "\r\n[Terminal closed: the browser did not keep up]"
                    "\r\n");
    assert(b->closed);
    service();
    expect_no_message();
    expect_table_empty();
    browser_reset();
}

/* Keystrokes beyond what the channel holds while the IN endpoint is busy. */
static void test_terminal_input_overflow(void)
{
    setup(true, false);
    assert(osvc_channels_claim(&s_channels, 1U, "guest-terminal"));
    service();
    const uint32_t terminal_id = expect_terminal_open(0U, 0U);
    fake_clear_output(&s_fake);
    static uint8_t keys[16 * 1024];
    for (size_t k = 0U; k < sizeof(keys); ++k) {
        keys[k] = pattern(k);
    }
    for (int i = 0; i < 4; ++i) {
        browser_binary(1U, keys, sizeof(keys));
    }
    assert(s_channels.counters.input_overflows == 0U);
    browser_binary(1U, "!", 1U);
    assert(s_channels.counters.input_overflows == 1U);
    service();
    /* The terminal closes; what came before the gap is not relayed. */
    fake_message_t message = expect_message(OSVC_SERVICE_TERMINAL,
                                            OSVC_TERMINAL_CLOSE);
    assert(osvc_get_u32(message.payload) == terminal_id);
    expect_no_message();
    pump(0U);
    expect_text(1U, "\r\n[" OSVC_TERMINAL_INPUT_LOST "]\r\n");
    assert(browser(1U)->closed);
    service();
    expect_table_empty();
    browser_reset();
}

/* Two shells that print and two users who type, across the rings' wrap:
 * every byte arrives in order on its own terminal. */
static void test_terminal_streams(void)
{
    setup(true, false);
    assert(osvc_channels_claim(&s_channels, 1U, "guest-terminal"));
    assert(osvc_channels_claim(&s_channels, 2U, "guest-terminal"));
    service();
    const uint32_t first = expect_terminal_open(0U, 0U);
    const uint32_t second = expect_terminal_open(0U, 0U);
    sender_terminal_status(OSVC_TERMINAL_OPENED, first, OSVC_TERMINAL_OPENED_OK);
    sender_terminal_status(OSVC_TERMINAL_OPENED, second,
                           OSVC_TERMINAL_OPENED_OK);
    fake_clear_output(&s_fake);
    static uint8_t piece[40000];
    static uint8_t typed[2U * 1024U * 1024U];
    uint64_t out_total = 0U;
    uint64_t in_total = 0U;
    size_t typed_length = 0U;
    unsigned pumps = 0U;
    for (unsigned round = 0U; round < 60U; ++round) {
        const size_t n = 1000U + (round * 7919U) % 39000U;
        for (size_t k = 0U; k < n; ++k) {
            piece[k] = pattern(out_total + k);
        }
        sender_output(first, piece, n);
        sender_output(second, piece, n);
        out_total += n;
        const size_t typing = 1U + (round * 3571U) % 20000U;
        for (size_t k = 0U; k < typing; ++k) {
            piece[k] = pattern(in_total + k + 7U);
        }
        browser_binary(1U, piece, typing);
        in_total += typing;
        service();
        typed_length += read_terminal_input(first, typed + typed_length,
                                            sizeof(typed) - typed_length);
        pumps += pump(0U);
    }
    assert(typed_length == in_total);
    for (uint64_t k = 0U; k < in_total; ++k) {
        assert(typed[k] == pattern(k + 7U));
    }
    for (uint32_t c = 1U; c <= 2U; ++c) {
        browser_channel_t *b = browser(c);
        assert(b->binary_length == out_total);
        for (uint64_t k = 0U; k < out_total; ++k) {
            assert(b->binary[k] == pattern(k));
        }
    }
    /* What the browsers took went back as credit, short of one step. */
    service();
    (void)read_terminal_input(first, typed, 0U);
    for (uint32_t id = first; id <= second; ++id) {
        assert(s_terminal_credit[id] <= out_total &&
               s_terminal_credit[id] + OSVC_TERMINAL_CREDIT_STEP > out_total);
    }
    printf("terminals: %llu bytes out to each of two browsers in %u messages, "
           "%llu bytes typed\n",
           (unsigned long long)out_total, pumps, (unsigned long long)in_total);
    assert(s_fake.terminal_closed_count == 0U);
    /* The peer goes away: the transport reports every channel closed. */
    osvc_channels_closed(&s_channels, 1U);
    osvc_channels_closed(&s_channels, 2U);
    service();
    (void)expect_message(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_CLOSE);
    (void)expect_message(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_CLOSE);
    expect_table_empty();
    browser_reset();
}

/* --- File drops ---------------------------------------------------------------- */

static uint32_t drop_files(const char *const *names, const uint64_t *sizes,
                           size_t count, uint32_t *transfer_ids)
{
    osvc_file_entry_t files[8];
    assert(count <= 8U);
    for (size_t i = 0U; i < count; ++i) {
        files[i] = (osvc_file_entry_t) { .name = names[i],
                                         .name_length = strlen(names[i]),
                                         .size = sizes[i] };
    }
    uint32_t drop_id = 0U;
    assert(osvc_session_file_drop(&s_session, files, count, &drop_id,
                                  transfer_ids) == NULL);
    (void)expect_message(OSVC_SERVICE_FILE, OSVC_FILE_DROP);
    fake_clear_output(&s_fake);
    return drop_id;
}

static uint32_t claim_drop(uint32_t channel, uint32_t drop_id)
{
    char label[32];
    snprintf(label, sizeof(label), "guestfile_%u", drop_id);
    assert(osvc_channels_claim(&s_channels, channel, label));
    return channel;
}

/* The browser reads a channel message as useGuestFileDrop.ts does. */
typedef struct {
    bool done;
    uint32_t transfer;
    uint64_t bytes;
    char status[16];
    char path[OSVC_FILE_PATH_REPORT_MAX + 1U];
    bool has_path;
} drop_message_t;

static bool next_drop_message(uint32_t channel, drop_message_t *m)
{
    const char *text = next_text(channel);
    if (text == NULL) {
        return false;
    }
    memset(m, 0, sizeof(*m));
    cJSON *json = cJSON_Parse(text);
    assert(json != NULL);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    const cJSON *transfer = cJSON_GetObjectItemCaseSensitive(json, "transfer");
    assert(cJSON_IsString(type) && cJSON_IsNumber(transfer));
    m->transfer = (uint32_t)transfer->valuedouble;
    if (strcmp(type->valuestring, "credit") == 0) {
        const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(json, "bytes");
        assert(cJSON_IsNumber(bytes) && bytes->valuedouble > 0);
        m->bytes = (uint64_t)bytes->valuedouble;
        assert(cJSON_GetArraySize(json) == 3);
    } else {
        assert(strcmp(type->valuestring, "done") == 0);
        m->done = true;
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(json, "status");
        assert(cJSON_IsString(status));
        snprintf(m->status, sizeof(m->status), "%s", status->valuestring);
        const cJSON *path = cJSON_GetObjectItemCaseSensitive(json, "path");
        if (path != NULL) {
            assert(cJSON_IsString(path) &&
                   strlen(path->valuestring) <= OSVC_FILE_PATH_REPORT_MAX);
            m->has_path = true;
            snprintf(m->path, sizeof(m->path), "%s", path->valuestring);
        }
    }
    cJSON_Delete(json);
    return true;
}

static void expect_credit(uint32_t channel, uint32_t transfer, uint64_t bytes)
{
    drop_message_t m;
    assert(next_drop_message(channel, &m));
    if (m.done || m.transfer != transfer || m.bytes != bytes) {
        fprintf(stderr, "expected credit %u %llu, got %s %u %llu\n", transfer,
                (unsigned long long)bytes, m.done ? "done" : "credit",
                m.transfer, (unsigned long long)m.bytes);
        abort();
    }
}

static void expect_done(uint32_t channel, uint32_t transfer, const char *status,
                        const char *path)
{
    drop_message_t m;
    assert(next_drop_message(channel, &m));
    if (!m.done || m.transfer != transfer || strcmp(m.status, status) != 0) {
        fprintf(stderr, "expected done %u %s, got %s %u %s\n", transfer,
                status, m.done ? "done" : "credit", m.transfer, m.status);
        abort();
    }
    assert(m.has_path == (path != NULL));
    assert(path == NULL || strcmp(m.path, path) == 0);
}

/* Outcomes without a path, in any order: the table takes the transfers in
 * turn from where its last message left off. */
static void expect_dones(uint32_t channel, const uint32_t *transfers,
                         size_t count, const char *status)
{
    bool seen[8] = {false};
    assert(count <= 8U);
    for (size_t n = 0U; n < count; ++n) {
        drop_message_t m;
        assert(next_drop_message(channel, &m));
        assert(m.done && strcmp(m.status, status) == 0 && !m.has_path);
        size_t i = 0U;
        while (i < count && transfers[i] != m.transfer) {
            ++i;
        }
        assert(i < count && !seen[i]);
        seen[i] = true;
    }
}

/* The browser's chunk: transfer ID, little-endian, then the bytes. */
static void browser_chunk(uint32_t channel, uint32_t transfer, uint64_t offset,
                          size_t length)
{
    static uint8_t message[4U + 32U * 1024U];
    assert(length <= 32U * 1024U);
    message[0] = (uint8_t)transfer;
    message[1] = (uint8_t)(transfer >> 8);
    message[2] = (uint8_t)(transfer >> 16);
    message[3] = (uint8_t)(transfer >> 24);
    for (size_t k = 0U; k < length; ++k) {
        message[4U + k] = pattern(offset + k + transfer);
    }
    browser_binary(channel, message, 4U + length);
}

/* The sender reads every file.chunk: each continues its transfer. */
static uint64_t sender_reads_chunks(uint32_t transfer, uint64_t *offset)
{
    uint64_t total = 0U;
    fake_message_t message;
    while (fake_next_message(&s_fake, &message)) {
        assert(message.service == OSVC_SERVICE_FILE &&
               message.kind == OSVC_FILE_CHUNK);
        assert(osvc_get_u32(message.payload) == transfer);
        assert(osvc_get_u64(message.payload + 4) == *offset);
        const size_t n = message.length - 12U;
        for (size_t k = 0U; k < n; ++k) {
            assert(message.payload[12U + k] == pattern(*offset + k + transfer));
        }
        *offset += n;
        total += n;
    }
    fake_clear_output(&s_fake);
    return total;
}

static void test_file_round_trip(void)
{
    setup(false, true);
    static const char *const names[] = {"a.txt", "b.bin"};
    static const uint64_t sizes[] = {5U, 100000U};
    uint32_t ids[2];
    const uint32_t drop_id = drop_files(names, sizes, 2U, ids);
    /* The sender is ready before the browser's channel opens: held. */
    sender_credit(OSVC_FILE_READY, ids[0], 1024U);
    sender_credit(OSVC_FILE_READY, ids[1], 65536U);
    const uint32_t ch = claim_drop(7U, drop_id);
    service();
    expect_no_message();
    pump(0U);
    /* No more than the file holds: 5 of the 1024 bytes. The rest of what
     * the browser may hold at once is less than a step for the second. */
    expect_credit(ch, ids[0], 5U);
    assert(next_text(ch) == NULL);

    /* The first file, then the second within the browser's credit. */
    browser_chunk(ch, ids[0], 0U, 5U);
    service();
    uint64_t offset = 0U;
    assert(sender_reads_chunks(ids[0], &offset) == 5U);
    sender_done(ids[0], OSVC_FILE_DONE_OK, "C:\\Users\\me\\Downloads\\a \"1\".txt",
                strlen("C:\\Users\\me\\Downloads\\a \"1\".txt"));
    pump(0U);
    expect_credit(ch, ids[1], OSVC_FILE_FLIGHT_BYTES);
    expect_done(ch, ids[0], "ok", "C:\\Users\\me\\Downloads\\a \"1\".txt");
    /* The sender opens the rest of the file once it wrote its first
     * 64 KiB. */
    uint64_t sent = 0U;
    uint64_t credit = OSVC_FILE_FLIGHT_BYTES;
    offset = 0U;
    for (unsigned rounds = 0U; offset < sizes[1]; ++rounds) {
        assert(rounds < 100U);
        uint64_t n = sizes[1] - sent < credit ? sizes[1] - sent : credit;
        n = n < 32768U ? n : 32768U;
        if (n > 0U) {
            browser_chunk(ch, ids[1], sent, (size_t)n);
            sent += n;
            credit -= n;
        }
        service();
        const uint64_t before = offset;
        (void)sender_reads_chunks(ids[1], &offset);
        if (before < 65536U && offset == 65536U) {
            sender_credit(OSVC_FILE_CREDIT, ids[1], 34464U);
        }
        pump(0U);
        drop_message_t m;
        while (next_drop_message(ch, &m)) {
            assert(!m.done && m.transfer == ids[1]);
            credit += m.bytes;
        }
    }
    assert(sent == sizes[1] && credit == 0U);
    sender_done(ids[1], OSVC_FILE_DONE_OK, "/home/me/Downloads/b.bin", 24U);
    pump(0U);
    expect_done(ch, ids[1], "ok", "/home/me/Downloads/b.bin");
    assert(!browser(ch)->closed);
    /* The browser closes the channel itself after the last outcome. */
    osvc_channels_closed(&s_channels, ch);
    service();
    expect_no_message();
    expect_table_empty();

    /* A drop whose channel the browser leaves open: closed 500 ms after
     * its last outcome. */
    static const char *const one[] = {"empty"};
    static const uint64_t zero[] = {0U};
    const uint32_t second = drop_files(one, zero, 1U, ids);
    const uint32_t ch2 = claim_drop(8U, second);
    service();
    sender_credit(OSVC_FILE_READY, ids[0], 4096U);
    sender_done(ids[0], OSVC_FILE_DONE_OK, "/tmp/empty", 10U);
    pump(0U);
    /* No credit for a file that ended. */
    expect_done(ch2, ids[0], "ok", "/tmp/empty");
    assert(!browser(ch2)->closed);
    s_fake.now += OSVC_FILE_CLOSE_DELAY_MS - 1;
    assert(pump(0U) == 0U);
    s_fake.now += 1;
    assert(pump(0U) == 1U);
    assert(browser(ch2)->closed);
    service();
    expect_table_empty();
    browser_reset();
}

static void test_file_unknown_and_late_channels(void)
{
    setup(false, true);
    /* A drop the session does not know: closed without a word. */
    claim_drop(1U, 12345U);
    service();
    assert(pump(0U) == 1U);
    assert(browser(1U)->closed && browser(1U)->text_count == 0U);
    service();
    expect_table_empty();

    /* Outcomes held for a channel that opens late; a second channel for
     * the same drop is closed. */
    static const char *const names[] = {"x", "y"};
    static const uint64_t sizes[] = {1U, 1U};
    uint32_t ids[2];
    const uint32_t drop_id = drop_files(names, sizes, 2U, ids);
    sender_credit(OSVC_FILE_READY, ids[0], 1U);
    sender_done(ids[1], OSVC_FILE_DONE_FAILED, "", 0U);
    claim_drop(2U, drop_id);
    claim_drop(3U, drop_id);
    service();
    pump(0U);
    expect_credit(2U, ids[0], 1U);
    expect_done(2U, ids[1], "failed", NULL);
    assert(!browser(2U)->closed);
    assert(browser(3U)->closed && browser(3U)->text_count == 0U);
    browser_chunk(2U, ids[0], 0U, 1U);
    service();
    uint64_t offset = 0U;
    assert(sender_reads_chunks(ids[0], &offset) == 1U);
    sender_done(ids[0], OSVC_FILE_DONE_OK, "/x", 2U);
    pump(0U);
    expect_done(2U, ids[0], "ok", "/x");
    osvc_channels_closed(&s_channels, 2U);
    service();
    expect_table_empty();
    browser_reset();
}

static void test_file_cancel_and_close(void)
{
    setup(false, true);
    static const char *const names[] = {"a", "b", "c"};
    static const uint64_t sizes[] = {1000U, 1000U, 1000U};
    uint32_t ids[3];
    const uint32_t drop_id = drop_files(names, sizes, 3U, ids);
    const uint32_t ch = claim_drop(4U, drop_id);
    service();
    for (size_t i = 0U; i < 3U; ++i) {
        sender_credit(OSVC_FILE_READY, ids[i], 1000U);
    }
    pump(0U);
    for (size_t i = 0U; i < 3U; ++i) {
        expect_credit(ch, ids[i], 1000U);
    }
    /* A cancel for another drop's transfer, a malformed one, then a real
     * one: file.cancel to the sender, done cancelled to the browser. */
    char text[96];
    snprintf(text, sizeof(text), "{\"type\":\"cancel\",\"transfer\":%u}",
             drop_id);
    browser_text(ch, text);
    browser_text(ch, "{\"type\":\"cancel\"}");
    browser_text(ch, "not json");
    snprintf(text, sizeof(text), "{\"type\":\"c\\u0061ncel\",\"transfer\":%u}",
             ids[1]);
    browser_text(ch, text);
    service();
    fake_message_t cancel = expect_message(OSVC_SERVICE_FILE, OSVC_FILE_CANCEL);
    assert(osvc_get_u32(cancel.payload) == ids[1]);
    expect_no_message();
    pump(0U);
    expect_done(ch, ids[1], "cancelled", NULL);
    /* Its unused credit is back in the window. */
    assert(s_channels.channels[0].file->outstanding == 2000U);

    /* Bytes, then the browser closes mid-transfer: the open transfers are
     * cancelled on the sender, and nothing more reaches the channel. */
    browser_chunk(ch, ids[0], 0U, 500U);
    browser_chunk(ch, ids[2], 0U, 4U);
    /* A message with no bytes after the ID is ignored. */
    browser_binary(ch, "\x01\x02\x03\x04", 4U);
    service();
    fake_message_t chunk = expect_message(OSVC_SERVICE_FILE, OSVC_FILE_CHUNK);
    assert(osvc_get_u32(chunk.payload) == ids[0] && chunk.length == 512U);
    chunk = expect_message(OSVC_SERVICE_FILE, OSVC_FILE_CHUNK);
    assert(osvc_get_u32(chunk.payload) == ids[2] && chunk.length == 16U);
    expect_no_message();
    osvc_channels_closed(&s_channels, ch);
    service();
    cancel = expect_message(OSVC_SERVICE_FILE, OSVC_FILE_CANCEL);
    assert(osvc_get_u32(cancel.payload) == ids[0]);
    cancel = expect_message(OSVC_SERVICE_FILE, OSVC_FILE_CANCEL);
    assert(osvc_get_u32(cancel.payload) == ids[2]);
    expect_no_message();
    assert(pump(0U) == 0U);
    expect_table_empty();
    /* The drop is gone: a new one can start. */
    drop_files(names, sizes, 1U, ids);
    browser_reset();
}

/* The sender's file.cancel for each of the transfers, in order. */
static void expect_cancels(const uint32_t *transfers, size_t count)
{
    for (size_t i = 0U; i < count; ++i) {
        fake_message_t cancel =
            expect_message(OSVC_SERVICE_FILE, OSVC_FILE_CANCEL);
        assert(osvc_get_u32(cancel.payload) == transfers[i]);
    }
    expect_no_message();
}

/* A drop's channel that is refused or closed before it is attached ends the
 * drop at once: the next drop is taken without waiting for the attach
 * timeout. */
static void test_file_refused_channels(void)
{
    setup(false, true);
    static const char *const names[] = {"a", "b"};
    static const uint64_t sizes[] = {10U, 20U};
    uint32_t ids[2];

    /* No memory for the channel's buffers: the drop fails, the browser
     * hears it for each transfer, and the channel closes. */
    uint32_t drop_id = drop_files(names, sizes, 2U, ids);
    s_fail_alloc = true;
    claim_drop(1U, drop_id);
    s_fail_alloc = false;
    assert(s_channels.counters.no_memory == 1U);
    browser_chunk(1U, ids[0], 0U, 4U);
    /* The transport sends while the service task writes file.cancel: the
     * channel gets its outcomes before its close. */
    s_pump_in_write = true;
    service();
    s_pump_in_write = false;
    expect_cancels(ids, 2U);
    pump(0U);
    expect_dones(1U, ids, 2U, "failed");
    assert(browser(1U)->closed && next_text(1U) == NULL);
    service();
    expect_table_empty();
    drop_id = drop_files(names, sizes, 2U, ids);

    /* The browser closes that channel before the service task saw it. */
    s_fail_alloc = true;
    claim_drop(2U, drop_id);
    s_fail_alloc = false;
    osvc_channels_closed(&s_channels, 2U);
    service();
    expect_cancels(ids, 2U);
    assert(pump(0U) == 0U);
    expect_table_empty();
    drop_id = drop_files(names, sizes, 2U, ids);

    /* A channel with buffers that the browser closes before it is
     * attached. */
    claim_drop(3U, drop_id);
    osvc_channels_closed(&s_channels, 3U);
    service();
    expect_cancels(ids, 2U);
    expect_table_empty();
    drop_id = drop_files(names, sizes, 2U, ids);

    /* The table is full: refused, and the drop ends. */
    for (uint32_t i = 0U; i < OSVC_CHANNELS_MAX; ++i) {
        assert(osvc_channels_claim(&s_channels, 10U + i, "guestfile_99"));
    }
    char label[32];
    snprintf(label, sizeof(label), "guestfile_%u", drop_id);
    assert(!osvc_channels_claim(&s_channels, 20U, label));
    service();
    expect_cancels(ids, 2U);
    pump(0U);
    service();
    expect_table_empty();
    drop_files(names, sizes, 2U, ids);
    browser_reset();
}

/* The sender finishes a transfer while credit for it is still on its way
 * to the browser: that credit must not count against the window, or every
 * such race shrinks it for the rest of the drop. */
static void test_file_done_while_credit_in_flight(void)
{
    setup(false, true);
    static const char *const names[] = {"a", "b"};
    static const uint64_t sizes[] = {1000U, 1000U};
    uint32_t ids[2];
    const uint32_t drop_id = drop_files(names, sizes, 2U, ids);
    const uint32_t ch = claim_drop(4U, drop_id);
    service();
    sender_credit(OSVC_FILE_READY, ids[0], 1000U);
    osvc_channel_output_t out;
    assert(osvc_channels_next_output(&s_channels, &out));
    assert(out.channel == ch && out.text);
    sender_done(ids[0], OSVC_FILE_DONE_FAILED, "", 0U);
    osvc_channels_output_done(&s_channels, ch, true);
    const osvc_file_channel_t *f = s_channels.channels[0].file;
    assert(f->transfers[0].pending == 0U && f->transfers[0].unused == 0U);
    assert(f->outstanding == 0U);
    pump(0U);
    expect_done(ch, ids[0], "failed", NULL);
    /* The other file still gets the whole of its credit. */
    sender_credit(OSVC_FILE_READY, ids[1], 1000U);
    pump(0U);
    expect_credit(ch, ids[1], 1000U);
    assert(f->outstanding == 1000U);
    osvc_channels_closed(&s_channels, ch);
    service();
    (void)expect_message(OSVC_SERVICE_FILE, OSVC_FILE_CANCEL);
    pump(0U);
    expect_table_empty();
    browser_reset();
}

/* A browser that sends more than the table can hold: its credit was not
 * that large, so the drop fails. */
static void test_file_overflow(void)
{
    setup(false, true);
    static const char *const names[] = {"big", "other"};
    static const uint64_t sizes[] = {10U * 1024U * 1024U, 10U};
    uint32_t ids[2];
    const uint32_t drop_id = drop_files(names, sizes, 2U, ids);
    const uint32_t ch = claim_drop(5U, drop_id);
    service();
    sender_credit(OSVC_FILE_READY, ids[0], 4U * 1024U * 1024U);
    pump(0U);
    expect_credit(ch, ids[0], OSVC_FILE_FLIGHT_BYTES);
    uint64_t sent = 0U;
    for (int i = 0; i < 6; ++i) {
        browser_chunk(ch, ids[0], sent, 32768U);
        sent += 32768U;
    }
    assert(s_channels.counters.input_overflows == 1U);
    service();
    /* The drop fails; what was held is dropped, not relayed. */
    fake_message_t message = expect_message(OSVC_SERVICE_FILE,
                                            OSVC_FILE_CANCEL);
    assert(osvc_get_u32(message.payload) == ids[0]);
    message = expect_message(OSVC_SERVICE_FILE, OSVC_FILE_CANCEL);
    assert(osvc_get_u32(message.payload) == ids[1]);
    expect_no_message();
    pump(0U);
    expect_dones(ch, ids, 2U, "failed");
    s_fake.now += OSVC_FILE_CLOSE_DELAY_MS;
    pump(0U);
    assert(browser(ch)->closed);
    service();
    expect_table_empty();
    browser_reset();
}

/* Each message also takes a record header in the input ring, which has room
 * for the headers of messages of at least 1 KiB or that use up their credit.
 * A browser that sends single bytes within its credit fills it instead, and
 * the drop fails. */
static void test_file_small_chunks(void)
{
    setup(false, true);
    static const char *const names[] = {"big", "small"};
    /* The first takes all the credit the browser may hold at once. */
    static const uint64_t sizes[] = {OSVC_FILE_FLIGHT_BYTES, 1500U};
    uint32_t ids[2];
    const uint32_t drop_id = drop_files(names, sizes, 2U, ids);
    const uint32_t ch = claim_drop(5U, drop_id);
    service();
    sender_credit(OSVC_FILE_READY, ids[0], 256U * 1024U);
    sender_credit(OSVC_FILE_READY, ids[1], 256U * 1024U);
    pump(0U);
    expect_credit(ch, ids[0], sizes[0]);
    assert(next_text(ch) == NULL);
    uint64_t sent = 0U;
    while (s_channels.counters.input_overflows == 0U) {
        browser_chunk(ch, ids[0], sent, 1U);
        sent++;
    }
    assert(sent < sizes[0]);
    service();
    fake_message_t message = expect_message(OSVC_SERVICE_FILE,
                                            OSVC_FILE_CANCEL);
    assert(osvc_get_u32(message.payload) == ids[0]);
    message = expect_message(OSVC_SERVICE_FILE, OSVC_FILE_CANCEL);
    assert(osvc_get_u32(message.payload) == ids[1]);
    expect_no_message();
    pump(0U);
    expect_dones(ch, ids, 2U, "failed");
    s_fake.now += OSVC_FILE_CLOSE_DELAY_MS;
    pump(0U);
    assert(browser(ch)->closed);
    service();
    expect_table_empty();
    browser_reset();
}

static void test_file_paths(void)
{
    setup(false, true);
    static const char *const names[] = {"1", "2", "3", "4"};
    static const uint64_t sizes[] = {0U, 0U, 0U, 0U};
    uint32_t ids[4];
    const uint32_t drop_id = drop_files(names, sizes, 4U, ids);
    const uint32_t ch = claim_drop(6U, drop_id);
    service();
    static char path[2048];
    /* Every character JSON must escape, and UTF-8. */
    static const char odd[] = "/t\x01\x1f\"\\/\t\n\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80";
    sender_done(ids[0], OSVC_FILE_DONE_OK, odd, sizeof(odd) - 1U);
    /* The longest path reported, and one byte more. */
    memset(path, 'p', sizeof(path));
    path[0] = '/';
    sender_done(ids[1], OSVC_FILE_DONE_OK, path, OSVC_FILE_PATH_REPORT_MAX);
    sender_done(ids[2], OSVC_FILE_DONE_OK, path, OSVC_FILE_PATH_REPORT_MAX + 1U);
    /* The longest path, all of it escaped: the message still fits. */
    memset(path, '\x01', OSVC_FILE_PATH_REPORT_MAX);
    sender_done(ids[3], OSVC_FILE_DONE_OK, path, OSVC_FILE_PATH_REPORT_MAX);
    pump(0U);
    expect_done(ch, ids[0], "ok", odd);
    memset(path, 'p', sizeof(path));
    path[0] = '/';
    path[OSVC_FILE_PATH_REPORT_MAX] = '\0';
    expect_done(ch, ids[1], "ok", path);
    expect_done(ch, ids[2], "ok", NULL);
    memset(path, '\x01', OSVC_FILE_PATH_REPORT_MAX);
    expect_done(ch, ids[3], "ok", path);
    s_fake.now += OSVC_FILE_CLOSE_DELAY_MS;
    pump(0U);
    service();
    expect_table_empty();
    browser_reset();
}

/* The transport holds an output while the browser closes the channel: the
 * slot waits for it. A refused send comes again. */
static void test_output_held_across_close(void)
{
    setup(true, false);
    assert(osvc_channels_claim(&s_channels, 1U, "guest-terminal"));
    service();
    const uint32_t terminal_id = expect_terminal_open(0U, 0U);
    sender_terminal_status(OSVC_TERMINAL_OPENED, terminal_id,
                           OSVC_TERMINAL_OPENED_OK);
    sender_output(terminal_id, (const uint8_t *)"abc", 3U);
    osvc_channel_output_t out;
    assert(osvc_channels_next_output(&s_channels, &out));
    assert(out.channel == 1U && !out.text && out.length == 3U);
    /* Not sent: the same output comes again. */
    osvc_channels_output_done(&s_channels, 1U, false);
    assert(osvc_channels_next_output(&s_channels, &out));
    assert(out.length == 3U && memcmp(out.data, "abc", 3U) == 0);
    /* One output at a time per channel. */
    osvc_channel_output_t other;
    assert(!osvc_channels_next_output(&s_channels, &other));
    osvc_channels_closed(&s_channels, 1U);
    service();
    (void)expect_message(OSVC_SERVICE_TERMINAL, OSVC_TERMINAL_CLOSE);
    assert(s_channels.channels[0].release);
    assert(s_allocs == s_frees + 1U);
    assert(memcmp(out.data, "abc", 3U) == 0);
    osvc_channels_output_done(&s_channels, 1U, true);
    expect_table_empty();
    browser_reset();
}

/* A 6 MiB file and a small one at once through the credit window, with a
 * sender that keeps 256 KiB of credit open. The browser never holds more
 * credit than it may at once, nor with what was not relayed more than the
 * window, and the table never more than its buffer. */
static void test_file_throughput(void)
{
    setup(false, true);
    static const char *const names[] = {"big.iso", "notes.txt"};
    static const uint64_t sizes[] = {6U * 1024U * 1024U + 123U, 70000U};
    uint32_t ids[2];
    const uint32_t drop_id = drop_files(names, sizes, 2U, ids);
    const uint32_t ch = claim_drop(9U, drop_id);
    service();
    for (size_t i = 0U; i < 2U; ++i) {
        sender_credit(OSVC_FILE_READY, ids[i], 256U * 1024U);
    }
    uint64_t browser_credit[2] = {0U, 0U};
    uint64_t browser_sent[2] = {0U, 0U};
    uint64_t sender_offset[2] = {0U, 0U};
    unsigned credit_messages = 0U;
    unsigned chunk_messages = 0U;
    unsigned rounds = 0U;
    unsigned service_calls = 0U;
    size_t peak_buffered = 0U;
    unsigned done = 0U;
    while (done < 2U) {
        assert(++rounds < 10000U);
        pump(0U);
        drop_message_t m;
        while (next_drop_message(ch, &m)) {
            const size_t i = m.transfer == ids[0] ? 0U : 1U;
            assert(m.transfer == ids[i]);
            if (m.done) {
                assert(strcmp(m.status, "ok") == 0);
                done++;
            } else {
                browser_credit[i] += m.bytes;
                credit_messages++;
            }
        }
        uint64_t outstanding = 0U;
        for (size_t i = 0U; i < 2U; ++i) {
            outstanding += browser_credit[i];
        }
        assert(outstanding <= OSVC_FILE_FLIGHT_BYTES);
        assert(outstanding + s_channels.channels[0].file->input.used <=
               OSVC_FILE_WINDOW_BYTES + 64U);
        /* The pumps, one chunk each in turn (the UI's are concurrent). */
        bool more = true;
        while (more) {
            more = false;
            for (size_t i = 0U; i < 2U; ++i) {
                uint64_t n = sizes[i] - browser_sent[i];
                n = n < browser_credit[i] ? n : browser_credit[i];
                n = n < 32768U ? n : 32768U;
                if (n == 0U) {
                    continue;
                }
                browser_chunk(ch, ids[i], browser_sent[i], (size_t)n);
                browser_sent[i] += n;
                browser_credit[i] -= n;
                chunk_messages++;
                more = true;
            }
        }
        assert(s_channels.counters.input_overflows == 0U);
        const size_t buffered = s_channels.channels[0].file->input.used;
        peak_buffered = buffered > peak_buffered ? buffered : peak_buffered;
        service_calls += service();
        /* The sender writes what arrived and opens as much credit again. */
        fake_message_t message;
        while (fake_next_message(&s_fake, &message)) {
            assert(message.kind == OSVC_FILE_CHUNK);
            const uint32_t transfer = osvc_get_u32(message.payload);
            const size_t i = transfer == ids[0] ? 0U : 1U;
            assert(osvc_get_u64(message.payload + 4) == sender_offset[i]);
            const size_t n = message.length - 12U;
            for (size_t k = 0U; k < n; k += 997U) {
                assert(message.payload[12U + k] ==
                       pattern(sender_offset[i] + k + transfer));
            }
            sender_offset[i] += n;
        }
        fake_clear_output(&s_fake);
        for (size_t i = 0U; i < 2U; ++i) {
            if (sender_offset[i] == sizes[i] &&
                !s_session.file_drop.transfers[i].ended &&
                s_session.file_drop.drop_id == drop_id) {
                sender_done(ids[i], OSVC_FILE_DONE_OK, "/d", 2U);
            } else if (s_session.file_drop.drop_id == drop_id &&
                       !s_session.file_drop.transfers[i].ended) {
                const uint64_t open = s_session.file_drop.transfers[i].credit -
                                      sender_offset[i];
                if (open < 256U * 1024U) {
                    sender_credit(OSVC_FILE_CREDIT, ids[i],
                                  (uint32_t)(256U * 1024U - open));
                }
            }
        }
    }
    assert(peak_buffered <= OSVC_FILE_INPUT_BYTES);
    printf("drop: %llu bytes in %u rounds, %u browser chunks, %u credit "
           "messages, %u service calls, peak buffer %zu bytes\n",
           (unsigned long long)(sizes[0] + sizes[1]), rounds, chunk_messages,
           credit_messages, service_calls, peak_buffered);
    osvc_channels_closed(&s_channels, ch);
    service();
    expect_table_empty();
    browser_reset();
}

/* The session ends (the sender went away): every terminal says so, every
 * transfer fails, and the channels close. */
static void test_session_end(void)
{
    setup(true, true);
    assert(osvc_channels_claim(&s_channels, 1U, "guest-terminal"));
    static const char *const names[] = {"f"};
    static const uint64_t sizes[] = {100U};
    uint32_t ids[1];
    const uint32_t drop_id = drop_files(names, sizes, 1U, ids);
    claim_drop(2U, drop_id);
    service();
    (void)expect_terminal_open(0U, 0U);
    osvc_session_end(&s_session, "the IN endpoint did not drain");
    s_fake.now += OSVC_FILE_CLOSE_DELAY_MS;
    pump(0U);
    expect_text(1U, "\r\n[OS Services disconnected]\r\n");
    assert(browser(1U)->closed);
    expect_done(2U, ids[0], "failed", NULL);
    assert(browser(2U)->closed);
    service();
    expect_table_empty();
    browser_reset();
}

int main(void)
{
    test_parsers();
    test_claims();
    test_terminal_round_trip();
    test_terminal_ends();
    test_terminal_slow_browser();
    test_terminal_input_overflow();
    test_terminal_streams();
    test_file_round_trip();
    test_file_unknown_and_late_channels();
    test_file_cancel_and_close();
    test_file_refused_channels();
    test_file_done_while_credit_in_flight();
    test_file_overflow();
    test_file_small_chunks();
    test_file_paths();
    test_output_held_across_close();
    test_file_throughput();
    test_session_end();
    browser_reset();
    for (size_t i = 0U; i < BROWSER_CHANNELS; ++i) {
        free(s_browser[i].binary);
    }
    printf("os_services channels: all tests passed\n");
    return 0;
}

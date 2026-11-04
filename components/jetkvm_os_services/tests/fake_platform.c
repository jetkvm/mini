/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "fake_platform.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "monocypher-ed25519.h"

static int64_t fake_now(void *ctx)
{
    return ((fake_platform_t *)ctx)->now;
}

static void fake_random(void *ctx, uint8_t *out, size_t length)
{
    fake_platform_t *fake = ctx;
    for (size_t i = 0U; i < length; ++i) {
        /* xorshift64* */
        fake->random_state ^= fake->random_state >> 12;
        fake->random_state ^= fake->random_state << 25;
        fake->random_state ^= fake->random_state >> 27;
        out[i] = (uint8_t)((fake->random_state * 2685821657736338717ULL) >> 56);
    }
}

static bool fake_public_key(void *ctx, uint8_t key[OSVC_PUBLIC_KEY_LEN])
{
    memcpy(key, ((fake_platform_t *)ctx)->public_key, OSVC_PUBLIC_KEY_LEN);
    return true;
}

static bool fake_sign(void *ctx, const uint8_t *message, size_t length,
                      uint8_t signature[OSVC_SIGNATURE_LEN])
{
    crypto_ed25519_sign(signature, ((fake_platform_t *)ctx)->secret_key,
                        message, length);
    return true;
}

static bool fake_write(void *ctx, const uint8_t envelope[OSVC_ENVELOPE_LEN],
                       const uint8_t *head, size_t head_length,
                       const uint8_t *body, size_t body_length)
{
    fake_platform_t *fake = ctx;
    fake->writes++;
    const size_t total = OSVC_ENVELOPE_LEN + head_length + body_length;
    if (fake->fail_writes || fake->out_length + total > FAKE_OUT_CAPACITY) {
        return false;
    }
    assert(osvc_get_u32(envelope + 4) == head_length + body_length);
    memcpy(fake->out + fake->out_length, envelope, OSVC_ENVELOPE_LEN);
    fake->out_length += OSVC_ENVELOPE_LEN;
    if (head_length > 0U) {
        memcpy(fake->out + fake->out_length, head, head_length);
        fake->out_length += head_length;
    }
    if (body_length > 0U) {
        memcpy(fake->out + fake->out_length, body, body_length);
        fake->out_length += body_length;
    }
    return true;
}

static void fake_session_ended(void *ctx, const char *reason)
{
    fake_platform_t *fake = ctx;
    fake->ended++;
    snprintf(fake->end_reason, sizeof(fake->end_reason), "%s",
             reason ? reason : "");
}

static void fake_reset_transport(void *ctx)
{
    fake_platform_t *fake = ctx;
    fake->resets++;
    /* What was queued for the IN endpoint is gone with the old session. */
    fake->out_length = 0U;
    fake->out_read = 0U;
}

static void fake_state_changed(void *ctx)
{
    ((fake_platform_t *)ctx)->state_changes++;
}

static void fake_clipboard_done(void *ctx, uint32_t request_id,
                                osvc_clipboard_status_t status, bool answered,
                                const uint8_t *text, size_t length,
                                bool truncated)
{
    fake_platform_t *fake = ctx;
    assert(fake->clipboard_count < FAKE_CLIPBOARD_EVENTS);
    fake_clipboard_event_t *event = &fake->clipboard[fake->clipboard_count++];
    event->request_id = request_id;
    event->status = status;
    event->answered = answered;
    event->length = length;
    event->truncated = truncated;
    if (length > 0U) {
        memcpy(event->text, text, length);
    }
    event->text[length] = '\0';
}

static bool fake_terminal_output(void *ctx, uint32_t terminal_id,
                                 const uint8_t *data, size_t length)
{
    fake_platform_t *fake = ctx;
    assert(terminal_id != 0U && length > 0U);
    fake->terminal_output_calls++;
    if (fake->backed_up) {
        return false;
    }
    assert(fake->terminal_output_length + length <= FAKE_TERMINAL_OUTPUT);
    memcpy(fake->terminal_output + fake->terminal_output_length, data, length);
    fake->terminal_output_length += length;
    fake->terminal_output_id = terminal_id;
    return true;
}

static void fake_terminal_closed(void *ctx, uint32_t terminal_id,
                                 const char *reason, bool exited)
{
    fake_platform_t *fake = ctx;
    assert(fake->terminal_closed_count < FAKE_TERMINAL_EVENTS);
    fake_terminal_closed_t *event =
        &fake->terminal_closed[fake->terminal_closed_count++];
    event->terminal_id = terminal_id;
    snprintf(event->reason, sizeof(event->reason), "%s", reason);
    event->exited = exited;
}

static fake_file_event_t *next_file_event(fake_platform_t *fake)
{
    assert(fake->file_count < FAKE_FILE_EVENTS);
    fake_file_event_t *event = &fake->file[fake->file_count++];
    memset(event, 0, sizeof(*event));
    return event;
}

static void fake_file_credit(void *ctx, uint32_t drop_id, uint32_t transfer_id,
                             uint64_t bytes)
{
    assert(bytes > 0U);
    fake_file_event_t *event = next_file_event(ctx);
    event->drop_id = drop_id;
    event->transfer_id = transfer_id;
    event->bytes = bytes;
}

static void fake_file_done(void *ctx, uint32_t drop_id, uint32_t transfer_id,
                           osvc_file_status_t status, const char *path,
                           size_t path_length, bool last)
{
    assert(path != NULL);
    fake_file_event_t *event = next_file_event(ctx);
    event->done = true;
    event->drop_id = drop_id;
    event->transfer_id = transfer_id;
    event->status = status;
    assert(path_length < sizeof(event->path));
    memcpy(event->path, path, path_length);
    event->path_length = path_length;
    event->last = last;
}

void fake_platform_init(fake_platform_t *fake, osvc_platform_t *platform)
{
    memset(fake, 0, sizeof(*fake));
    fake->now = 1000000;
    fake->random_state = 0x9e3779b97f4a7c15ULL;
    uint8_t seed[32];
    for (size_t i = 0U; i < sizeof(seed); ++i) {
        seed[i] = (uint8_t)(i * 7U + 1U);
    }
    crypto_ed25519_key_pair(fake->secret_key, fake->public_key, seed);
    *platform = (osvc_platform_t) {
        .ctx = fake,
        .now_ms = fake_now,
        .fill_random = fake_random,
        .public_key = fake_public_key,
        .sign = fake_sign,
        .write = fake_write,
        .session_ended = fake_session_ended,
        .reset_transport = fake_reset_transport,
        .state_changed = fake_state_changed,
        .clipboard_done = fake_clipboard_done,
        .terminal_output = fake_terminal_output,
        .terminal_closed = fake_terminal_closed,
        .file_credit = fake_file_credit,
        .file_done = fake_file_done,
    };
}

bool fake_next_message(fake_platform_t *fake, fake_message_t *message)
{
    if (fake->out_read + OSVC_ENVELOPE_LEN > fake->out_length) {
        return false;
    }
    const uint8_t *p = fake->out + fake->out_read;
    message->service = osvc_get_u16(p);
    message->kind = osvc_get_u16(p + 2);
    message->length = osvc_get_u32(p + 4);
    message->payload = p + OSVC_ENVELOPE_LEN;
    assert(fake->out_read + OSVC_ENVELOPE_LEN + message->length <=
           fake->out_length);
    fake->out_read += OSVC_ENVELOPE_LEN + message->length;
    return true;
}

void fake_clear_output(fake_platform_t *fake)
{
    fake->out_length = 0U;
    fake->out_read = 0U;
}

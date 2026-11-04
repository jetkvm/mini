/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * A flat C API over one osvc_session_t for test_os_services_protocol.py
 * (ctypes). The platform is internal: a clock the test sets, a seeded
 * random source, an Ed25519 key from a seed, and an output queue the test
 * drains. With video, the display and slot are fake_video.c's g_fake_video,
 * which the test reads and sets directly.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fake_video.h"
#include "monocypher-ed25519.h"
#include "os_services_session.h"

#define SHIM_OUT_CAPACITY (4U * 1024U * 1024U)
#define SHIM_EVENTS 64U
#define SHIM_SERVICE_EVENTS 4096U
#define SHIM_TERMINAL_BYTES (4U * 1024U * 1024U)

/* What the session told the browser through the terminal and file hooks,
 * in order. Output bytes go to a separate stream; each output event holds
 * their count. */
enum {
    SHIM_TERMINAL_OUTPUT = 1,
    SHIM_TERMINAL_CLOSED = 2,
    SHIM_FILE_CREDIT = 3,
    SHIM_FILE_DONE = 4,
};

typedef struct {
    int32_t type;
    uint32_t id;       /* terminal_id or drop_id */
    uint32_t transfer; /* transfer_id */
    int32_t status;    /* terminal_closed: exited; file_done: status */
    int32_t last;      /* file_done: last of the drop */
    uint32_t length;   /* output bytes, or the length of text */
    uint64_t bytes;    /* file_credit */
    char text[512];    /* terminal_closed reason or file_done path */
} shim_service_event_t;

typedef struct {
    uint32_t request_id;
    int32_t status;
    int32_t answered;
    uint32_t length;
    int32_t truncated;
    uint8_t text[OSVC_CLIPBOARD_MAX_TEXT];
} shim_clipboard_event_t;

static osvc_session_t s_session;
static uint8_t s_payload[OSVC_MAX_CONTROL_PAYLOAD];
static int64_t s_now;
static uint64_t s_random;
static uint8_t s_secret[64];
static uint8_t s_public[32];
static uint8_t *s_out;
static size_t s_out_length;
static int s_fail_writes;
static int s_reset_pending;
static int s_ended;
static int s_state_changes;
static shim_clipboard_event_t s_events[SHIM_EVENTS];
static size_t s_event_count;
static shim_service_event_t *s_service_events;
static size_t s_service_event_count;
static int s_service_events_lost;
static uint8_t *s_terminal_bytes;
static size_t s_terminal_bytes_length;
static int s_backed_up;

static int64_t shim_now(void *ctx)
{
    (void)ctx;
    return s_now;
}

static void shim_random(void *ctx, uint8_t *out, size_t length)
{
    (void)ctx;
    for (size_t i = 0U; i < length; ++i) {
        s_random ^= s_random >> 12;
        s_random ^= s_random << 25;
        s_random ^= s_random >> 27;
        out[i] = (uint8_t)((s_random * 2685821657736338717ULL) >> 56);
    }
}

static bool shim_public_key(void *ctx, uint8_t key[OSVC_PUBLIC_KEY_LEN])
{
    (void)ctx;
    memcpy(key, s_public, sizeof(s_public));
    return true;
}

static bool shim_sign(void *ctx, const uint8_t *message, size_t length,
                      uint8_t signature[OSVC_SIGNATURE_LEN])
{
    (void)ctx;
    crypto_ed25519_sign(signature, s_secret, message, length);
    return true;
}

static bool shim_write(void *ctx, const uint8_t envelope[OSVC_ENVELOPE_LEN],
                       const uint8_t *head, size_t head_length,
                       const uint8_t *body, size_t body_length)
{
    (void)ctx;
    const size_t total = OSVC_ENVELOPE_LEN + head_length + body_length;
    if (s_fail_writes || s_out_length + total > SHIM_OUT_CAPACITY) {
        return false;
    }
    memcpy(s_out + s_out_length, envelope, OSVC_ENVELOPE_LEN);
    s_out_length += OSVC_ENVELOPE_LEN;
    if (head_length) {
        memcpy(s_out + s_out_length, head, head_length);
        s_out_length += head_length;
    }
    if (body_length) {
        memcpy(s_out + s_out_length, body, body_length);
        s_out_length += body_length;
    }
    return true;
}

static void shim_session_ended(void *ctx, const char *reason)
{
    (void)ctx;
    (void)reason;
    s_ended++;
}

static void shim_reset_transport(void *ctx)
{
    (void)ctx;
    /* Bytes queued for the old session never reach the new one here; on
     * the device they may (Byte stream), which the test covers by feeding
     * stale bytes itself. */
    s_out_length = 0U;
    s_reset_pending = 1;
}

static void shim_state_changed(void *ctx)
{
    (void)ctx;
    s_state_changes++;
}

static void shim_clipboard_done(void *ctx, uint32_t request_id,
                                osvc_clipboard_status_t status, bool answered,
                                const uint8_t *text, size_t length,
                                bool truncated)
{
    (void)ctx;
    if (s_event_count == SHIM_EVENTS) {
        return;
    }
    shim_clipboard_event_t *event = &s_events[s_event_count++];
    event->request_id = request_id;
    event->status = (int32_t)status;
    event->answered = answered;
    event->length = (uint32_t)length;
    event->truncated = truncated;
    if (length) {
        memcpy(event->text, text, length);
    }
}

static shim_service_event_t *next_service_event(int32_t type)
{
    if (s_service_event_count == SHIM_SERVICE_EVENTS) {
        s_service_events_lost = 1;
        return NULL;
    }
    shim_service_event_t *event = &s_service_events[s_service_event_count++];
    memset(event, 0, sizeof(*event));
    event->type = type;
    return event;
}

static void set_text(shim_service_event_t *event, const char *text,
                     size_t length)
{
    event->length = (uint32_t)length;
    const size_t n = length < sizeof(event->text) - 1U ?
                         length : sizeof(event->text) - 1U;
    if (n > 0U) {
        memcpy(event->text, text, n);
    }
}

static bool shim_terminal_output(void *ctx, uint32_t terminal_id,
                                 const uint8_t *data, size_t length)
{
    (void)ctx;
    if (s_backed_up) {
        return false;
    }
    if (s_terminal_bytes_length + length > SHIM_TERMINAL_BYTES) {
        s_service_events_lost = 1;
        return true;
    }
    shim_service_event_t *event = next_service_event(SHIM_TERMINAL_OUTPUT);
    if (event == NULL) {
        return true;
    }
    event->id = terminal_id;
    event->length = (uint32_t)length;
    memcpy(s_terminal_bytes + s_terminal_bytes_length, data, length);
    s_terminal_bytes_length += length;
    return true;
}

static void shim_terminal_closed(void *ctx, uint32_t terminal_id,
                                 const char *reason, bool exited)
{
    (void)ctx;
    shim_service_event_t *event = next_service_event(SHIM_TERMINAL_CLOSED);
    if (event != NULL) {
        event->id = terminal_id;
        event->status = exited;
        set_text(event, reason, strlen(reason));
    }
}

static void shim_file_credit(void *ctx, uint32_t drop_id, uint32_t transfer_id,
                             uint64_t bytes)
{
    (void)ctx;
    shim_service_event_t *event = next_service_event(SHIM_FILE_CREDIT);
    if (event != NULL) {
        event->id = drop_id;
        event->transfer = transfer_id;
        event->bytes = bytes;
    }
}

static void shim_file_done(void *ctx, uint32_t drop_id, uint32_t transfer_id,
                           osvc_file_status_t status, const char *path,
                           size_t path_length, bool last)
{
    (void)ctx;
    shim_service_event_t *event = next_service_event(SHIM_FILE_DONE);
    if (event != NULL) {
        event->id = drop_id;
        event->transfer = transfer_id;
        event->status = (int32_t)status;
        event->last = last;
        set_text(event, path != NULL ? path : "", path != NULL ? path_length : 0U);
    }
}

int shim_init(uint64_t seed, const uint8_t key_seed[32], int video)
{
    if (s_out == NULL) {
        s_out = malloc(SHIM_OUT_CAPACITY);
        s_service_events = malloc(SHIM_SERVICE_EVENTS *
                                  sizeof(shim_service_event_t));
        s_terminal_bytes = malloc(SHIM_TERMINAL_BYTES);
        if (s_out == NULL || s_service_events == NULL ||
            s_terminal_bytes == NULL) {
            return -1;
        }
    }
    s_service_event_count = 0U;
    s_service_events_lost = 0;
    s_terminal_bytes_length = 0U;
    s_backed_up = 0;
    s_now = 1000;
    s_random = seed ? seed : 1U;
    s_out_length = 0U;
    s_fail_writes = 0;
    s_reset_pending = 0;
    s_ended = 0;
    s_state_changes = 0;
    s_event_count = 0U;
    uint8_t copy[32];
    memcpy(copy, key_seed, sizeof(copy));
    crypto_ed25519_key_pair(s_secret, s_public, copy);
    osvc_platform_t platform = {
        .now_ms = shim_now,
        .fill_random = shim_random,
        .public_key = shim_public_key,
        .sign = shim_sign,
        .write = shim_write,
        .session_ended = shim_session_ended,
        .reset_transport = shim_reset_transport,
        .state_changed = shim_state_changed,
        .clipboard_done = shim_clipboard_done,
        .terminal_output = shim_terminal_output,
        .terminal_closed = shim_terminal_closed,
        .file_credit = shim_file_credit,
        .file_done = shim_file_done,
    };
    if (video) {
        fake_video_attach(&platform);
    }
    const osvc_settings_t settings = {
        .clipboard_enabled = true,
        .terminal_enabled = true,
        .files_enabled = true,
        .video_enabled = true,
        .width = 1920U,
        .height = 1080U,
        .layout = OSVC_LAYOUT_EXTEND,
    };
    osvc_session_init(&s_session, &platform, &settings, s_payload,
                      sizeof(s_payload));
    return 0;
}

void shim_set_now(int64_t now) { s_now = now; }
void shim_set_fail_writes(int fail) { s_fail_writes = fail; }
int shim_ended(void) { return s_ended; }
int shim_state_changes(void) { return s_state_changes; }

void shim_reset(uint8_t status[OSVC_USB_STATUS_LEN])
{
    osvc_session_usb_reset(&s_session, status);
}

void shim_status(uint8_t status[OSVC_USB_STATUS_LEN])
{
    /* The transport finishes reaping the old transfers between polls. */
    if (s_reset_pending) {
        s_reset_pending = 0;
        osvc_session_usb_status(&s_session, status);
        osvc_session_transport_drained(&s_session);
        return;
    }
    osvc_session_usb_status(&s_session, status);
}

size_t shim_receive(const uint8_t *data, size_t length)
{
    return osvc_session_receive(&s_session, data, length);
}

size_t shim_take_output(uint8_t *out, size_t capacity)
{
    const size_t n = s_out_length < capacity ? s_out_length : capacity;
    memcpy(out, s_out, n);
    memmove(s_out, s_out + n, s_out_length - n);
    s_out_length -= n;
    return n;
}

void shim_tick(void) { osvc_session_tick(&s_session); }

int shim_open(void) { return osvc_session_open(&s_session); }

int shim_approval_pending(void) { return s_session.approval_pending; }

void shim_public(uint8_t out[32]) { memcpy(out, s_public, 32); }

uint32_t shim_clipboard_request(const uint8_t *text, size_t length, int set,
                                int32_t *status)
{
    osvc_clipboard_status_t result;
    const uint32_t id = osvc_session_clipboard_request(
        &s_session, set ? text : NULL, length, &result);
    *status = (int32_t)result;
    return id;
}

size_t shim_clipboard_events(void) { return s_event_count; }

const shim_clipboard_event_t *shim_clipboard_event(size_t index)
{
    return index < s_event_count ? &s_events[index] : NULL;
}

void shim_set_settings(int clipboard, int terminal, int files,
                       const char *display, uint32_t width, uint32_t height)
{
    osvc_settings_t settings = s_session.settings;
    settings.clipboard_enabled = clipboard;
    settings.terminal_enabled = terminal;
    settings.files_enabled = files;
    strncpy(settings.capture_display, display,
            sizeof(settings.capture_display) - 1U);
    settings.capture_display[sizeof(settings.capture_display) - 1U] = '\0';
    settings.width = width;
    settings.height = height;
    osvc_session_apply_settings(&s_session, &settings);
}

const osvc_counters_t *shim_counters(void) { return &s_session.counters; }

uint32_t shim_registered(void)
{
    return (uint32_t)s_session.clipboard_registered |
           ((uint32_t)s_session.video_registered << 1) |
           ((uint32_t)s_session.terminal_shell << 2) |
           ((uint32_t)s_session.file_receive << 3) |
           ((uint32_t)s_session.update_install << 4);
}

void shim_set_video(int enabled, uint32_t target_kbps)
{
    osvc_settings_t settings = s_session.settings;
    settings.video_enabled = enabled;
    settings.target_kbps = target_kbps;
    osvc_session_apply_settings(&s_session, &settings);
}

void shim_display_status(int presented, int owned)
{
    osvc_session_display_status(&s_session, presented, owned);
}

int shim_video_source(void) { return (int)s_session.video_source; }

const char *shim_video_error(void) { return s_session.video_error; }

const osvc_video_counters_t *shim_video_counters(void)
{
    return &s_session.video_counters;
}

int shim_frame_waiting(void) { return osvc_session_frame_waiting(&s_session); }

void shim_end(const char *reason) { osvc_session_end(&s_session, reason); }

int shim_terminal_available(void)
{
    return osvc_session_terminal_available(&s_session);
}

int shim_file_available(void) { return osvc_session_file_available(&s_session); }

/* Copies the refusal line into error (empty when it opened). */
static void copy_error(const char *text, char *error, size_t capacity)
{
    if (capacity == 0U) {
        return;
    }
    const size_t length = text != NULL ? strlen(text) : 0U;
    const size_t n = length < capacity - 1U ? length : capacity - 1U;
    if (n > 0U) {
        memcpy(error, text, n);
    }
    error[n] = '\0';
}

uint32_t shim_terminal_open(uint32_t rows, uint32_t cols, char *error,
                            size_t capacity)
{
    uint32_t terminal_id = 0U;
    copy_error(osvc_session_terminal_open(&s_session, rows, cols, &terminal_id),
               error, capacity);
    return terminal_id;
}

int shim_terminal_input(uint32_t terminal_id, const uint8_t *data,
                        size_t length)
{
    return osvc_session_terminal_input(&s_session, terminal_id, data, length);
}

int shim_terminal_resize(uint32_t terminal_id, uint32_t rows, uint32_t cols)
{
    return osvc_session_terminal_resize(&s_session, terminal_id, rows, cols);
}

void shim_terminal_close(uint32_t terminal_id)
{
    osvc_session_terminal_close(&s_session, terminal_id);
}

/* names holds the names one after the other; name_lengths splits them. */
uint32_t shim_file_drop(const uint8_t *names, const uint32_t *name_lengths,
                        const uint64_t *sizes, size_t count,
                        uint32_t *transfer_ids, char *error, size_t capacity)
{
    osvc_file_entry_t *files = calloc(count ? count : 1U, sizeof(*files));
    if (files == NULL) {
        copy_error("shim: out of memory", error, capacity);
        return 0U;
    }
    size_t offset = 0U;
    for (size_t i = 0U; i < count; ++i) {
        files[i].name = (const char *)names + offset;
        files[i].name_length = name_lengths[i];
        files[i].size = sizes[i];
        offset += name_lengths[i];
    }
    uint32_t drop_id = 0U;
    copy_error(osvc_session_file_drop(&s_session, files, count, &drop_id,
                                      transfer_ids),
               error, capacity);
    free(files);
    return drop_id;
}

int shim_file_attach(uint32_t drop_id)
{
    return osvc_session_file_attach(&s_session, drop_id);
}

int shim_file_data(uint32_t drop_id, uint32_t transfer_id, const uint8_t *data,
                   size_t length)
{
    return osvc_session_file_data(&s_session, drop_id, transfer_id, data,
                                  length);
}

int shim_file_cancel(uint32_t drop_id, uint32_t transfer_id)
{
    return osvc_session_file_cancel(&s_session, drop_id, transfer_id);
}

void shim_file_channel_closed(uint32_t drop_id)
{
    osvc_session_file_channel_closed(&s_session, drop_id);
}

void shim_set_backed_up(int backed_up) { s_backed_up = backed_up; }

int shim_service_events_lost(void) { return s_service_events_lost; }

/* Moves up to capacity events, oldest first, to out. */
size_t shim_take_service_events(shim_service_event_t *out, size_t capacity)
{
    const size_t n = s_service_event_count < capacity ? s_service_event_count :
                                                        capacity;
    memcpy(out, s_service_events, n * sizeof(*out));
    memmove(s_service_events, s_service_events + n,
            (s_service_event_count - n) * sizeof(*out));
    s_service_event_count -= n;
    return n;
}

/* Moves the terminal output bytes, in the order of their events. */
size_t shim_take_terminal_bytes(uint8_t *out, size_t capacity)
{
    const size_t n = s_terminal_bytes_length < capacity ?
                         s_terminal_bytes_length : capacity;
    memcpy(out, s_terminal_bytes, n);
    memmove(s_terminal_bytes, s_terminal_bytes + n,
            s_terminal_bytes_length - n);
    s_terminal_bytes_length -= n;
    return n;
}

size_t shim_video_size(void) { return sizeof(g_fake_video); }

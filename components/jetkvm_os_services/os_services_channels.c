/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "os_services_channels.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "os_services_wire.h"

/* A drop's input: the transfer ID and the length before each message. */
#define RECORD_HEADER_LEN 8U
/* Browser bytes the service task relays before it looks at the rest. */
#define SERVICE_BUDGET_BYTES (128U * 1024U)

enum {
    OUTPUT_NONE = 0,
    OUTPUT_DATA,
    OUTPUT_CLOSING,
    OUTPUT_FILE_MESSAGE,
    OUTPUT_REFUSED,
    OUTPUT_CLOSE,
};

/* --- Byte rings ----------------------------------------------------------- */

static size_t ring_free(const osvc_byte_ring_t *r)
{
    return r->capacity - r->used;
}

static void ring_write(osvc_byte_ring_t *r, const uint8_t *data, size_t length)
{
    size_t tail = (r->head + r->used) % r->capacity;
    const size_t first =
        length < r->capacity - tail ? length : r->capacity - tail;
    memcpy(r->data + tail, data, first);
    memcpy(r->data, data + first, length - first);
    r->used += length;
}

/* The readable bytes that follow head without wrapping. */
static size_t ring_peek(const osvc_byte_ring_t *r, const uint8_t **data)
{
    *data = r->data + r->head;
    const size_t contiguous = r->capacity - r->head;
    return r->used < contiguous ? r->used : contiguous;
}

static void ring_read(const osvc_byte_ring_t *r, uint8_t *out, size_t length)
{
    const size_t first =
        length < r->capacity - r->head ? length : r->capacity - r->head;
    memcpy(out, r->data + r->head, first);
    memcpy(out + first, r->data, length - first);
}

static void ring_consume(osvc_byte_ring_t *r, size_t length)
{
    r->head = (r->head + length) % r->capacity;
    r->used -= length;
    if (r->used == 0U) {
        r->head = 0U;
    }
}

static void ring_clear(osvc_byte_ring_t *r)
{
    r->head = 0U;
    r->used = 0U;
}

/* --- Control messages ----------------------------------------------------- */

/* The JSON value that fills text, or NULL. */
static cJSON *parse_json(const uint8_t *text, size_t length)
{
    /* cJSON stops after the value: only whitespace (to cJSON, any byte up to
     * a space) may follow it. */
    while (length > 0U && text[length - 1U] <= ' ') {
        length--;
    }
    const char *end = NULL;
    cJSON *json =
        cJSON_ParseWithLengthOpts((const char *)text, length, &end, false);
    if (end != (const char *)text + length) {
        cJSON_Delete(json);
        json = NULL;
    }
    return json;
}

/* An object's member that is an integer from min to max. */
static bool get_integer(const cJSON *object, const char *name, uint32_t min,
                        uint32_t max, uint32_t *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < min ||
        item->valuedouble > max ||
        item->valuedouble != (uint32_t)item->valuedouble) {
        return false;
    }
    *value = (uint32_t)item->valuedouble;
    return true;
}

bool osvc_channels_parse_size(const uint8_t *text, size_t length,
                              uint32_t *rows, uint32_t *cols)
{
    cJSON *json = parse_json(text, length);
    const bool valid = get_integer(json, "rows", 1U, UINT16_MAX, rows) &&
                       get_integer(json, "cols", 1U, UINT16_MAX, cols);
    cJSON_Delete(json);
    return valid;
}

bool osvc_channels_parse_cancel(const uint8_t *text, size_t length,
                                uint32_t *transfer_id)
{
    cJSON *json = parse_json(text, length);
    const char *type =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json, "type"));
    const bool valid =
        type != NULL && strcmp(type, "cancel") == 0 &&
        get_integer(json, "transfer", 0U, UINT32_MAX, transfer_id);
    cJSON_Delete(json);
    return valid;
}

/* --- The table ---------------------------------------------------------------- */

static void lock(osvc_channels_t *t)
{
    t->platform.lock(t->platform.ctx);
}

static void unlock(osvc_channels_t *t)
{
    t->platform.unlock(t->platform.ctx);
}

static void output_pending(osvc_channels_t *t)
{
    if (t->platform.output_pending != NULL) {
        t->platform.output_pending(t->platform.ctx);
    }
}

static void service_pending(osvc_channels_t *t)
{
    if (t->platform.service_pending != NULL) {
        t->platform.service_pending(t->platform.ctx);
    }
}

static int64_t now_ms(osvc_channels_t *t)
{
    return t->platform.now_ms(t->platform.ctx);
}

void osvc_channels_init(osvc_channels_t *channels,
                        const osvc_channels_platform_t *platform)
{
    memset(channels, 0, sizeof(*channels));
    channels->platform = *platform;
}

static osvc_channel_t *find(osvc_channels_t *t, uint32_t channel)
{
    for (size_t i = 0U; channel != 0U && i < OSVC_CHANNELS_MAX; ++i) {
        osvc_channel_t *c = &t->channels[i];
        if (c->kind != OSVC_CHANNEL_FREE && c->channel == channel) {
            return c;
        }
    }
    return NULL;
}

static void free_memory(osvc_channels_t *t, void *memory)
{
    if (memory != NULL) {
        t->platform.free(t->platform.ctx, memory);
    }
}

/* Under the lock (a mutex: freeing under it is allowed). The slot is freed
 * now, or when the transport gives back the output it holds. */
static void release(osvc_channels_t *t, osvc_channel_t *c)
{
    if (c->output_busy) {
        c->release = true;
        return;
    }
    if (c->kind == OSVC_CHANNEL_FILE) {
        for (size_t i = 0U; i < OSVC_FILE_DROP_MAX; ++i) {
            free_memory(t, c->file->transfers[i].path);
        }
    }
    free_memory(t, c->memory);
    memset(c, 0, sizeof(*c));
}

/* "guestfile_<drop ID>", as the drop's RPC named it: decimal, no leading
 * zero, not 0. */
static bool parse_file_label(const char *label, uint32_t *drop_id)
{
    const size_t prefix = sizeof(OSVC_FILE_CHANNEL_PREFIX) - 1U;
    if (strncmp(label, OSVC_FILE_CHANNEL_PREFIX, prefix) != 0) {
        return false;
    }
    const char *digits = label + prefix;
    if (digits[0] < '1' || digits[0] > '9') {
        return false;
    }
    uint64_t value = 0U;
    for (size_t i = 0U; digits[i] != '\0'; ++i) {
        if (digits[i] < '0' || digits[i] > '9' || i >= 10U) {
            return false;
        }
        value = value * 10U + (uint64_t)(digits[i] - '0');
    }
    if (value > UINT32_MAX) {
        return false;
    }
    *drop_id = (uint32_t)value;
    return true;
}

bool osvc_channels_claim(osvc_channels_t *t, uint32_t channel,
                         const char *label)
{
    if (channel == 0U || label == NULL) {
        return false;
    }
    osvc_channel_kind_t kind;
    uint32_t drop_id = 0U;
    size_t size;
    if (strcmp(label, OSVC_TERMINAL_CHANNEL_LABEL) == 0) {
        kind = OSVC_CHANNEL_TERMINAL;
        size = sizeof(osvc_terminal_channel_t) + OSVC_TERMINAL_INPUT_BYTES +
               OSVC_TERMINAL_OUTPUT_BYTES;
    } else if (parse_file_label(label, &drop_id)) {
        kind = OSVC_CHANNEL_FILE;
        size = sizeof(osvc_file_channel_t) + OSVC_FILE_INPUT_BYTES;
    } else {
        return false;
    }
    uint8_t *memory = t->platform.alloc(t->platform.ctx, size);
    lock(t);
    if (memory == NULL) {
        t->counters.no_memory++;
        if (kind != OSVC_CHANNEL_FILE) {
            unlock(t);
            return false;
        }
    }
    osvc_channel_t *c = NULL;
    for (size_t i = 0U; i < OSVC_CHANNELS_MAX && c == NULL; ++i) {
        if (t->channels[i].kind == OSVC_CHANNEL_FREE) {
            c = &t->channels[i];
        }
    }
    const bool known = find(t, channel) != NULL;
    if (c == NULL || known) {
        t->counters.refused++;
        /* The drop would wait for this channel until its attach timeout,
         * and refuse every new drop meanwhile. */
        const bool end_drop = kind == OSVC_CHANNEL_FILE && !known;
        if (end_drop) {
            t->refused_drop = drop_id;
        }
        unlock(t);
        free_memory(t, memory);
        if (end_drop) {
            service_pending(t);
        }
        return false;
    }
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    c->channel = channel;
    c->memory = memory;
    if (memory == NULL) {
        /* Taken without buffers, to tell the browser that the drop failed. */
        c->kind = OSVC_CHANNEL_REFUSED;
        c->refused_drop = drop_id;
    } else if (kind == OSVC_CHANNEL_TERMINAL) {
        osvc_terminal_channel_t *term = c->terminal;
        memset(term, 0, sizeof(*term));
        term->input.data = memory + sizeof(*term);
        term->input.capacity = OSVC_TERMINAL_INPUT_BYTES;
        term->output.data = term->input.data + OSVC_TERMINAL_INPUT_BYTES;
        term->output.capacity = OSVC_TERMINAL_OUTPUT_BYTES;
    } else {
        osvc_file_channel_t *f = c->file;
        memset(f, 0, sizeof(*f));
        f->drop_id = drop_id;
        f->input.data = memory + sizeof(*f);
        f->input.capacity = OSVC_FILE_INPUT_BYTES;
    }
    t->counters.claimed++;
    unlock(t);
    service_pending(t);
    return true;
}

static void terminal_message(osvc_channels_t *t, osvc_channel_t *c, bool text,
                             const uint8_t *data, size_t length)
{
    osvc_terminal_channel_t *term = c->terminal;
    if (text) {
        /* Keystrokes are binary, so pasted JSON never passes for a size;
         * other text is ignored. */
        uint32_t rows;
        uint32_t cols;
        if (osvc_channels_parse_size(data, length, &rows, &cols)) {
            term->resize = true;
            term->rows = (uint16_t)rows;
            term->cols = (uint16_t)cols;
        }
        return;
    }
    if (c->overflow || c->ending) {
        return;
    }
    if (length > ring_free(&term->input)) {
        c->overflow = true;
        t->counters.input_overflows++;
        return;
    }
    ring_write(&term->input, data, length);
}

static void file_message(osvc_channels_t *t, osvc_channel_t *c, bool text,
                         const uint8_t *data, size_t length)
{
    osvc_file_channel_t *f = c->file;
    if (text) {
        uint32_t transfer_id;
        if (osvc_channels_parse_cancel(data, length, &transfer_id) &&
            transfer_id > f->drop_id &&
            transfer_id - f->drop_id - 1U < OSVC_FILE_DROP_MAX) {
            f->transfers[transfer_id - f->drop_id - 1U].cancel = true;
        }
        return;
    }
    /* A transfer ID and at least one byte. */
    if (length <= 4U || c->overflow || c->ending) {
        return;
    }
    const size_t bytes = length - 4U;
    if (RECORD_HEADER_LEN + bytes > ring_free(&f->input)) {
        /* Only a browser that sends beyond its credit, or in messages under
         * 1 KiB that leave credit unused, gets here. */
        c->overflow = true;
        t->counters.input_overflows++;
        return;
    }
    uint8_t header[RECORD_HEADER_LEN];
    memcpy(header, data, 4U);
    osvc_put_u32(header + 4, (uint32_t)bytes);
    ring_write(&f->input, header, sizeof(header));
    ring_write(&f->input, data + 4, bytes);
    /* Little-endian, as the browser writes it and the wire codec reads. */
    const uint32_t transfer_id = osvc_get_u32(data);
    const uint32_t index = transfer_id - f->drop_id - 1U;
    if (transfer_id > f->drop_id && index < OSVC_FILE_DROP_MAX) {
        osvc_channel_transfer_t *transfer = &f->transfers[index];
        const uint64_t used = bytes < transfer->unused ? bytes : transfer->unused;
        transfer->unused -= used;
        f->outstanding -= used;
    }
}

void osvc_channels_message(osvc_channels_t *t, uint32_t channel, bool text,
                           const uint8_t *data, size_t length)
{
    if (data == NULL || length == 0U) {
        return;
    }
    lock(t);
    osvc_channel_t *c = find(t, channel);
    if (c == NULL || c->closed || c->kind == OSVC_CHANNEL_REFUSED) {
        unlock(t);
        return;
    }
    t->counters.messages_in++;
    t->counters.bytes_in += length;
    if (c->kind == OSVC_CHANNEL_TERMINAL) {
        terminal_message(t, c, text, data, length);
    } else {
        file_message(t, c, text, data, length);
    }
    unlock(t);
    service_pending(t);
}

void osvc_channels_closed(osvc_channels_t *t, uint32_t channel)
{
    lock(t);
    osvc_channel_t *c = find(t, channel);
    if (c != NULL) {
        c->closed = true;
    }
    unlock(t);
    if (c != NULL) {
        service_pending(t);
    }
}

/* --- Output ------------------------------------------------------------------ */

static size_t append(char *out, size_t capacity, size_t o, const char *text)
{
    const size_t length = strlen(text);
    if (o + length >= capacity) {
        return capacity;
    }
    memcpy(out + o, text, length);
    return o + length;
}

/* A path as a JSON string's contents, or capacity when it does not fit. */
static size_t append_escaped(char *out, size_t capacity, size_t o,
                             const char *text, size_t length)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0U; i < length && o < capacity; ++i) {
        const uint8_t ch = (uint8_t)text[i];
        if (ch == '"' || ch == '\\') {
            if (o + 2U >= capacity) {
                return capacity;
            }
            out[o++] = '\\';
            out[o++] = (char)ch;
        } else if (ch < 0x20U) {
            if (o + 6U >= capacity) {
                return capacity;
            }
            memcpy(out + o, "\\u00", 4U);
            out[o + 4U] = hex[ch >> 4];
            out[o + 5U] = hex[ch & 0x0fU];
            o += 6U;
        } else {
            if (o + 1U >= capacity) {
                return capacity;
            }
            out[o++] = (char)ch;
        }
    }
    return o;
}

static size_t compose_credit(char *out, size_t capacity, uint32_t transfer_id,
                             uint64_t bytes)
{
    const int n = snprintf(out, capacity,
                           "{\"type\":\"credit\",\"transfer\":%" PRIu32
                           ",\"bytes\":%" PRIu64 "}",
                           transfer_id, bytes);
    return n > 0 && (size_t)n < capacity ? (size_t)n : 0U;
}

static size_t compose_done(char *out, size_t capacity, uint32_t transfer_id,
                           const osvc_channel_transfer_t *transfer)
{
    const int n = snprintf(
        out, capacity,
        "{\"type\":\"done\",\"transfer\":%" PRIu32 ",\"status\":\"%s\"",
        transfer_id,
        osvc_file_status_name((osvc_file_status_t)transfer->status));
    if (n <= 0 || (size_t)n >= capacity) {
        return 0U;
    }
    size_t o = (size_t)n;
    if (transfer->path != NULL) {
        size_t with_path = append(out, capacity, o, ",\"path\":\"");
        with_path = append_escaped(out, capacity, with_path, transfer->path,
                                   transfer->path_length);
        with_path = append(out, capacity, with_path, "\"");
        if (with_path < capacity) {
            o = with_path;
        }
    }
    o = append(out, capacity, o, "}");
    return o < capacity ? o : 0U;
}

/* The next of the drop's messages, composed into f->message: an outcome, or
 * credit within the window and within what the browser may hold at once. */
static bool file_output(osvc_file_channel_t *f)
{
    const uint64_t used = f->outstanding + f->input.used;
    const uint64_t window =
        used < OSVC_FILE_WINDOW_BYTES ? OSVC_FILE_WINDOW_BYTES - used : 0U;
    const uint64_t flight = f->outstanding < OSVC_FILE_FLIGHT_BYTES ?
                                OSVC_FILE_FLIGHT_BYTES - f->outstanding :
                                0U;
    const uint64_t room = window < flight ? window : flight;
    for (uint32_t k = 0U; k < OSVC_FILE_DROP_MAX; ++k) {
        const uint32_t i = (f->next_transfer + k) % OSVC_FILE_DROP_MAX;
        osvc_channel_transfer_t *transfer = &f->transfers[i];
        const uint32_t transfer_id = f->drop_id + 1U + i;
        if (transfer->done && !transfer->reported) {
            f->message_length = compose_done(f->message, sizeof(f->message),
                                             transfer_id, transfer);
            f->message_transfer = i;
            f->message_done = true;
            f->message_grant = 0U;
            return f->message_length > 0U;
        }
        if (transfer->done || transfer->pending == 0U || room == 0U) {
            continue;
        }
        const uint64_t grant = transfer->pending < room ? transfer->pending : room;
        if (grant < transfer->pending && grant < OSVC_FILE_GRANT_MIN) {
            continue;
        }
        f->message_length = compose_credit(f->message, sizeof(f->message),
                                           transfer_id, grant);
        f->message_transfer = i;
        f->message_done = false;
        f->message_grant = grant;
        return f->message_length > 0U;
    }
    return false;
}

static bool channel_output(osvc_channels_t *t, osvc_channel_t *c,
                           osvc_channel_output_t *out, int *kind)
{
    out->channel = c->channel;
    out->close = false;
    out->text = false;
    if (c->kind == OSVC_CHANNEL_TERMINAL) {
        osvc_terminal_channel_t *term = c->terminal;
        const uint8_t *data;
        size_t length = ring_peek(&term->output, &data);
        if (length > 0U) {
            out->data = data;
            out->length = length < OSVC_CHANNEL_MESSAGE_MAX ?
                              length :
                              OSVC_CHANNEL_MESSAGE_MAX;
            *kind = OUTPUT_DATA;
            return true;
        }
        if (!c->ending) {
            return false;
        }
        if (term->closing_length > 0U && !term->closing_sent) {
            out->text = true;
            out->data = (const uint8_t *)term->closing;
            out->length = term->closing_length;
            *kind = OUTPUT_CLOSING;
            return true;
        }
    } else if (c->kind == OSVC_CHANNEL_REFUSED) {
        if (!c->opened) {
            return false;
        }
        if (c->refused_sent < c->refused_count) {
            const int n = snprintf(
                c->refused_message, sizeof(c->refused_message),
                "{\"type\":\"done\",\"transfer\":%" PRIu32
                ",\"status\":\"%s\"}",
                c->refused_drop + 1U + c->refused_sent,
                osvc_file_status_name(OSVC_FILE_DONE_FAILED));
            c->refused_length =
                n > 0 && (size_t)n < sizeof(c->refused_message) ? (size_t)n : 0U;
            out->text = true;
            out->data = (const uint8_t *)c->refused_message;
            out->length = c->refused_length;
            *kind = OUTPUT_REFUSED;
            return out->length > 0U;
        }
    } else {
        osvc_file_channel_t *f = c->file;
        if (file_output(f)) {
            out->text = true;
            out->data = (const uint8_t *)f->message;
            out->length = f->message_length;
            *kind = OUTPUT_FILE_MESSAGE;
            return true;
        }
        if (!c->ending || now_ms(t) < f->close_at) {
            return false;
        }
    }
    out->close = true;
    out->data = NULL;
    out->length = 0U;
    *kind = OUTPUT_CLOSE;
    return true;
}

bool osvc_channels_next_output(osvc_channels_t *t, osvc_channel_output_t *out)
{
    lock(t);
    for (size_t k = 0U; k < OSVC_CHANNELS_MAX; ++k) {
        const size_t i = (t->next_output + k) % OSVC_CHANNELS_MAX;
        osvc_channel_t *c = &t->channels[i];
        int kind = OUTPUT_NONE;
        if (c->kind == OSVC_CHANNEL_FREE || c->closed || c->close_sent ||
            c->output_busy || c->release ||
            !channel_output(t, c, out, &kind)) {
            continue;
        }
        c->output_busy = true;
        c->output_kind = (uint8_t)kind;
        c->output_length = out->length;
        t->next_output = (i + 1U) % OSVC_CHANNELS_MAX;
        unlock(t);
        return true;
    }
    unlock(t);
    return false;
}

void osvc_channels_output_done(osvc_channels_t *t, uint32_t channel, bool sent)
{
    void *memory = NULL;
    bool more = false;
    bool drained = false;
    lock(t);
    osvc_channel_t *c = find(t, channel);
    if (c == NULL || !c->output_busy) {
        unlock(t);
        return;
    }
    c->output_busy = false;
    if (sent && !c->release) {
        t->counters.messages_out++;
        t->counters.bytes_out += c->output_length;
        switch (c->output_kind) {
        case OUTPUT_DATA:
            ring_consume(&c->terminal->output, c->output_length);
            c->terminal->drained += (uint32_t)c->output_length;
            drained = true;
            break;
        case OUTPUT_CLOSING:
            c->terminal->closing_sent = true;
            break;
        case OUTPUT_FILE_MESSAGE: {
            osvc_file_channel_t *f = c->file;
            osvc_channel_transfer_t *transfer =
                &f->transfers[f->message_transfer];
            if (f->message_done) {
                transfer->reported = true;
                memory = transfer->path;
                transfer->path = NULL;
            } else if (!transfer->done) {
                transfer->pending -= f->message_grant;
                transfer->unused += f->message_grant;
                f->outstanding += f->message_grant;
            }
            /* Credit for a transfer that ended while it was on its way:
             * its bytes are never counted, so it takes no window. */
            f->next_transfer = (f->message_transfer + 1U) % OSVC_FILE_DROP_MAX;
            break;
        }
        case OUTPUT_REFUSED:
            c->refused_sent++;
            break;
        case OUTPUT_CLOSE:
            c->close_sent = true;
            break;
        default:
            break;
        }
        more = true;
    }
    c->output_kind = OUTPUT_NONE;
    if (c->release) {
        /* The service task finished with it while the output was out. */
        release(t, c);
    }
    unlock(t);
    free_memory(t, memory);
    if (more) {
        output_pending(t);
    }
    if (drained) {
        /* The session may hold shell output that fits now. */
        service_pending(t);
    }
}

/* --- The session's hooks ------------------------------------------------------- */

static osvc_channel_t *find_terminal(osvc_channels_t *t, uint32_t terminal_id)
{
    for (size_t i = 0U; terminal_id != 0U && i < OSVC_CHANNELS_MAX; ++i) {
        osvc_channel_t *c = &t->channels[i];
        if (c->kind == OSVC_CHANNEL_TERMINAL && c->opened &&
            c->terminal->live && c->terminal->terminal_id == terminal_id) {
            return c;
        }
    }
    return NULL;
}

static osvc_channel_t *find_drop(osvc_channels_t *t, uint32_t drop_id)
{
    for (size_t i = 0U; drop_id != 0U && i < OSVC_CHANNELS_MAX; ++i) {
        osvc_channel_t *c = &t->channels[i];
        if (c->kind == OSVC_CHANNEL_FILE && c->file->attached &&
            c->file->drop_id == drop_id) {
            return c;
        }
    }
    return NULL;
}

bool osvc_channels_terminal_output(osvc_channels_t *t, uint32_t terminal_id,
                                   const uint8_t *data, size_t length)
{
    lock(t);
    osvc_channel_t *c = find_terminal(t, terminal_id);
    if (c == NULL || c->closed || c->ending || length == 0U) {
        /* The browser has gone: the session closes the terminal when the
         * service task sees the close. */
        unlock(t);
        return true;
    }
    if (length > ring_free(&c->terminal->output)) {
        unlock(t);
        return false;
    }
    ring_write(&c->terminal->output, data, length);
    unlock(t);
    output_pending(t);
    return true;
}

/* Under the lock: the channel sends the line, then closes. */
static void end_terminal_channel(osvc_channel_t *c, const char *reason)
{
    osvc_terminal_channel_t *term = c->terminal;
    term->live = false;
    /* What the browser typed after this is dropped. The input ring is left
     * alone: the service task may be relaying from it. */
    c->ending = true;
    term->closing_length = 0U;
    term->closing_sent = false;
    if (reason != NULL && reason[0] != '\0') {
        /* The reasons are ASCII; a long one is cut. */
        const int n = snprintf(term->closing, sizeof(term->closing),
                               "\r\n[%.*s]\r\n",
                               (int)(sizeof(term->closing) - 7U), reason);
        term->closing_length = n > 0 ? (size_t)n : 0U;
    }
}

void osvc_channels_terminal_closed(osvc_channels_t *t, uint32_t terminal_id,
                                   const char *reason)
{
    lock(t);
    osvc_channel_t *c = find_terminal(t, terminal_id);
    if (c != NULL) {
        end_terminal_channel(c, reason);
    }
    unlock(t);
    if (c != NULL) {
        output_pending(t);
    }
}

void osvc_channels_file_credit(osvc_channels_t *t, uint32_t drop_id,
                               uint32_t transfer_id, uint64_t bytes)
{
    lock(t);
    osvc_channel_t *c = find_drop(t, drop_id);
    const uint32_t index = transfer_id - drop_id - 1U;
    const bool known = c != NULL && transfer_id > drop_id &&
                       index < OSVC_FILE_DROP_MAX;
    if (known && !c->file->transfers[index].done) {
        osvc_channel_transfer_t *transfer = &c->file->transfers[index];
        const uint64_t usable = bytes < transfer->room ? bytes : transfer->room;
        transfer->room -= usable;
        transfer->pending += usable;
    }
    unlock(t);
    if (known) {
        output_pending(t);
    }
}

void osvc_channels_file_done(osvc_channels_t *t, uint32_t drop_id,
                             uint32_t transfer_id, osvc_file_status_t status,
                             const char *path, size_t path_length, bool last)
{
    char *copy = NULL;
    if (path_length > 0U && path_length <= OSVC_FILE_PATH_REPORT_MAX &&
        osvc_utf8_valid((const uint8_t *)path, path_length)) {
        copy = t->platform.alloc(t->platform.ctx, path_length);
        if (copy != NULL) {
            memcpy(copy, path, path_length);
        }
    }
    lock(t);
    osvc_channel_t *c = find_drop(t, drop_id);
    const uint32_t index = transfer_id - drop_id - 1U;
    const bool known = c != NULL && transfer_id > drop_id &&
                       index < OSVC_FILE_DROP_MAX;
    if (known) {
        osvc_channel_transfer_t *transfer = &c->file->transfers[index];
        transfer->done = true;
        transfer->pending = 0U;
        /* Credit it left unused goes back to the window. */
        c->file->outstanding -= transfer->unused;
        transfer->unused = 0U;
        transfer->status = (uint8_t)status;
        if (transfer->path == NULL && copy != NULL) {
            transfer->path = copy;
            transfer->path_length = (uint16_t)path_length;
            copy = NULL;
        }
        if (last) {
            /* Let the outcomes leave before the channel closes. */
            c->ending = true;
            c->file->close_at = now_ms(t) + OSVC_FILE_CLOSE_DELAY_MS;
        }
    }
    unlock(t);
    free_memory(t, copy);
    if (known) {
        output_pending(t);
    }
}

/* --- The service task --------------------------------------------------------- */

static void open_channel(osvc_channels_t *t, osvc_channel_t *c,
                         osvc_session_t *session)
{
    if (c->kind == OSVC_CHANNEL_TERMINAL) {
        osvc_terminal_channel_t *term = c->terminal;
        /* A size that came with the open goes with terminal.open. */
        const uint32_t rows = term->resize ? term->rows : 0U;
        const uint32_t cols = term->resize ? term->cols : 0U;
        term->resize = false;
        unlock(t);
        uint32_t terminal_id = 0U;
        const char *reason =
            osvc_session_terminal_open(session, rows, cols, &terminal_id);
        lock(t);
        c->opened = true;
        if (reason != NULL) {
            end_terminal_channel(c, reason);
        } else {
            term->terminal_id = terminal_id;
            term->live = true;
        }
        return;
    }
    osvc_file_channel_t *f = c->file;
    /* The sizes bound the credit worth giving. A read of the session's
     * state, not a call into its effects: it neither blocks nor calls a
     * hook. */
    for (uint32_t i = 0U; i < OSVC_FILE_DROP_MAX; ++i) {
        uint64_t size;
        f->transfers[i].room =
            osvc_session_file_size(session, f->drop_id, f->drop_id + 1U + i,
                                   &size) ?
                size :
                UINT64_MAX;
    }
    /* The session reports the drop's held credit and outcomes from inside
     * the attach, to this channel. */
    f->attached = true;
    c->opened = true;
    unlock(t);
    const bool attached = osvc_session_file_attach(session, f->drop_id);
    lock(t);
    if (!attached) {
        /* An unknown drop, or one with a channel already: closed at once,
         * without a word. */
        f->attached = false;
        c->ending = true;
        f->close_at = now_ms(t);
    }
}

/* The browser closed the channel: the session forgets its side. */
static void close_channel(osvc_channels_t *t, osvc_channel_t *c,
                          osvc_session_t *session)
{
    if (c->kind == OSVC_CHANNEL_TERMINAL) {
        osvc_terminal_channel_t *term = c->terminal;
        const uint32_t terminal_id = term->live ? term->terminal_id : 0U;
        term->live = false;
        if (terminal_id != 0U) {
            unlock(t);
            osvc_session_terminal_close(session, terminal_id);
            lock(t);
        }
    } else {
        osvc_file_channel_t *f = c->file;
        const uint32_t drop_id = f->attached ? f->drop_id : 0U;
        f->attached = false;
        if (drop_id != 0U) {
            unlock(t);
            osvc_session_file_channel_closed(session, drop_id);
            lock(t);
        } else if (!c->opened) {
            /* Closed before it was attached: the drop is over. */
            const uint32_t unattached = f->drop_id;
            unlock(t);
            (void)osvc_session_file_end_unattached(
                session, unattached, OSVC_FILE_DONE_CANCELLED);
            lock(t);
        }
    }
    release(t, c);
}

/* A drop's channel taken without buffers: the drop fails at once, and the
 * channel's outcomes go out before its close. Returns true when the
 * transport has output. */
static bool refused_channel(osvc_channels_t *t, osvc_channel_t *c,
                            osvc_session_t *session)
{
    bool output = false;
    if (!c->opened) {
        const uint32_t drop_id = c->refused_drop;
        const osvc_file_status_t status = c->closed ?
            OSVC_FILE_DONE_CANCELLED : OSVC_FILE_DONE_FAILED;
        unlock(t);
        const uint32_t count =
            osvc_session_file_end_unattached(session, drop_id, status);
        lock(t);
        /* Published together: the transport sends nothing for the channel
         * until it is opened, and then every outcome before the close. */
        c->refused_count = count;
        c->opened = true;
        c->ending = true;
        output = true;
    }
    if (c->closed) {
        release(t, c);
    }
    return output;
}

static void handle_overflow(osvc_channels_t *t, osvc_channel_t *c,
                            osvc_session_t *session)
{
    c->overflow = false;
    if (c->kind == OSVC_CHANNEL_TERMINAL) {
        osvc_terminal_channel_t *term = c->terminal;
        const uint32_t terminal_id = term->live ? term->terminal_id : 0U;
        /* A gap would change what the shell runs. */
        end_terminal_channel(c, OSVC_TERMINAL_INPUT_LOST);
        if (terminal_id != 0U) {
            unlock(t);
            osvc_session_terminal_close(session, terminal_id);
            lock(t);
        }
        return;
    }
    osvc_file_channel_t *f = c->file;
    const uint32_t drop_id = f->attached ? f->drop_id : 0U;
    /* The bytes after the lost ones would land at the wrong offset. */
    ring_clear(&f->input);
    if (drop_id != 0U) {
        unlock(t);
        osvc_session_file_fail(session, drop_id);
        lock(t);
    }
    if (!c->ending) {
        /* A drop that was already over: nothing is left to report. */
        c->ending = true;
        f->close_at = now_ms(t) + OSVC_FILE_CLOSE_DELAY_MS;
    }
}

/* Returns the output the browser took to the sender, a step at a time. */
static void terminal_credit(osvc_channels_t *t, osvc_terminal_channel_t *term,
                            osvc_session_t *session)
{
    if (!term->live || term->drained < OSVC_TERMINAL_CREDIT_STEP) {
        return;
    }
    const uint32_t terminal_id = term->terminal_id;
    const uint32_t bytes = term->drained;
    term->drained = 0U;
    unlock(t);
    /* A failed write ends the session, and the credit with it. */
    (void)osvc_session_terminal_credit(session, terminal_id, bytes);
    lock(t);
}

/* Relays up to *budget of a terminal's keystrokes. */
static void terminal_input(osvc_channels_t *t, osvc_channel_t *c,
                           osvc_session_t *session, size_t *budget)
{
    osvc_terminal_channel_t *term = c->terminal;
    terminal_credit(t, term, session);
    if (term->resize && term->live) {
        const uint32_t terminal_id = term->terminal_id;
        const uint32_t rows = term->rows;
        const uint32_t cols = term->cols;
        term->resize = false;
        unlock(t);
        (void)osvc_session_terminal_resize(session, terminal_id, rows, cols);
        lock(t);
    }
    while (*budget > 0U && !c->closed && !c->ending && !c->overflow &&
           term->live) {
        const uint8_t *data;
        size_t length = ring_peek(&term->input, &data);
        if (length == 0U) {
            return;
        }
        length = length < *budget ? length : *budget;
        const uint32_t terminal_id = term->terminal_id;
        unlock(t);
        /* A failed write ends the terminal (terminal_closed) and the
         * session. */
        (void)osvc_session_terminal_input(session, terminal_id, data, length);
        lock(t);
        ring_consume(&term->input, length);
        *budget -= length;
    }
}

/* Relays up to *budget of a drop's bytes, and the browser's cancels. */
static void file_input(osvc_channels_t *t, osvc_channel_t *c,
                       osvc_session_t *session, size_t *budget)
{
    osvc_file_channel_t *f = c->file;
    const uint32_t drop_id = f->drop_id;
    for (uint32_t i = 0U; i < OSVC_FILE_DROP_MAX && f->attached; ++i) {
        if (!f->transfers[i].cancel) {
            continue;
        }
        f->transfers[i].cancel = false;
        unlock(t);
        (void)osvc_session_file_cancel(session, drop_id, drop_id + 1U + i);
        lock(t);
    }
    bool relayed = false;
    while (*budget > 0U && !c->closed && !c->ending && !c->overflow &&
           f->attached && f->input.used >= RECORD_HEADER_LEN) {
        uint8_t header[RECORD_HEADER_LEN];
        ring_read(&f->input, header, sizeof(header));
        const uint32_t transfer_id = osvc_get_u32(header);
        const uint32_t length = osvc_get_u32(header + 4);
        ring_consume(&f->input, sizeof(header));
        /* One record at a time; it may wrap, and then goes in two parts. */
        for (uint32_t done = 0U; done < length;) {
            const uint8_t *data;
            size_t n = ring_peek(&f->input, &data);
            n = n < length - done ? n : length - done;
            unlock(t);
            (void)osvc_session_file_data(session, drop_id, transfer_id, data, n);
            lock(t);
            ring_consume(&f->input, n);
            done += (uint32_t)n;
        }
        *budget = length < *budget ? *budget - length : 0U;
        relayed = true;
    }
    if (relayed) {
        /* The window has room again. */
        unlock(t);
        output_pending(t);
        lock(t);
    }
}

bool osvc_channels_service(osvc_channels_t *t, osvc_session_t *session)
{
    size_t budget = SERVICE_BUDGET_BYTES;
    bool output = false;
    lock(t);
    if (t->refused_drop != 0U) {
        const uint32_t drop_id = t->refused_drop;
        t->refused_drop = 0U;
        unlock(t);
        (void)osvc_session_file_end_unattached(session, drop_id,
                                               OSVC_FILE_DONE_FAILED);
        lock(t);
    }
    /* Each call starts at the next channel, so a busy one cannot starve the
     * others of the budget. */
    const size_t start = t->next_service;
    t->next_service = (start + 1U) % OSVC_CHANNELS_MAX;
    for (size_t k = 0U; k < OSVC_CHANNELS_MAX; ++k) {
        osvc_channel_t *c = &t->channels[(start + k) % OSVC_CHANNELS_MAX];
        if (c->kind == OSVC_CHANNEL_FREE || c->release) {
            continue;
        }
        if (c->kind == OSVC_CHANNEL_REFUSED) {
            output = refused_channel(t, c, session) || output;
            continue;
        }
        if (!c->opened && !c->closed) {
            open_channel(t, c, session);
        }
        if (c->closed) {
            close_channel(t, c, session);
            continue;
        }
        if (c->overflow) {
            handle_overflow(t, c, session);
        }
        if (c->ending) {
            continue;
        }
        if (c->kind == OSVC_CHANNEL_TERMINAL) {
            terminal_input(t, c, session, &budget);
        } else {
            file_input(t, c, session, &budget);
        }
    }
    bool more = false;
    for (size_t i = 0U; i < OSVC_CHANNELS_MAX && !more; ++i) {
        const osvc_channel_t *c = &t->channels[i];
        if (c->kind == OSVC_CHANNEL_FREE || c->release) {
            continue;
        }
        if (c->kind == OSVC_CHANNEL_REFUSED) {
            more = !c->opened || c->closed;
            continue;
        }
        const osvc_byte_ring_t *input = c->kind == OSVC_CHANNEL_TERMINAL ?
                                            &c->terminal->input :
                                            &c->file->input;
        more = c->closed || (!c->opened) || c->overflow ||
               (!c->ending && input->used > 0U);
    }
    unlock(t);
    if (output) {
        output_pending(t);
    }
    return more;
}

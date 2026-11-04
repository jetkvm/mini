/* The peer task's OS Services channel table: channel IDs, claims, routing,
 * closes from either side, and the send phase's budget, full buffers and
 * failed sends. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "guest_channels.h"

/* --- The handler (OS Services) -------------------------------------------- */

#define EVENTS_MAX 256U
#define OUTPUTS_MAX 32U

typedef enum { EV_CLAIM, EV_MESSAGE, EV_CLOSED, EV_DONE } event_kind_t;

typedef struct {
    event_kind_t kind;
    uint32_t channel;
    bool flag; /* text for a message, sent for a done */
    size_t length;
    char label[32];
} event_t;

static struct {
    bool refuse;
    event_t events[EVENTS_MAX];
    size_t count;
    /* Outputs in order; the head is given until it is sent. */
    webrtc_transport_guest_output_t outputs[OUTPUTS_MAX];
    size_t head, tail;
    bool busy;
    /* An endless run of empty messages for this channel. */
    uint32_t endless;
} h;

static event_t *record(event_kind_t kind, uint32_t channel)
{
    assert(h.count < EVENTS_MAX);
    event_t *e = &h.events[h.count++];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->channel = channel;
    return e;
}

static bool claim(uint32_t channel, const char *label)
{
    event_t *e = record(EV_CLAIM, channel);
    snprintf(e->label, sizeof(e->label), "%s", label);
    return !h.refuse;
}

static void message(uint32_t channel, bool text, const uint8_t *data, size_t length)
{
    assert(data != NULL);
    event_t *e = record(EV_MESSAGE, channel);
    e->flag = text;
    e->length = length;
}

static void closed(uint32_t channel)
{
    record(EV_CLOSED, channel);
}

static bool next_output(webrtc_transport_guest_output_t *output)
{
    assert(!h.busy);
    if (h.endless != 0U) {
        *output = (webrtc_transport_guest_output_t){ .channel = h.endless, .data = (const uint8_t *)"" };
        h.busy = true;
        return true;
    }
    if (h.head == h.tail) {
        return false;
    }
    *output = h.outputs[h.head];
    h.busy = true;
    return true;
}

static void output_done(uint32_t channel, bool sent)
{
    assert(h.busy);
    h.busy = false;
    event_t *e = record(EV_DONE, channel);
    e->flag = sent;
    if (sent && h.endless == 0U) {
        assert(h.outputs[h.head].channel == channel);
        h.head++;
    }
}

static void queue(uint32_t channel, bool close, bool text, const char *data, size_t length)
{
    assert(h.tail < OUTPUTS_MAX);
    h.outputs[h.tail++] = (webrtc_transport_guest_output_t){
        .channel = channel, .close = close, .text = text,
        .data = (const uint8_t *)data, .length = length,
    };
}

static const webrtc_transport_guest_channels_t handler = {
    .claim = claim, .message = message, .closed = closed,
    .next_output = next_output, .output_done = output_done,
};

static void reset_handler(void)
{
    memset(&h, 0, sizeof(h));
}

static size_t count(event_kind_t kind, uint32_t channel)
{
    size_t n = 0U;
    for (size_t i = 0U; i < h.count; ++i) {
        n += h.events[i].kind == kind && h.events[i].channel == channel;
    }
    return n;
}

/* --- The peer -------------------------------------------------------------- */

static guest_channels_t table;

static struct {
    guest_send_result_t result;
    unsigned sends;
    uint16_t last_stream;
    bool last_text;
    size_t bytes;
    unsigned closes;
    uint16_t closed_stream;
    /* The vendor reports the close inside its close call. */
    bool reports_close;
} peer_state;

static guest_send_result_t peer_send(void *ctx, uint16_t stream_id, bool text,
                                     const uint8_t *data, size_t length)
{
    assert(ctx == &peer_state && data != NULL && length > 0U);
    peer_state.sends++;
    peer_state.last_stream = stream_id;
    peer_state.last_text = text;
    if (peer_state.result == GUEST_SEND_OK) {
        peer_state.bytes += length;
    }
    return peer_state.result;
}

static void peer_close(void *ctx, uint16_t stream_id)
{
    assert(ctx == &peer_state);
    peer_state.closes++;
    peer_state.closed_stream = stream_id;
    if (peer_state.reports_close) {
        assert(guest_channels_closed(&table, &handler, stream_id));
    }
}

static const guest_peer_t peer = { .send = peer_send, .close = peer_close, .ctx = &peer_state };

static void start(void)
{
    memset(&table, 0, sizeof(table));
    memset(&peer_state, 0, sizeof(peer_state));
    reset_handler();
}

/* Opens a channel on stream and returns the ID the handler was given. */
static uint32_t open_channel(uint16_t stream, const char *label)
{
    assert(guest_channels_open(&table, &handler, stream, label));
    /* The claim is the last call; a stale channel's close may precede it. */
    const event_t *e = &h.events[h.count - 1U];
    assert(e->kind == EV_CLAIM && strcmp(e->label, label) == 0);
    return e->channel;
}

/* --- Tests ----------------------------------------------------------------- */

static void test_labels(void)
{
    assert(guest_channels_label("guest-terminal"));
    assert(guest_channels_label("guestfile_1"));
    /* The handler checks the drop; anything after the prefix reaches it. */
    assert(guest_channels_label("guestfile_"));
    assert(!guest_channels_label("guest-terminal2"));
    assert(!guest_channels_label("Guest-terminal"));
    assert(!guest_channels_label("guestfile"));
    assert(!guest_channels_label("rpc"));
    assert(!guest_channels_label(""));
    assert(!guest_channels_label(NULL));
    puts("guest labels: OK");
}

static void test_claims(void)
{
    start();
    uint32_t ids[GUEST_CHANNELS_MAX];
    for (size_t i = 0U; i < GUEST_CHANNELS_MAX; ++i) {
        ids[i] = open_channel((uint16_t)(10U + 2U * i), i < 4U ? "guest-terminal" : "guestfile_3");
        assert(ids[i] != 0U);
        for (size_t j = 0U; j < i; ++j) assert(ids[j] != ids[i]);
    }
    /* A seventh channel is refused without asking the handler. */
    const size_t before = h.count;
    assert(!guest_channels_open(&table, &handler, 30U, "guest-terminal"));
    assert(h.count == before && table.counters.refused == 1U);

    /* The browser closes one: closed once, and its stream is unknown. */
    assert(guest_channels_closed(&table, &handler, 12U));
    assert(count(EV_CLOSED, ids[1]) == 1U);
    assert(!guest_channels_closed(&table, &handler, 12U));
    assert(!guest_channels_message(&table, &handler, 12U, false, (const uint8_t *)"x", 1U));
    assert(count(EV_CLOSED, ids[1]) == 1U);

    /* Its stream opens again with a new ID that was never given out. */
    const uint32_t again = open_channel(12U, "guest-terminal");
    for (size_t i = 0U; i < GUEST_CHANNELS_MAX; ++i) assert(again != ids[i]);

    /* The handler can refuse; nothing is held for the stream. */
    assert(guest_channels_closed(&table, &handler, 12U));
    h.refuse = true;
    assert(!guest_channels_open(&table, &handler, 12U, "guestfile_9"));
    assert(table.counters.refused == 2U);
    assert(!guest_channels_message(&table, &handler, 12U, true, (const uint8_t *)"{}", 2U));
    h.refuse = false;

    /* Without a handler, or with another label, nothing is claimed. */
    const webrtc_transport_guest_channels_t none = { 0 };
    assert(!guest_channels_open(&table, &none, 12U, "guest-terminal"));
    assert(!guest_channels_open(&table, &handler, 12U, "rpc"));
    assert(table.counters.refused == 4U);

    /* A stream that opens while we still hold it closed unnoticed: the old
     * channel is closed first. */
    const uint32_t replaced = open_channel(10U, "guestfile_4");
    assert(count(EV_CLOSED, ids[0]) == 1U && replaced != ids[0]);

    /* The peer goes away: every channel closes once. */
    guest_channels_reset(&table, &handler);
    assert(count(EV_CLOSED, replaced) == 1U);
    for (size_t i = 2U; i < GUEST_CHANNELS_MAX; ++i) assert(count(EV_CLOSED, ids[i]) == 1U);
    guest_channels_reset(&table, &handler);
    assert(count(EV_CLOSED, replaced) == 1U);
    for (size_t i = 0U; i < GUEST_CHANNELS_MAX; ++i) assert(table.slots[i].channel == 0U);
    /* IDs keep counting across peers. */
    assert(open_channel(10U, "guest-terminal") > replaced);
    puts("guest claims, closes, refusals and peer teardown: OK");
}

static void test_messages(void)
{
    start();
    const uint32_t terminal = open_channel(4U, "guest-terminal");
    const uint32_t file = open_channel(6U, "guestfile_1");
    assert(guest_channels_message(&table, &handler, 4U, false, (const uint8_t *)"ls\r", 3U));
    assert(guest_channels_message(&table, &handler, 6U, true, (const uint8_t *)"{\"type\":\"cancel\"}", 17U));
    assert(!guest_channels_message(&table, &handler, 8U, false, (const uint8_t *)"x", 1U));
    const event_t *a = &h.events[2], *b = &h.events[3];
    assert(a->kind == EV_MESSAGE && a->channel == terminal && !a->flag && a->length == 3U);
    assert(b->kind == EV_MESSAGE && b->channel == file && b->flag && b->length == 17U);
    assert(table.counters.messages_in == 2U && table.counters.bytes_in == 20U);
    puts("guest messages reach their channel, text and binary apart: OK");
}

static void test_send(void)
{
    static char big[GUEST_CHANNELS_SEND_BUDGET];
    start();
    const uint32_t terminal = open_channel(4U, "guest-terminal");
    const uint32_t file = open_channel(6U, "guestfile_1");
    queue(terminal, false, false, "hello", 5U);
    queue(file, false, true, "{\"type\":\"credit\"}", 17U);
    assert(!guest_channels_send(&table, &handler, &peer, GUEST_CHANNELS_SEND_BUDGET));
    assert(peer_state.sends == 2U && peer_state.bytes == 22U);
    assert(peer_state.last_stream == 6U && peer_state.last_text);
    assert(count(EV_DONE, terminal) == 1U && count(EV_DONE, file) == 1U);

    /* A full buffer: the output is handed back and comes again. */
    queue(terminal, false, false, "world", 5U);
    peer_state.result = GUEST_SEND_BLOCKED;
    assert(guest_channels_send(&table, &handler, &peer, GUEST_CHANNELS_SEND_BUDGET));
    assert(!h.events[h.count - 1U].flag && h.head == 2U && table.counters.blocked == 1U);
    peer_state.result = GUEST_SEND_OK;
    assert(!guest_channels_send(&table, &handler, &peer, GUEST_CHANNELS_SEND_BUDGET));
    assert(h.head == 3U && peer_state.bytes == 27U && peer_state.last_stream == 4U);

    /* The budget: a phase stops once it has sent that much. */
    queue(terminal, false, false, big, sizeof(big) / 2U);
    queue(terminal, false, false, big, sizeof(big) / 2U);
    queue(terminal, false, false, "!", 1U);
    assert(guest_channels_send(&table, &handler, &peer, GUEST_CHANNELS_SEND_BUDGET));
    assert(h.head == 5U);
    assert(!guest_channels_send(&table, &handler, &peer, GUEST_CHANNELS_SEND_BUDGET));
    assert(h.head == 6U && table.counters.messages_out == 6U);

    /* Short messages: each also counts its fixed cost, so a phase sends 7
     * of 300 bytes, not all 12. */
    for (int i = 0; i < 12; ++i) {
        queue(terminal, false, false, big, 300U);
    }
    assert(guest_channels_send(&table, &handler, &peer, GUEST_CHANNELS_SEND_BUDGET));
    assert(h.head == 13U);
    assert(!guest_channels_send(&table, &handler, &peer, GUEST_CHANNELS_SEND_BUDGET));
    assert(h.head == 18U && table.counters.messages_out == 18U);

    /* A close: done first, then the stream closes, and the peer reports it. */
    peer_state.reports_close = true;
    queue(file, true, false, NULL, 0U);
    assert(!guest_channels_send(&table, &handler, &peer, GUEST_CHANNELS_SEND_BUDGET));
    assert(peer_state.closes == 1U && peer_state.closed_stream == 6U);
    assert(h.events[h.count - 2U].kind == EV_DONE && h.events[h.count - 2U].flag);
    assert(h.events[h.count - 1U].kind == EV_CLOSED && h.events[h.count - 1U].channel == file);
    assert(count(EV_CLOSED, file) == 1U);

    /* A failed send ends the channel; a peer that does not report the close
     * still gets it closed, once. */
    peer_state.reports_close = false;
    peer_state.result = GUEST_SEND_FAILED;
    queue(terminal, false, false, "lost", 4U);
    assert(!guest_channels_send(&table, &handler, &peer, GUEST_CHANNELS_SEND_BUDGET));
    assert(peer_state.closes == 2U && peer_state.closed_stream == 4U);
    assert(count(EV_CLOSED, terminal) == 1U && table.counters.send_failures == 1U);
    peer_state.result = GUEST_SEND_OK;

    /* Output for a channel that already closed is done without a send. */
    const unsigned sends = peer_state.sends;
    queue(terminal, false, false, "late", 4U);
    queue(terminal, true, false, NULL, 0U);
    assert(!guest_channels_send(&table, &handler, &peer, GUEST_CHANNELS_SEND_BUDGET));
    assert(peer_state.sends == sends && peer_state.closes == 2U && h.head == h.tail);
    assert(count(EV_CLOSED, terminal) == 1U);

    /* An empty message is done without a send, and a phase ends after a
     * bounded number of outputs. */
    const uint32_t again = open_channel(4U, "guest-terminal");
    h.endless = again;
    assert(guest_channels_send(&table, &handler, &peer, GUEST_CHANNELS_SEND_BUDGET));
    assert(count(EV_DONE, again) == GUEST_CHANNELS_SEND_OUTPUTS_MAX && peer_state.sends == sends);
    h.endless = 0U;

    /* No handler, nothing to send. */
    const webrtc_transport_guest_channels_t none = { 0 };
    assert(!guest_channels_send(&table, &none, &peer, GUEST_CHANNELS_SEND_BUDGET));
    puts("guest send phase: order, budget, full buffer, closes and failures: OK");
}

int main(void)
{
    test_labels();
    test_claims();
    test_messages();
    test_send();
    return 0;
}

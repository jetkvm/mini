#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "data_channel_queue.h"
#include "reboot_flow.h"

static void test_single_exact_slot(void)
{
    webrtc_reboot_slot_t slot = {0};
    assert(webrtc_reboot_slot_accept(&slot, true, 7U, 11U, 2U));
    assert(slot.occupied);
    assert(!slot.dispatch_ready);
    assert(slot.force);
    assert(webrtc_reboot_slot_matches(&slot, 7U, 11U, 2U));
    assert(!webrtc_reboot_slot_matches(&slot, 7U, 12U, 2U));

    /* There is no FIFO: even a different peer is busy while this is owned. */
    assert(!webrtc_reboot_slot_accept(&slot, false, 8U, 12U, 4U));
    assert(!webrtc_reboot_slot_mark_dispatch_ready(&slot, 7U, 12U, 2U));
    assert(webrtc_reboot_slot_mark_dispatch_ready(&slot, 7U, 11U, 2U));
    assert(slot.dispatch_ready);
    assert(!webrtc_reboot_slot_mark_dispatch_ready(&slot, 7U, 11U, 2U));

    webrtc_reboot_slot_clear(&slot);
    assert(!slot.occupied);
    assert(webrtc_reboot_slot_accept(&slot, false, 8U, 12U, 4U));
    assert(!slot.force);

    /* A generation race cancels ownership before dispatch and cannot release
     * the worker using either the stale or a replacement generation. */
    assert(!webrtc_reboot_slot_cancel_generation(&slot, 11U));
    assert(webrtc_reboot_slot_cancel_generation(&slot, 12U));
    assert(!webrtc_reboot_slot_mark_dispatch_ready(&slot, 8U, 12U, 4U));
    assert(!webrtc_reboot_slot_matches(&slot, 8U, 12U, 4U));
}

/* Once the response and willReboot are dispatched, the browser was told the
 * Mini restarts: a closing peer or channel does not cancel the reboot. */
static void test_dispatched_reboot_survives_cancel(void)
{
    webrtc_reboot_slot_t slot = {0};
    assert(webrtc_reboot_slot_accept(&slot, false, 7U, 11U, 2U));
    assert(webrtc_reboot_slot_mark_dispatch_ready(&slot, 7U, 11U, 2U));
    assert(!webrtc_reboot_slot_cancel_generation(&slot, 11U));
    assert(!webrtc_reboot_slot_cancel(&slot));
    assert(webrtc_reboot_slot_matches(&slot, 7U, 11U, 2U));
    assert(slot.dispatch_ready);
    /* A later request is still busy. */
    assert(!webrtc_reboot_slot_accept(&slot, false, 8U, 12U, 4U));

    webrtc_reboot_slot_clear(&slot);
    assert(webrtc_reboot_slot_accept(&slot, false, 8U, 12U, 4U));
    assert(webrtc_reboot_slot_cancel(&slot));
    assert(!slot.occupied);
}

/* Runs the reboot task's wait as the task does: it sleeps what
 * webrtc_reboot_wait_us() returns until that is 0. Returns the time after
 * the dispatch at the restart; the response and willReboot leave the send
 * queue at leave_us (-1: never). */
static int64_t restart_at_us(int64_t leave_us)
{
    webrtc_reboot_slot_t slot = {0};
    assert(webrtc_reboot_slot_accept(&slot, false, 7U, 11U, 2U));
    assert(webrtc_reboot_slot_mark_dispatch_ready(&slot, 7U, 11U, 2U));
    const int64_t dispatched_us = 5000LL;
    slot.dispatched_us = dispatched_us;
    slot.sent_us = dispatched_us;
    int64_t now_us = dispatched_us;
    for (;;) {
        bool wait = leave_us < 0 || now_us - dispatched_us < leave_us;
        webrtc_reboot_slot_messages_wait(&slot, wait, now_us);
        int64_t wait_us = webrtc_reboot_wait_us(&slot, now_us);
        if (wait_us == 0) {
            return now_us - dispatched_us;
        }
        assert(wait_us > 0);
        now_us += wait_us;
        assert(now_us - dispatched_us < 60LL * 1000LL * 1000LL);
    }
}

static void test_reboot_wait(void)
{
    /* Nothing waits: the restart comes the settle time after the dispatch. */
    assert(restart_at_us(0) == WEBRTC_REBOOT_SETTLE_US);
    /* The messages leave the queue after a stall: the full settle time
     * follows from then on, so the browser can still get them. */
    const int64_t leave_us = WEBRTC_REBOOT_SETTLE_US + 300LL * 1000LL;
    int64_t restart_us = restart_at_us(leave_us);
    assert(restart_us >= leave_us + WEBRTC_REBOOT_SETTLE_US &&
           restart_us <= leave_us + WEBRTC_REBOOT_POLL_US +
                             WEBRTC_REBOOT_SETTLE_US);
    /* A browser that never acknowledges: the restart still comes, at the
     * limit. */
    assert(restart_at_us(-1) == WEBRTC_REBOOT_LIMIT_US);
}

/* A queued response or willReboot that is lost cancels the dispatched
 * reboot; other lost messages do not. */
static void test_lost_reboot_message_cancels(void)
{
    webrtc_reboot_slot_t slot = {0};
    assert(webrtc_reboot_slot_accept(&slot, false, 7U, 11U, 2U));
    /* Not dispatched yet: the send result itself decides. */
    assert(!webrtc_reboot_slot_message_lost(&slot, 2U, 5U));
    assert(webrtc_reboot_slot_mark_dispatch_ready(&slot, 7U, 11U, 2U));
    slot.first_queued = 5U;
    slot.last_queued = 6U;
    assert(!webrtc_reboot_slot_message_lost(&slot, 3U, 5U));
    assert(!webrtc_reboot_slot_message_lost(&slot, 2U, 4U));
    assert(!webrtc_reboot_slot_message_lost(&slot, 2U, 7U));
    assert(slot.dispatch_ready);
    assert(webrtc_reboot_slot_message_lost(&slot, 2U, 6U));
    assert(!slot.occupied && !slot.dispatch_ready);
}

/* A send buffer that loses the reboot's response (the message starting with
 * 'r') and takes everything else. */
typedef struct {
    webrtc_reboot_slot_t slot;
    data_channel_queue_t queue;
    size_t room;
    unsigned sent;
    unsigned will_reboot_sent;
} reboot_peer_t;

static data_channel_send_result_t reboot_peer_send(
    void *arg, uint16_t stream_id, bool text, const uint8_t *data,
    size_t size)
{
    reboot_peer_t *peer = arg;
    (void)stream_id;
    (void)text;
    if (size > peer->room) {
        return DATA_CHANNEL_BLOCKED;
    }
    if (data[0] == 'r') {
        return DATA_CHANNEL_FAILED;
    }
    if (data[0] == 'w') {
        peer->will_reboot_sent++;
    }
    peer->sent++;
    return DATA_CHANNEL_SENT;
}

/* As queued_message_lost() in webrtc_data_channels.c does. */
static void reboot_peer_lost(void *arg, uint16_t stream_id, uint32_t number)
{
    reboot_peer_t *peer = arg;
    const uint32_t last_queued = peer->slot.last_queued;
    if (webrtc_reboot_slot_message_lost(&peer->slot, stream_id, number)) {
        data_channel_queue_drop_through(&peer->queue, stream_id,
                                        last_queued);
    }
}

static void reboot_peer_queue(reboot_peer_t *peer, const char *text)
{
    assert(data_channel_queue_send(&peer->queue, reboot_peer_send, peer, 2U,
                                   true, (const uint8_t *)text,
                                   strlen(text)) == DATA_CHANNEL_QUEUED);
}

/* The response is lost and willReboot would go: the browser must not get
 * willReboot, and the Mini does not restart. Later messages still go. */
static void test_lost_response_drops_will_reboot(void)
{
    reboot_peer_t peer = {0};
    assert(webrtc_reboot_slot_accept(&peer.slot, false, 7U, 11U, 2U));
    peer.slot.first_queued = peer.queue.queued + 1U;
    reboot_peer_queue(&peer, "response");
    reboot_peer_queue(&peer, "willReboot");
    peer.slot.last_queued = peer.queue.queued;
    reboot_peer_queue(&peer, "later");
    assert(webrtc_reboot_slot_mark_dispatch_ready(&peer.slot, 7U, 11U, 2U));

    peer.room = 100U;
    assert(data_channel_queue_drain(&peer.queue, reboot_peer_send,
                                    reboot_peer_lost, &peer) == 1U);
    assert(peer.will_reboot_sent == 0U && peer.sent == 1U);
    assert(!peer.slot.occupied && !peer.slot.dispatch_ready);
    assert(peer.queue.head == NULL && peer.queue.bytes == 0U);
}

static void test_will_reboot_golden(void)
{
    const char expected[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"willReboot\",\"params\":null}";
    char output[96];
    size_t output_size = 99U;
    assert(webrtc_reboot_render_will_event(
        output, sizeof(output), &output_size));
    assert(output_size == strlen(expected));
    assert(strcmp(output, expected) == 0);

    char short_output[8];
    output_size = 99U;
    assert(!webrtc_reboot_render_will_event(
        short_output, sizeof(short_output), &output_size));
    assert(output_size == 0U);
}

int main(void)
{
    test_single_exact_slot();
    test_dispatched_reboot_survives_cancel();
    test_reboot_wait();
    test_lost_reboot_message_cancels();
    test_lost_response_drops_will_reboot();
    test_will_reboot_golden();
    puts("reboot flow tests: OK");
    return 0;
}

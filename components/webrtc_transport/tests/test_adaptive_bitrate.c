#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "adaptive_bitrate.h"

/* Bytes the transport sends during one second at a given bit rate. */
static uint64_t second_at(uint32_t bps) { return bps / 8U; }

/* Receiver report state: packets the receiver expected and lost so far. */
typedef struct { int32_t lost; uint32_t sequence; } receiver_t;

static void report(webrtc_bitrate_controller_t *s, receiver_t *r, uint32_t packets,
                   int32_t lost)
{
    r->sequence += packets;
    r->lost += lost;
    webrtc_bitrate_loss(s, r->lost, r->sequence);
}

/* Motion after an idle desktop: the receiver estimate lags the send rate and
 * even falls, as Chrome's delay detector does for large scene-change frames
 * on a link that loses nothing. Without loss the target stays. */
static void test_falling_estimate_without_loss_holds(void)
{
    webrtc_bitrate_controller_t s = {0};
    receiver_t r = {0};
    uint64_t sent = 0;
    const uint32_t ceiling = 7000000;
    webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 0);
    webrtc_bitrate_remb(&s, 250000, 500);
    report(&s, &r, 20, 0);
    sent += second_at(160000);
    assert(webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 1000) == ceiling);
    for (int t = 2; t <= 20; ++t) {
        webrtc_bitrate_remb(&s, 250000 + (t / 2) * 16000, t * 1000 - 100);
        report(&s, &r, 700, 0);
        sent += second_at(ceiling);
        assert(webrtc_bitrate_step(&s, ceiling, sent, 0, 0, t * 1000) == ceiling);
    }
    webrtc_bitrate_remb(&s, 300000, 20900);
    report(&s, &r, 700, 0);
    sent += second_at(ceiling);
    assert(webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 21000) == ceiling);
    assert(s.decisions[WEBRTC_BITRATE_CUT] == 0);
}

/* A policed path loses packets while the encoder sends less than the target
 * would allow: loss cuts below both the sent rate and the estimate. */
static void test_loss_cuts_application_limited_sender(void)
{
    webrtc_bitrate_controller_t s = {0};
    receiver_t r = {0};
    uint64_t sent = 0;
    const uint32_t ceiling = 7000000;
    webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 0);
    report(&s, &r, 250, 0);
    webrtc_bitrate_remb(&s, 2000000, 900);
    sent += second_at(2500000);
    assert(webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 1000) == ceiling);
    report(&s, &r, 250, 13);      /* 5.2%: one lossy interval holds */
    webrtc_bitrate_remb(&s, 2000000, 1900);
    sent += second_at(2500000);
    assert(webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 2000) == ceiling);
    assert(s.loss_permille == 52);
    report(&s, &r, 250, 12);      /* 4.8%: the second one cuts */
    webrtc_bitrate_remb(&s, 2000000, 2900);
    sent += second_at(2500000);
    assert(webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 3000) == 1800000);
    assert(s.reason == WEBRTC_BITRATE_CUT);

    /* Continued loss at the new rate cuts again, below the sent rate even
     * when the estimate has not caught up. */
    report(&s, &r, 180, 9);       /* 5% */
    webrtc_bitrate_remb(&s, 2500000, 3900);
    sent += second_at(1800000);
    assert(webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 4000) == 1620000);
}

/* A report taken while the tail of a large frame is still in flight counts
 * it as lost; the next report takes it back. Isolated spikes never cut. */
static void test_transient_loss_spikes_hold(void)
{
    webrtc_bitrate_controller_t s = {0};
    receiver_t r = {0};
    uint64_t sent = 0;
    const uint32_t ceiling = 7000000;
    webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 0);
    report(&s, &r, 300, 0);
    webrtc_bitrate_remb(&s, 4000000, 900);
    sent += second_at(3000000);
    webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 1000);
    for (int t = 2; t <= 9; ++t) {
        report(&s, &r, 300, (t & 1) ? -13 : 13);
        webrtc_bitrate_remb(&s, 4000000, t * 1000 - 100);
        sent += second_at(3000000);
        assert(webrtc_bitrate_step(&s, ceiling, sent, 0, 0, t * 1000) == ceiling);
    }
    assert(s.decisions[WEBRTC_BITRATE_CUT] == 0);
}

static void test_loss_threshold_and_sample_size(void)
{
    webrtc_bitrate_controller_t s = {0};
    receiver_t r = {0};
    uint64_t sent = 0;
    const uint32_t ceiling = 7000000;
    webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 0);
    report(&s, &r, 700, 0);
    webrtc_bitrate_remb(&s, 3000000, 900);
    sent += second_at(3000000);
    webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 1000);
    report(&s, &r, 1000, 19);     /* 1.9%: residual, holds */
    webrtc_bitrate_remb(&s, 3000000, 1900);
    sent += second_at(3000000);
    assert(webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 2000) == ceiling);
    assert(s.loss_permille == 19);
    report(&s, &r, 40, 20);       /* 50% of too few packets to judge */
    webrtc_bitrate_remb(&s, 3000000, 2900);
    sent += second_at(3000000);
    assert(webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 3000) == ceiling);
    report(&s, &r, 1000, 20);     /* 2% */
    webrtc_bitrate_remb(&s, 3000000, 3900);
    sent += second_at(3000000);
    assert(webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 4000) == ceiling);
    report(&s, &r, 1000, 20);     /* 2% again: cuts */
    webrtc_bitrate_remb(&s, 3000000, 4900);
    sent += second_at(3000000);
    assert(webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 5000) == 2700000);
    /* An evaluation without a new report measures no loss. */
    sent += second_at(2700000);
    assert(webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 6000) == 2700000);
    assert(s.loss_permille == 0);
}

/* After a loss cut, continuous motion fills every target while the receiver
 * grows additively. Recovery must not require an idle scene. */
static void test_motion_recovery(void)
{
    webrtc_bitrate_controller_t s = {0};
    receiver_t r = {0};
    uint64_t sent = 0;
    const uint32_t ceiling = 7000000;
    webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 0);
    report(&s, &r, 700, 0);
    webrtc_bitrate_remb(&s, 1000000, 900);
    sent += second_at(ceiling);
    webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 1000);
    for (int t = 2; t <= 3; ++t) {
        report(&s, &r, 700, 70);
        webrtc_bitrate_remb(&s, 1000000, t * 1000 - 100);
        sent += second_at(ceiling);
        webrtc_bitrate_step(&s, ceiling, sent, 0, 0, t * 1000);
    }
    assert(s.target == 900000);
    for (int t = 4; t <= 37; ++t) {
        webrtc_bitrate_remb(&s, 1000000 + (t - 3) * 16000, t * 1000 - 100);
        report(&s, &r, s.target / 10000, 0);
        sent += second_at(s.target);
        webrtc_bitrate_step(&s, ceiling, sent, 0, 0, t * 1000);
    }
    assert(s.target == ceiling);
    assert(s.decisions[WEBRTC_BITRATE_CUT] == 1);
    assert(s.decisions[WEBRTC_BITRATE_RESTORE] > 0);
}

/* Receiver loss arms the falling-estimate cut for 30 s, so a real bottleneck
 * is followed before it drops more packets; without further loss the
 * estimate-only holds return. */
static void test_loss_arms_estimate_cuts(void)
{
    webrtc_bitrate_controller_t s = {0};
    receiver_t r = {0};
    uint64_t sent = 0;
    const uint32_t ceiling = 7000000;
    webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 0);
    report(&s, &r, 300, 0);
    webrtc_bitrate_remb(&s, 1500000, 900);
    sent += second_at(3000000);
    webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 1000);
    for (int t = 2; t <= 3; ++t) {
        report(&s, &r, 300, 30);
        webrtc_bitrate_remb(&s, 1500000, t * 1000 - 100);
        sent += second_at(3000000);
        webrtc_bitrate_step(&s, ceiling, sent, 0, 0, t * 1000);
    }
    assert(s.target == 1350000);
    /* Motion saturates the new target; the estimate falls with no loss. */
    report(&s, &r, 150, 0);
    webrtc_bitrate_remb(&s, 1200000, 3900);
    sent += second_at(1350000);
    assert(webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 4000) == 1080000);
    /* 30 s after the last loss the same pattern holds. */
    for (int t = 5; t <= 33; ++t) {
        report(&s, &r, 110, 0);
        webrtc_bitrate_remb(&s, 1200000, t * 1000 - 100);
        sent += second_at(s.target);
        webrtc_bitrate_step(&s, ceiling, sent, 0, 0, t * 1000);
    }
    const uint32_t before = s.target;
    report(&s, &r, 110, 0);
    webrtc_bitrate_remb(&s, before / 2, 33900);
    sent += second_at(before);
    assert(webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 34000) == before);
}

/* Slot-exhaustion drops: large frames on a clean link never cut, the same
 * drops after receiver loss (NACK retransmission load) do. */
static void test_slot_drops_count_only_after_loss(void)
{
    webrtc_bitrate_controller_t s = {0};
    receiver_t r = {0};
    uint64_t sent = 0, drops = 0;
    const uint32_t ceiling = 7000000;
    webrtc_bitrate_step(&s, ceiling, sent, 0, drops, 0);
    for (int t = 1; t <= 5; ++t) {
        report(&s, &r, 600, 0);
        webrtc_bitrate_remb(&s, 4000000, t * 1000 - 100);
        sent += second_at(6000000);
        drops += 3;
        assert(webrtc_bitrate_step(&s, ceiling, sent, 0, drops, t * 1000) == ceiling);
    }
    for (int t = 6; t <= 7; ++t) {
        report(&s, &r, 600, 30);
        webrtc_bitrate_remb(&s, 4000000, t * 1000 - 100);
        sent += second_at(6000000);
        webrtc_bitrate_step(&s, ceiling, sent, 0, drops, t * 1000);
    }
    assert(s.target == 3600000);
    report(&s, &r, 360, 0);
    webrtc_bitrate_remb(&s, 3000000, 7900);
    sent += second_at(3600000);
    drops += 2;
    assert(webrtc_bitrate_step(&s, ceiling, sent, 0, drops, 8000) == 2700000);
    assert(s.reason == WEBRTC_BITRATE_CUT);
}

/* Socket backpressure on a saturating sender cuts without receiver loss;
 * repeated backpressure holds; reports re-read later never probe. */
static void test_backpressure_and_probe_guards(void)
{
    webrtc_bitrate_controller_t s = {0};
    uint64_t sent = 0;
    const uint32_t ceiling = 7000000;
    webrtc_bitrate_step(&s, ceiling, sent, 0, 0, 0);
    webrtc_bitrate_remb(&s, 1000000, 900);
    sent += second_at(ceiling);
    assert(webrtc_bitrate_step(&s, ceiling, sent, 1, 0, 1000) == 900000);
    assert(s.reason == WEBRTC_BITRATE_CUT);
    for (int t = 2; t <= 12; ++t) {
        /* Re-reading a report must not authorize probing, even during the
         * five-second freshness window. */
        sent += second_at(900000);
        assert(webrtc_bitrate_step(&s, ceiling, sent, 1, 0, t * 1000) == 900000);
    }
    for (int t = 13; t <= 16; ++t) {
        webrtc_bitrate_remb(&s, 1000000, t * 1000 - 100);
        sent += second_at(900000);
        assert(webrtc_bitrate_step(&s, ceiling, sent, t - 11, 0, t * 1000) == 900000);
    }
    for (int t = 17; t <= 19; ++t) {
        webrtc_bitrate_remb(&s, 1000000, t * 1000 - 100);
        sent += second_at(900000);
        webrtc_bitrate_step(&s, ceiling, sent, 5, 0, t * 1000);
        if (t < 19) assert(s.target == 900000);
    }
    assert(s.target == 990000 && s.reason == WEBRTC_BITRATE_RESTORE);
    /* A decrease between evaluation ticks is preserved: it blocks the next
     * restore and restarts the recovery delay, but does not cut. */
    webrtc_bitrate_remb(&s, 700000, 19200);
    webrtc_bitrate_remb(&s, 800000, 19900);
    sent += second_at(s.target);
    assert(webrtc_bitrate_step(&s, ceiling, sent, 5, 0, 20000) == 990000);
    assert(s.reason == WEBRTC_BITRATE_HOLD);
    webrtc_bitrate_remb(&s, 800000, 20900);
    sent += second_at(s.target);
    assert(webrtc_bitrate_step(&s, ceiling, sent, 5, 0, 21000) == 990000);
}

/* A fixed rate that is measured: the target is the ceiling whatever the
 * receiver reports, each second gives the send rate, and a controller that
 * adapts again starts from the ceiling with none of the earlier reports and
 * none of the transport's earlier refusals. */
static void test_fixed_rate_is_measured(void)
{
    webrtc_bitrate_controller_t s = {0};
    receiver_t r = {0};
    uint64_t sent = 1000;
    const uint32_t ceiling = 12000000;
    /* The transport has refused 7 access units and dropped 3 for a slot
     * before; those counts are cumulative and do not grow here. */
    const uint64_t stalls = 7, drops = 3;
    assert(webrtc_bitrate_fixed(&s, ceiling, sent, stalls, drops, 5000) == ceiling);
    assert(s.step_ms == 5000 && s.achieved == 0);
    webrtc_bitrate_remb(&s, 2000000, 5500);
    report(&s, &r, 700, 140);
    sent += second_at(8400000) / 2;
    /* A call inside the second keeps the interval and forgets the reports. */
    assert(webrtc_bitrate_fixed(&s, ceiling, sent, stalls, drops, 5500) == ceiling);
    assert(s.step_ms == 5000 && s.achieved == 0);
    assert(s.remb_reports == 0 && s.loss_reports == 0);
    sent += second_at(8400000) / 2;
    assert(webrtc_bitrate_fixed(&s, ceiling, sent, stalls, drops, 6000) == ceiling);
    assert(s.step_ms == 6000 && s.achieved == 8400000);
    sent += second_at(6000000) * 2;
    assert(webrtc_bitrate_fixed(&s, ceiling, sent, stalls, drops, 8000) == ceiling);
    assert(s.achieved == 6000000);
    webrtc_bitrate_remb(&s, 2000000, 8500);
    assert(webrtc_bitrate_fixed(&s, ceiling, sent, stalls, drops, 9000) == ceiling);
    /* Auto is chosen, and a low estimate arrives before the first step. The
     * sender is at its target and the socket refuses nothing new, so the
     * earlier refusals are no backpressure and the step does not cut. */
    webrtc_bitrate_remb(&s, 2000000, 9200);
    sent += second_at(ceiling);
    assert(webrtc_bitrate_step(&s, ceiling, sent, stalls, drops, 10000) == ceiling);
    assert(s.decisions[WEBRTC_BITRATE_CUT] == 0 && s.achieved == ceiling);
}

int main(void)
{
    test_falling_estimate_without_loss_holds();
    test_loss_cuts_application_limited_sender();
    test_transient_loss_spikes_hold();
    test_loss_threshold_and_sample_size();
    test_motion_recovery();
    test_loss_arms_estimate_cuts();
    test_slot_drops_count_only_after_loss();
    test_backpressure_and_probe_guards();
    test_fixed_rate_is_measured();

    /* A zero estimate is not loss. With loss it cuts below the sent rate. */
    webrtc_bitrate_controller_t s = {0};
    receiver_t r = {0};
    uint64_t sent = 0;
    assert(webrtc_bitrate_step(&s, 7000000, sent, 0, 0, 0) == 7000000);
    report(&s, &r, 700, 0);
    webrtc_bitrate_remb(&s, 0, 900);
    sent += second_at(7000000);
    assert(webrtc_bitrate_step(&s, 7000000, sent, 0, 0, 1000) == 7000000);
    report(&s, &r, 700, 140);
    webrtc_bitrate_remb(&s, 0, 1900);
    sent += second_at(7000000);
    assert(webrtc_bitrate_step(&s, 7000000, sent, 0, 0, 2000) == 7000000);
    report(&s, &r, 700, 140);
    webrtc_bitrate_remb(&s, 0, 2900);
    sent += second_at(7000000);
    assert(webrtc_bitrate_step(&s, 7000000, sent, 0, 0, 3000) == 6300000);

    /* A lower ceiling clamps immediately, including below the floor; a
     * raised ceiling starts a new measurement interval at the new target. */
    assert(webrtc_bitrate_step(&s, 100000, sent, 0, 0, 8001) == 100000);
    assert(webrtc_bitrate_step(&s, 4000000, sent, 0, 0, 8002) == 4000000);
    assert(s.reason == WEBRTC_BITRATE_CEILING && s.step_ms == 8002);
    memset(&s, 0, sizeof(s));
    assert(webrtc_bitrate_step(&s, 7000000, 12345, 7, 0, 9000) == 7000000);
    assert(s.sent_bytes == 12345 && s.stalls == 7);
    assert(strcmp(webrtc_bitrate_reason_name(WEBRTC_BITRATE_CUT), "cut") == 0);
    assert(strcmp(webrtc_bitrate_reason_name(WEBRTC_BITRATE_REASON_COUNT), "unknown") == 0);
    puts("adaptive bitrate: loss and backpressure cuts, estimate-only holds, recovery, probing and ceiling PASS");
    return 0;
}

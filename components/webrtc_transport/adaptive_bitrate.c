#include "adaptive_bitrate.h"

/* Receive-side REMB only describes traffic the sender actually put on the
 * wire: it constrains a saturating sender and says nothing about an
 * application-limited one. A falling REMB alone is not evidence either: a
 * 100-packet scene-change frame takes this sender about 45 ms to put out,
 * and Chrome's delay detector reports that burst as overuse on links that
 * lose nothing. On a real bottleneck the same decreases arrive before the
 * loss does, so receiver loss arms them: for 30 s after lossy intervals a
 * falling REMB and slot-exhaustion drops cut as they did before. README.md
 * documents the policy. */

#define STEP_INTERVAL_MS 1000
#define REPORT_STALE_MS 5000
#define FLOOR_BPS 256000U
#define RESTORE_MIN_STEP_BPS 32000U
#define PROBE_COOLDOWN_MS 3000
/* Receiver loss over two consecutive evaluation intervals that cuts the
 * target. Wi-Fi and NACK retransmission leave zero residual loss on a link
 * with headroom, but a report taken while late packets are still in flight
 * counts them as lost (the Wi-Fi Mini's cumulative loss moved 0 -> 13 -> 0
 * within 250 ms about 40 times per 170 s run); the next report reverses it. */
#define LOSS_CUT_PERMILLE 20
#define LOSS_MIN_PACKETS 50
#define LOSS_ARM_MS 30000

static uint32_t minimum(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t maximum(uint32_t a, uint32_t b) { return a > b ? a : b; }

void webrtc_bitrate_remb(webrtc_bitrate_controller_t *s, uint32_t bps, int64_t now)
{
    /* Preserve decreases even if another report arrives before evaluation.
     * The first report can constrain a sender that started with motion. */
    if (!s->remb_reports || bps < s->remb) s->remb_decreases++;
    s->remb = bps;
    s->remb_ms = now;
    s->remb_reports++;
}

void webrtc_bitrate_loss(webrtc_bitrate_controller_t *s, int32_t cumulative_lost,
                         uint32_t highest_sequence)
{
    s->lost = cumulative_lost;
    s->sequence = highest_sequence;
    s->loss_reports++;
}

/* Loss over the interval since the previous evaluation, in permille; sets
 * the new interval's baseline. Zero without two reports to compare. */
static uint32_t interval_loss_permille(webrtc_bitrate_controller_t *s, uint32_t *expected)
{
    *expected = 0;
    uint32_t permille = 0;
    if (s->step_loss_reports && s->loss_reports != s->step_loss_reports) {
        *expected = s->sequence - s->step_sequence;
        const int64_t lost = (int64_t)s->lost - s->step_lost;
        if (*expected && *expected < 0x80000000U && lost > 0) {
            permille = (uint32_t)minimum(1000U, (uint32_t)(lost * 1000 / *expected));
        }
    }
    if (s->loss_reports != s->step_loss_reports) {
        s->step_lost = s->lost;
        s->step_sequence = s->sequence;
        s->step_loss_reports = s->loss_reports;
    }
    return permille;
}

uint32_t webrtc_bitrate_step(webrtc_bitrate_controller_t *s, uint32_t ceiling,
                             uint64_t sent_bytes, uint64_t stalls, uint64_t slot_drops,
                             int64_t now)
{
    if (ceiling > s->ceiling || s->target > ceiling) {
        /* A new peer or a raised manual ceiling carries no network evidence
         * against the ceiling; a lowered one clamps the target. Either way
         * the bytes so far were sent under the old target, so the interval
         * starts over. */
        s->ceiling = ceiling;
        s->target = ceiling;
        s->reason = WEBRTC_BITRATE_CEILING;
        s->step_ms = now;
        s->sent_bytes = sent_bytes;
        s->stalls = stalls;
        s->slot_drops = slot_drops;
        s->probe_after_ms = now + PROBE_COOLDOWN_MS;
        return ceiling;
    }
    s->ceiling = ceiling;
    const int64_t elapsed = now - s->step_ms;
    if (elapsed < STEP_INTERVAL_MS) return s->target;
    const uint64_t delta = sent_bytes >= s->sent_bytes ? sent_bytes - s->sent_bytes : 0;
    s->achieved = (uint32_t)(delta * 8000ULL / (uint64_t)elapsed);
    s->sent_bytes = sent_bytes;
    s->step_ms = now;

    /* Hardware rate control meets or overshoots its target whenever the
     * scene demands it; a full interval well below the target that was in
     * force means the scene, not the network, bounded the stream, unless the
     * transport dropped access units because sends backed up. */
    uint32_t expected = 0;
    s->loss_permille = interval_loss_permille(s, &expected);
    const bool interval_lossy = expected >= LOSS_MIN_PACKETS &&
                                s->loss_permille >= LOSS_CUT_PERMILLE;
    const bool lossy = interval_lossy && s->previous_interval_lossy;
    s->previous_interval_lossy = interval_lossy;
    if (lossy) s->loss_armed_until_ms = now + LOSS_ARM_MS;
    const bool armed = now < s->loss_armed_until_ms;
    const bool stalled = stalls != s->stalls || (armed && slot_drops != s->slot_drops);
    s->stalls = stalls;
    s->slot_drops = slot_drops;
    const bool limited = !stalled && s->achieved < s->target - s->target / 4;
    const bool fresh = s->remb_reports && now - s->remb_ms <= REPORT_STALE_MS;
    const bool new_feedback = fresh && s->remb_reports != s->consumed_remb;
    const bool decreased = new_feedback && s->remb_decreases != s->consumed_decreases;
    s->consumed_remb = s->remb_reports;
    s->evaluated_remb = s->remb;
    s->consumed_decreases = s->remb_decreases;
    if (stalled || decreased || lossy) s->probe_after_ms = now + PROBE_COOLDOWN_MS;
    uint32_t target = s->target;
    webrtc_bitrate_reason_t reason = WEBRTC_BITRATE_HOLD;
    const uint32_t floor = minimum(FLOOR_BPS, ceiling);
    const uint32_t limit = maximum(floor, minimum(ceiling,
        (uint32_t)((uint64_t)s->remb * 9 / 10)));
    if (lossy) {
        /* The path carried less than was sent, whatever the scene demanded:
         * go below both the achieved rate and a fresh estimate. */
        uint32_t cut = maximum(floor, (uint32_t)((uint64_t)s->achieved * 9 / 10));
        if (fresh && s->remb > 0) cut = minimum(cut, limit);
        if (cut < target) {
            target = cut;
            reason = WEBRTC_BITRATE_CUT;
        }
        s->decisions[reason]++;
    } else if (new_feedback) {
        /* A low but flat/rising estimate can still describe the static scene
         * before motion began. Socket backpressure on a saturating sender
         * cuts; a falling estimate and slot drops only after recent loss. */
        if (!limited && limit < target && s->remb < s->achieved &&
            (stalled || (armed && decreased))) {
            target = limit;
            reason = WEBRTC_BITRATE_CUT;
        } else if (!stalled && !decreased && s->remb > 0 && target < ceiling &&
                   (limited || now >= s->probe_after_ms)) {
            /* Idle scenes restore quickly; continuous motion also needs a
             * way out of a low target. Each new report permits one increase. */
            const uint32_t increase = limited ? target :
                maximum(target / 10, RESTORE_MIN_STEP_BPS);
            const uint64_t restored = (uint64_t)target + increase;
            target = restored < ceiling ? (uint32_t)restored : ceiling;
            reason = WEBRTC_BITRATE_RESTORE;
        } else if (limited && limit < target) {
            reason = WEBRTC_BITRATE_IGNORED;
        }
        s->decisions[reason]++;
    }
    s->target = target;
    s->reason = reason;
    return target;
}

uint32_t webrtc_bitrate_fixed(webrtc_bitrate_controller_t *s, uint32_t ceiling,
                              uint64_t sent_bytes, uint64_t stalls, uint64_t slot_drops,
                              int64_t now)
{
    int64_t step_ms = s->step_ms;
    uint64_t step_bytes = s->sent_bytes;
    uint32_t achieved = s->achieved;
    const int64_t elapsed = now - step_ms;
    if (step_ms == 0 || elapsed >= STEP_INTERVAL_MS) {
        achieved = step_ms != 0 && sent_bytes >= step_bytes ?
            (uint32_t)((sent_bytes - step_bytes) * 8000ULL / (uint64_t)elapsed) : 0;
        step_ms = now;
        step_bytes = sent_bytes;
    }
    /* Every call forgets the receiver's reports and takes the transport's
     * cumulative stalls and slot drops as the baseline. A step that follows
     * at any time then has no report from the fixed rate's time, and counts
     * as backpressure only what the socket refuses from now on. */
    *s = (webrtc_bitrate_controller_t){
        .target = ceiling, .ceiling = ceiling, .step_ms = step_ms,
        .sent_bytes = step_bytes, .achieved = achieved,
        .stalls = stalls, .slot_drops = slot_drops,
    };
    return ceiling;
}

const char *webrtc_bitrate_reason_name(webrtc_bitrate_reason_t reason)
{
    static const char *const names[WEBRTC_BITRATE_REASON_COUNT] = {
        "hold", "cut", "ignored", "restore", "ceiling",
    };
    return reason < WEBRTC_BITRATE_REASON_COUNT ? names[reason] : "unknown";
}

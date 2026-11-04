#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Why the last evaluation changed (or kept) the target. */
typedef enum {
    WEBRTC_BITRATE_HOLD = 0,     /* no new feedback, or a report that is neither evidence nor application-limited */
    WEBRTC_BITRATE_CUT,          /* receiver loss, or a saturating sender the socket pushed back on */
    WEBRTC_BITRATE_IGNORED,      /* receiver estimate below target from an application-limited sender */
    WEBRTC_BITRATE_RESTORE,      /* returning toward the ceiling, quickly when application-limited */
    WEBRTC_BITRATE_CEILING,      /* new peer or raised manual ceiling */
    WEBRTC_BITRATE_REASON_COUNT,
} webrtc_bitrate_reason_t;

typedef struct {
    uint32_t target, ceiling, remb;
    uint32_t remb_reports;
    uint32_t consumed_remb;
    uint32_t remb_decreases, consumed_decreases;
    int64_t remb_ms, step_ms;
    int64_t probe_after_ms;
    uint64_t sent_bytes;      /* cumulative transport bytes at the last evaluation */
    uint64_t stalls;          /* cumulative socket refusals at the last evaluation */
    uint64_t slot_drops;      /* cumulative slot-exhaustion drops at the last evaluation */
    uint32_t achieved;        /* send rate over the last evaluation interval, bit/s */
    uint32_t evaluated_remb;  /* receiver estimate the last evaluation saw */
    uint32_t loss_reports;    /* receiver report blocks for the video source */
    int32_t lost;             /* latest cumulative packets lost */
    uint32_t sequence;        /* latest extended highest sequence number */
    int32_t step_lost;        /* the two above at the last evaluation */
    uint32_t step_sequence;
    uint32_t step_loss_reports;
    uint32_t loss_permille;   /* receiver loss over the last evaluation interval */
    bool previous_interval_lossy;
    int64_t loss_armed_until_ms; /* falling REMB cuts until then */
    uint32_t decisions[WEBRTC_BITRATE_REASON_COUNT];
    webrtc_bitrate_reason_t reason;
} webrtc_bitrate_controller_t;

void webrtc_bitrate_remb(webrtc_bitrate_controller_t *, uint32_t bps, int64_t now_ms);

/* An RTCP receiver report block for the video source: cumulative packets
 * lost and the extended highest sequence number received. */
void webrtc_bitrate_loss(webrtc_bitrate_controller_t *, int32_t cumulative_lost,
                         uint32_t highest_sequence);

/* Evaluate at most once per second. sent_bytes is the cumulative count of
 * video bytes the transport sent; the controller derives the achieved send
 * rate from it. stalls is the cumulative count of access units the socket
 * refused (a full queue, a would-block or a send error): an interval with
 * new stalls counts as saturating even when few bytes went out. slot_drops is
 * the cumulative count of access units dropped because both slots were still
 * sending. On a clean link that is the sender's own per-packet cost for large
 * frames; after receiver loss it is NACK retransmission load, so it counts as
 * a stall only while recent loss has armed the controller. A zeroed controller initializes itself on the first call.
 * Returns the encoder target. */
uint32_t webrtc_bitrate_step(webrtc_bitrate_controller_t *, uint32_t ceiling,
                             uint64_t sent_bytes, uint64_t stalls, uint64_t slot_drops,
                             int64_t now_ms);

/* A fixed rate that is still measured: the target is the ceiling, and once
 * per second the achieved send rate is taken. Every call forgets the
 * feedback history and takes stalls and slot_drops, the cumulative counts
 * that webrtc_bitrate_step() gets, as the baseline, so that a later
 * webrtc_bitrate_step() starts from the ceiling with no report and no
 * refusal of the fixed rate's time. Returns the ceiling. */
uint32_t webrtc_bitrate_fixed(webrtc_bitrate_controller_t *, uint32_t ceiling,
                              uint64_t sent_bytes, uint64_t stalls, uint64_t slot_drops,
                              int64_t now_ms);

const char *webrtc_bitrate_reason_name(webrtc_bitrate_reason_t);

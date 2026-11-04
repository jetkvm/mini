/* Replay a recorded stats trace through the bitrate controller on the host
 * and score the result.
 *
 * The trace is a CSV file. Its first line names the columns; each later line
 * is one sample. The columns are fields of GET /webrtc/stats?bitrate=1, with
 * the JSON field names as column names, plus any columns the recorder adds.
 * The replay finds its columns by name and ignores the others: peerConnected
 * ("true" or "false"), generation, videoTargetBitrate and
 * videoBitrateCeiling (bit/s), and the videoBitrateStep* inputs listed in
 * `names` below. It skips rows without a peer, with a zero ceiling, or with
 * videoBitrateStepMs 0.
 *
 *   cc -std=c11 -O2 -I components/webrtc_transport \
 *     components/webrtc_transport/adaptive_bitrate.c components/webrtc_transport/tests/bitrate_replay.c \
 *     -o build-host/bitrate_replay
 *   build-host/bitrate_replay trace.csv [--rows]
 *
 * The device reports the inputs of its last controller evaluation
 * (videoBitrateStep*: time, cumulative bytes and stalls, REMB estimate,
 * report and decrease counters, receiver-report loss and sequence). Each time videoBitrateStepMs changes, the
 * replay feeds exactly those inputs to its own controller. With the unchanged
 * controller the replayed target equals the recorded one at every
 * evaluation; the "agree" figure measures that. After a policy change the
 * targets diverge where the decisions differ.
 *
 * The replay is open-loop: the recorded bytes were sent under the recorded
 * target, not the replayed one. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "adaptive_bitrate.h"

enum { PEER, GEN, TARGET, CEILING, STEP, BYTES, STALLS, REMB, REPORTS, DECREASES,
       LOST, SEQUENCE, LOSS_REPORTS, SLOT_DROPS, COLUMN_COUNT };
static const char *const names[COLUMN_COUNT] = {
    "peerConnected", "generation", "videoTargetBitrate", "videoBitrateCeiling",
    "videoBitrateStepMs", "videoBitrateStepBytes", "videoBitrateStepStalls",
    "videoBitrateStepRemb", "videoBitrateStepReports", "videoBitrateStepDecreases",
    "videoBitrateStepLost", "videoBitrateStepSequence", "videoBitrateStepLossReports",
    "videoBitrateStepSlotDrops",
};

typedef struct {
    double seconds, below_half;
    unsigned evaluations, agree, cuts, restores;
} score_t;

static void print_score(const char *label, const score_t *s)
{
    printf("%-9s %7.1f s  below 50%% of ceiling %6.1f s (%4.1f%%)  cuts %3u  restores %3u",
           label, s->seconds, s->below_half,
           s->seconds > 0 ? 100.0 * s->below_half / s->seconds : 0.0, s->cuts, s->restores);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s trace.csv [--rows]\n", argv[0]);
        return 2;
    }
    const int rows = argc > 2 && strcmp(argv[2], "--rows") == 0;
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    char line[1024];
    int index[COLUMN_COUNT];
    for (int i = 0; i < COLUMN_COUNT; ++i) index[i] = -1;
    if (!fgets(line, sizeof(line), f)) return 1;
    int column = 0;
    for (char *tok = strtok(line, ",\r\n"); tok; tok = strtok(NULL, ",\r\n"), ++column) {
        for (int i = 0; i < COLUMN_COUNT; ++i) {
            if (strcmp(tok, names[i]) == 0) index[i] = column;
        }
    }
    for (int i = 0; i < COLUMN_COUNT; ++i) {
        if (index[i] < 0) {
            fprintf(stderr, "missing column %s\n", names[i]);
            return 1;
        }
    }

    webrtc_bitrate_controller_t c = {0};
    score_t recorded = {0}, replayed = {0};
    uint32_t last_generation = 0, last_recorded = 0, last_replayed = 0;
    int64_t last_step = -1;
    int active = 0;
    if (rows) printf("stepMs,ceiling,remb,recorded,replayed,reason\n");
    while (fgets(line, sizeof(line), f)) {
        char *field[64];
        int n = 0;
        for (char *p = line; n < 64; ++n) {
            field[n] = p;
            char *comma = strchr(p, ',');
            if (!comma) { ++n; break; }
            *comma = '\0';
            p = comma + 1;
        }
        int complete = 1;
        for (int i = 0; i < COLUMN_COUNT; ++i) complete &= n > index[i];
        if (!complete) continue;
        const int peer = strcmp(field[index[PEER]], "true") == 0;
        const uint32_t generation = (uint32_t)strtoul(field[index[GEN]], NULL, 10);
        const uint32_t target = (uint32_t)strtoul(field[index[TARGET]], NULL, 10);
        const uint32_t ceiling = (uint32_t)strtoul(field[index[CEILING]], NULL, 10);
        const int64_t step = strtoll(field[index[STEP]], NULL, 10);
        if (!peer || ceiling == 0 || step == 0) {
            active = 0;
            continue;
        }
        /* The device zeroes the controller when a new peer takes over. */
        if (!active || generation != last_generation) {
            memset(&c, 0, sizeof(c));
            active = 1;
            last_step = -1;
        }
        last_generation = generation;
        if (step == last_step) continue;
        c.remb = (uint32_t)strtoul(field[index[REMB]], NULL, 10);
        c.remb_reports = (uint32_t)strtoul(field[index[REPORTS]], NULL, 10);
        c.remb_decreases = (uint32_t)strtoul(field[index[DECREASES]], NULL, 10);
        c.remb_ms = step;
        const uint32_t loss_reports = (uint32_t)strtoul(field[index[LOSS_REPORTS]], NULL, 10);
        if (loss_reports != c.loss_reports) {
            webrtc_bitrate_loss(&c, (int32_t)strtol(field[index[LOST]], NULL, 10),
                                (uint32_t)strtoul(field[index[SEQUENCE]], NULL, 10));
            c.loss_reports = loss_reports;
        }
        const unsigned cuts_before = c.decisions[WEBRTC_BITRATE_CUT];
        const unsigned restores_before = c.decisions[WEBRTC_BITRATE_RESTORE];
        const uint32_t out = webrtc_bitrate_step(&c, ceiling,
            strtoull(field[index[BYTES]], NULL, 10), strtoull(field[index[STALLS]], NULL, 10),
            strtoull(field[index[SLOT_DROPS]], NULL, 10), step);
        replayed.cuts += c.decisions[WEBRTC_BITRATE_CUT] - cuts_before;
        replayed.restores += c.decisions[WEBRTC_BITRATE_RESTORE] - restores_before;
        replayed.evaluations++;
        if (out == target) replayed.agree++;
        if (last_step >= 0) {
            const double dt = (double)(step - last_step) / 1000.0;
            recorded.seconds += dt;
            replayed.seconds += dt;
            if (last_recorded < ceiling / 2) recorded.below_half += dt;
            if (last_replayed < ceiling / 2) replayed.below_half += dt;
            if (target < last_recorded) recorded.cuts++;
            if (target > last_recorded) recorded.restores++;
        }
        last_step = step;
        last_recorded = target;
        last_replayed = out;
        if (rows) {
            printf("%" PRId64 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%s\n", step, ceiling,
                   c.remb, target, out, webrtc_bitrate_reason_name(c.reason));
        }
    }
    fclose(f);
    if (rows) return 0;
    print_score("recorded", &recorded);
    printf("\n");
    print_score("replayed", &replayed);
    printf("  agree %u/%u evaluations\n", replayed.agree, replayed.evaluations);
    return 0;
}

/* twcc implementation.
 * See LICENSE and LICENSES/ for copyright and licensing notices.
 */
#include "peer_platform.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "twcc.h"
#include "rtcp.h"
#include "rtp.h"

struct twcc_ctx {
    uint8_t ext_id;
    bool pre_reserved;
    uint16_t send_seq;
    uint16_t tx_oldest_seq;
    bool tx_valid;
    uint8_t fb_pkt_count;
    uint32_t last_fb_ms;
    bool last_fb_valid;
    uint16_t last_reported_seq;
    bool last_reported_valid;
    uint32_t recv_bps;
    bool has_bps;
    bool rx_valid;
    uint16_t oldest_seq;
    uint16_t latest_seq;
    uint16_t history_size;
    uint16_t history_mask;
    uint32_t history[];
};

static uint16_t *twcc_tx_sizes(twcc_ctx_t *ctx)
{
    return (uint16_t *)ctx->history;
}

static const uint16_t *twcc_tx_sizes_const(const twcc_ctx_t *ctx)
{
    return (const uint16_t *)ctx->history;
}

static uint32_t *twcc_rx_bitmap(twcc_ctx_t *ctx)
{
    return ctx->history + (ctx->history_size >> 1);
}

static const uint32_t *twcc_rx_bitmap_const(const twcc_ctx_t *ctx)
{
    return ctx->history + (ctx->history_size >> 1);
}

static uint32_t *twcc_arrival_ticks(twcc_ctx_t *ctx)
{
    return twcc_rx_bitmap(ctx) + (ctx->history_size >> 5);
}

static const uint32_t *twcc_arrival_ticks_const(const twcc_ctx_t *ctx)
{
    return twcc_rx_bitmap_const(ctx) + (ctx->history_size >> 5);
}

static int seq_ahead(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b) > 0;
}

static uint32_t twcc_now_tick(void)
{
    return esp_timer_get_time() / 250;
}

static void twcc_bit_set(twcc_ctx_t *ctx, uint16_t seq)
{
    uint16_t i = seq & ctx->history_mask;
    twcc_rx_bitmap(ctx)[i >> 5] |= 1U << (i & 31);
}

static void twcc_bit_clear(twcc_ctx_t *ctx, uint16_t seq)
{
    uint16_t i = seq & ctx->history_mask;
    twcc_rx_bitmap(ctx)[i >> 5] &= ~(1U << (i & 31));
}

static int twcc_bit_test(const twcc_ctx_t *ctx, uint16_t seq)
{
    uint16_t i = seq & ctx->history_mask;
    return (twcc_rx_bitmap_const(ctx)[i >> 5] >> (i & 31)) & 1;
}

static int twcc_rx_got(const twcc_ctx_t *ctx, uint16_t seq)
{
    if (!ctx->rx_valid)
        return 0;
    if ((uint16_t)(seq - ctx->oldest_seq) >= ctx->history_size)
        return 0;
    if ((int16_t)(ctx->latest_seq - seq) < 0)
        return 0;
    return twcc_bit_test(ctx, seq);
}

static void twcc_rx_slide(twcc_ctx_t *ctx, uint16_t new_oldest)
{
    uint16_t gap = new_oldest - ctx->oldest_seq;
    if (gap == 0)
        return;
    if (gap >= ctx->history_size) {
        memset(twcc_rx_bitmap(ctx), 0, ctx->history_size >> 3);
    } else {
        uint16_t seq = ctx->oldest_seq;
        for (uint16_t i = 0; i < gap; i++, seq++)
            twcc_bit_clear(ctx, seq);
    }
    ctx->oldest_seq = new_oldest;
}

static void twcc_rx_reset(twcc_ctx_t *ctx)
{
    ctx->rx_valid = false;
    memset(twcc_rx_bitmap(ctx), 0, ctx->history_size >> 3);
}

twcc_ctx_t *twcc_create(uint16_t history_size)
{
    if (history_size == 0)
        history_size = 256;
    if (history_size < 32 || history_size > 32768)
        return NULL;
    if (history_size & (history_size - 1))
        return NULL;
    size_t history_words = (history_size >> 1) + (history_size >> 5) + history_size;
    twcc_ctx_t *ctx = media_lib_calloc(1, sizeof(twcc_ctx_t) + history_words * sizeof(uint32_t));
    if (ctx) {
        ctx->ext_id = 3;
        ctx->history_size = history_size;
        ctx->history_mask = history_size - 1;
    }
    return ctx;
}

void twcc_destroy(twcc_ctx_t *ctx)
{
    media_lib_free(ctx);
}

void twcc_set_ext_id(twcc_ctx_t *ctx, uint8_t id)
{
    if (ctx && id > 0 && id < 15)
        ctx->ext_id = id;
}

uint8_t twcc_get_ext_id(twcc_ctx_t *ctx)
{
    return ctx ? ctx->ext_id : 3;
}

void twcc_set_ext_reserved(twcc_ctx_t *ctx, bool reserved)
{
    if (ctx)
        ctx->pre_reserved = reserved;
}

static void twcc_write_ext(uint8_t *d, uint8_t id, uint16_t seq)
{
    d[0] = 0xbe;
    d[1] = 0xde;
    d[2] = 0;
    d[3] = 1;
    d[4] = (id << 4) | 1;
    d[5] = seq >> 8;
    d[6] = seq;
    d[7] = 0;
}

int twcc_on_send(twcc_ctx_t *ctx, uint8_t *pkt, size_t *size, size_t cap)
{
    if (!ctx || !pkt || !size || *size < 12)
        return -1;
    rtp_header_t *h = (rtp_header_t *)pkt;
    size_t hdr = sizeof(rtp_header_t) + h->csrccount * 4;
    if (ctx->pre_reserved) {
        if (*size < hdr + 8)
            return -1;
    } else {
        if (h->extension)
            return -1;
        if (*size < hdr)
            return -1;
        if (cap < *size + 8)
            return -1;
        memmove(pkt + hdr + 8, pkt + hdr, *size - hdr);
        *size += 8;
    }
    uint16_t seq = ctx->send_seq++;
    twcc_write_ext(pkt + hdr, ctx->ext_id, seq);
    h->extension = 1;
    twcc_tx_sizes(ctx)[seq & ctx->history_mask] = *size > UINT16_MAX ? UINT16_MAX : (uint16_t)*size;
    if (!ctx->tx_valid) {
        ctx->tx_oldest_seq = seq;
        ctx->tx_valid = true;
    } else if ((uint16_t)(seq - ctx->tx_oldest_seq) >= ctx->history_size) {
        ctx->tx_oldest_seq = (uint16_t)(seq - ctx->history_size) + 1;
    }
    return 0;
}

static int twcc_parse_rtp_seq(twcc_ctx_t *ctx, const uint8_t *pkt, size_t size, uint16_t *seq)
{
    if (!ctx || !pkt || size < 20)
        return -1;
    rtp_header_t *h = (rtp_header_t *)pkt;
    if (!h->extension)
        return -1;
    size_t hdr = sizeof(rtp_header_t) + h->csrccount * 4;
    if (size < hdr + 4)
        return -1;
    uint16_t profile = (pkt[hdr] << 8) | pkt[hdr + 1];
    uint16_t words = (pkt[hdr + 2] << 8) | pkt[hdr + 3];
    const uint8_t *p = pkt + hdr + 4;
    const uint8_t *end = p + words * 4;
    if (end > pkt + size)
        return -1;
    if (profile != 0xbede)
        return -1;
    while (p < end) {
        if (*p == 0) {
            p++;
            continue;
        }
        uint8_t id = *p >> 4;
        size_t n = (*p & 15) + 1;
        p++;
        if (p + n > end)
            return -1;
        if (id == ctx->ext_id && n >= 2) {
            *seq = (p[0] << 8) | p[1];
            return 0;
        }
        p += n;
    }
    return -1;
}

void twcc_on_recv(twcc_ctx_t *ctx, const uint8_t *pkt, size_t size)
{
    uint16_t seq = 0;
    if (!ctx || twcc_parse_rtp_seq(ctx, pkt, size, &seq) != 0)
        return;
    uint32_t tick = twcc_now_tick();
    if (!ctx->rx_valid) {
        ctx->oldest_seq = seq;
        ctx->latest_seq = seq;
        ctx->rx_valid = true;
        twcc_bit_set(ctx, seq);
        twcc_arrival_ticks(ctx)[seq & ctx->history_mask] = tick;
        return;
    }
    if ((int16_t)(seq - ctx->oldest_seq) < 0)
        return;
    if ((uint16_t)(seq - ctx->oldest_seq) >= ctx->history_size)
        twcc_rx_slide(ctx, (uint16_t)(seq - ctx->history_size) + 1);
    twcc_bit_set(ctx, seq);
    twcc_arrival_ticks(ctx)[seq & ctx->history_mask] = tick;
    if (seq_ahead(seq, ctx->latest_seq))
        ctx->latest_seq = seq;
}

static uint16_t twcc_lookup_send(const twcc_ctx_t *ctx, uint16_t seq)
{
    if (!ctx->tx_valid)
        return 0;
    if ((uint16_t)(seq - ctx->tx_oldest_seq) >= ctx->history_size)
        return 0;
    if (seq_ahead(seq, ctx->send_seq - 1))
        return 0;
    return twcc_tx_sizes_const(ctx)[seq & ctx->history_mask];
}

static int32_t twcc_delta_ticks(uint32_t tick, uint32_t prev_tick)
{
    int32_t ticks = tick - prev_tick;
    if (ticks > INT16_MAX)
        return INT16_MAX;
    if (ticks < INT16_MIN)
        return INT16_MIN;
    return ticks;
}

static uint8_t twcc_status_from_ticks(int32_t ticks)
{
    if (ticks >= 0 && ticks < 256)
        return 1;
    return 2;
}

static uint8_t twcc_classify(const twcc_ctx_t *ctx, uint16_t seq, uint32_t *prev_tick,
                             bool *have_ref, uint32_t *ref_units)
{
    if (!twcc_rx_got(ctx, seq))
        return 0;
    uint32_t tick = twcc_arrival_ticks_const(ctx)[seq & ctx->history_mask];
    if (!*have_ref) {
        *ref_units = tick >> 8;
        *prev_tick = tick & ~255U;
        *have_ref = true;
    }
    int32_t ticks = twcc_delta_ticks(tick, *prev_tick);
    *prev_tick = tick;
    return twcc_status_from_ticks(ticks);
}

static int twcc_consume_delta(uint8_t st, const uint8_t *pkt, int len, int *off,
                              uint32_t *cur_us, uint32_t *first_us, uint32_t *last_us,
                              bool *have_time)
{
    int32_t ticks;
    if (st == 1) {
        if (*off >= len)
            return -1;
        ticks = pkt[(*off)++];
    } else {
        if (*off + 1 >= len)
            return -1;
        ticks = (int16_t)((pkt[*off] << 8) | pkt[*off + 1]);
        *off += 2;
    }
    *cur_us += ticks * 250;
    if (!*have_time) {
        *first_us = *cur_us;
        *have_time = true;
    }
    *last_us = *cur_us;
    return 0;
}

static int twcc_apply_one(twcc_ctx_t *ctx, uint16_t seq, uint8_t st,
                          const uint8_t *pkt, int len, int *off, uint32_t *sent,
                          uint32_t *acked, uint32_t *cur_us, uint32_t *first_us,
                          uint32_t *last_us, uint16_t *recv_packets, bool *have_time)
{
    if (st > 2)
        return -1;
    uint16_t size = twcc_lookup_send(ctx, seq);
    *sent += size;
    if (!st)
        return 0;
    if (twcc_consume_delta(st, pkt, len, off, cur_us, first_us, last_us, have_time) != 0)
        return -1;
    *acked += size;
    (*recv_packets)++;
    return 0;
}

int twcc_build_feedback(twcc_ctx_t *ctx, uint8_t *out, int out_len,
                        uint32_t sender_ssrc, uint32_t media_ssrc,
                        uint32_t now_ms, uint16_t period_ms)
{
    if (!ctx || !out || out_len < 20 || !ctx->rx_valid)
        return 0;
    if (!period_ms)
        period_ms = 200;
    if (!ctx->last_fb_valid) {
        ctx->last_fb_ms = now_ms;
        ctx->last_fb_valid = true;
        return 0;
    }
    if (now_ms - ctx->last_fb_ms < period_ms)
        return 0;

    uint16_t base = ctx->oldest_seq;
    uint16_t last = ctx->latest_seq;
    if (ctx->last_reported_valid) {
        uint16_t expect = ctx->last_reported_seq + 1;
        if (seq_ahead(base, expect)) {
            uint16_t gap = base - expect;
            uint16_t max_prefix = ctx->history_size >> 2;
            if (gap <= max_prefix)
                base = expect;
            else
                base -= max_prefix;
        }
    }
    uint16_t count = (uint16_t)(last - base) + 1;
    if (!count || count > ctx->history_size)
        count = ctx->history_size;
    int max_count = (out_len - 24) / 3;
    if (max_count <= 0)
        return 0;
    if (count > (uint16_t)max_count)
        count = (uint16_t)max_count;

    uint32_t prev_tick = 0;
    uint32_t ref_units = 0;
    bool have_ref = false;
    uint16_t probe;
    for (probe = 0; probe < count; probe++) {
        if (twcc_classify(ctx, base + probe, &prev_tick, &have_ref, &ref_units))
            break;
    }
    if (!have_ref) {
        twcc_rx_reset(ctx);
        return 0;
    }

    memset(out, 0, 20);
    uint8_t *p = out;
    rtcp_header_t *hdr = (rtcp_header_t *)p;
    hdr->version = 2;
    hdr->rc = 15;
    hdr->type = RTCP_RTPFB;
    uint32_t ssrc_n = __builtin_bswap32(sender_ssrc);
    uint32_t media_n = __builtin_bswap32(media_ssrc);
    memcpy(p + 4, &ssrc_n, 4);
    memcpy(p + 8, &media_n, 4);
    p[12] = base >> 8;
    p[13] = base;
    p[14] = count >> 8;
    p[15] = count;
    p[16] = ref_units >> 16;
    p[17] = ref_units >> 8;
    p[18] = ref_units;
    p[19] = ctx->fb_pkt_count++;
    int off = 20;

    prev_tick = 0;
    have_ref = false;
    ref_units = 0;
    uint16_t i = 0;
    while (i < count) {
        if (off + 1 >= out_len)
            break;
        uint32_t lp = prev_tick;
        bool lr = have_ref;
        uint32_t lru = ref_units;
        uint8_t st = twcc_classify(ctx, base + i, &lp, &lr, &lru);
        uint16_t run = 1;
        while ((uint16_t)(i + run) < count && run < 8191) {
            uint8_t st2 = twcc_classify(ctx, base + i + run, &lp, &lr, &lru);
            if (st2 != st)
                break;
            run++;
        }
        if (run >= 8 || (uint16_t)(i + run) >= count) {
            uint16_t chunk = ((st & 3) << 13) | (run & 0x1fff);
            p[off++] = chunk >> 8;
            p[off++] = chunk;
            for (uint16_t n = 0; n < run; n++, i++)
                twcc_classify(ctx, base + i, &prev_tick, &have_ref, &ref_units);
        } else {
            uint16_t chunk = 0xc000;
            int s;
            for (s = 0; s < 7 && i < count; s++, i++) {
                uint8_t sti = twcc_classify(ctx, base + i, &prev_tick, &have_ref, &ref_units);
                chunk |= (uint16_t)((sti & 3) << ((6 - s) * 2));
            }
            p[off++] = chunk >> 8;
            p[off++] = chunk;
        }
    }

    prev_tick = 0;
    have_ref = false;
    ref_units = 0;
    for (i = 0; i < count; i++) {
        uint16_t seq = base + i;
        if (!twcc_rx_got(ctx, seq))
            continue;
        uint32_t tick = twcc_arrival_ticks_const(ctx)[seq & ctx->history_mask];
        if (!have_ref) {
            ref_units = tick >> 8;
            prev_tick = tick & ~255U;
            have_ref = true;
        }
        int32_t ticks = twcc_delta_ticks(tick, prev_tick);
        prev_tick = tick;
        if (ticks >= 0 && ticks <= 255) {
            if (off >= out_len)
                break;
            p[off++] = ticks;
        } else {
            if (off + 1 >= out_len)
                break;
            p[off++] = (uint16_t)ticks >> 8;
            p[off++] = ticks;
        }
    }
    while (off & 3) {
        if (off >= out_len)
            break;
        p[off++] = 0;
    }
    hdr->length = __builtin_bswap16((uint16_t)((off / 4) - 1));
    twcc_rx_reset(ctx);
    ctx->last_fb_ms = now_ms;
    ctx->last_fb_valid = true;
    ctx->last_reported_seq = (uint16_t)(base + count) - 1;
    ctx->last_reported_valid = true;
    return off;
}

int twcc_parse_feedback(twcc_ctx_t *ctx, const uint8_t *pkt, int len,
                        esp_peer_twcc_report_t *report)
{
    if (!ctx || !pkt || !report || len < 20)
        return -1;
    uint16_t base = (pkt[12] << 8) | pkt[13];
    uint16_t count = (pkt[14] << 8) | pkt[15];
    uint32_t ref_units = (pkt[16] << 16) | (pkt[17] << 8) | pkt[18];
    if (!count || count > ctx->history_size)
        return -1;

    int chunk_off = 20;
    uint16_t filled = 0;
    while (filled < count && chunk_off + 1 < len) {
        uint16_t chunk = (pkt[chunk_off] << 8) | pkt[chunk_off + 1];
        chunk_off += 2;
        if (!(chunk & 0x8000)) {
            uint16_t run = chunk & 0x1fff;
            if (!run || run > (uint16_t)(count - filled))
                return -1;
            filled += run;
        } else if (chunk & 0x4000) {
            uint16_t left = count - filled;
            filled += left < 7 ? left : 7;
        } else {
            uint16_t left = count - filled;
            filled += left < 14 ? left : 14;
        }
    }
    if (filled < count)
        return -1;

    int off = chunk_off;
    uint32_t acked = 0;
    uint32_t sent = 0;
    uint32_t first_us = 0;
    uint32_t last_us = 0;
    uint16_t recv_packets = 0;
    bool have_time = false;
    uint32_t cur_us = ref_units * 64000;

    int walk = 20;
    filled = 0;
    while (filled < count && walk + 1 < chunk_off) {
        uint16_t chunk = (pkt[walk] << 8) | pkt[walk + 1];
        walk += 2;
        if (!(chunk & 0x8000)) {
            uint8_t st = (chunk >> 13) & 3;
            uint16_t run = chunk & 0x1fff;
            for (uint16_t n = 0; n < run && filled < count; n++, filled++) {
                if (twcc_apply_one(ctx, base + filled, st, pkt, len, &off, &sent,
                                   &acked, &cur_us, &first_us, &last_us, &recv_packets,
                                   &have_time) != 0)
                    return -1;
            }
        } else {
            int two_bit = chunk & 0x4000;
            int smax = two_bit ? 7 : 14;
            int s;
            for (s = 0; s < smax && filled < count; s++, filled++) {
                uint8_t st = two_bit ? (chunk >> ((6 - s) * 2)) & 3 :
                                      (chunk >> (13 - s)) & 1;
                if (twcc_apply_one(ctx, base + filled, st, pkt, len, &off, &sent,
                                   &acked, &cur_us, &first_us, &last_us, &recv_packets,
                                   &have_time) != 0)
                    return -1;
            }
        }
    }
    if (filled < count)
        return -1;
    uint32_t window_us = have_time ? last_us - first_us : 1000;
    if (window_us < 1000)
        window_us = 1000;
    uint32_t instant = (uint64_t)acked * 8000000 / window_us;
    if (!ctx->has_bps) {
        ctx->recv_bps = instant;
        ctx->has_bps = true;
    } else {
        ctx->recv_bps = (ctx->recv_bps * 3 + instant) >> 2;
    }
    report->sent_size = sent;
    report->recved_size = acked;
    report->sent_packets = count;
    report->recv_packets = recv_packets;
    report->recv_bitrate = ctx->recv_bps;
    report->lost_ratio = (float)(count - recv_packets) / (float)(uint32_t)count;
    return 0;
}

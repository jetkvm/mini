/* rtp jitter implementation.
 * See LICENSE and LICENSES/ for copyright and licensing notices.
 */
#include "peer_platform.h"
#include "peer_types.h"
#include "rtp_jitter.h"
#include "buf_mngr.h"
#include "rtp.h"
#include "rtcp.h"

#include <stdbool.h>
#include <stdlib.h>

typedef struct {
    int (*rtp_callback)(uint8_t *payload, uint32_t size, void *ctx);
    uint32_t src_ssrc;
    uint32_t max_timeout;
    uint32_t resend_delay;
    void *ctx;
    uint32_t base_seq;
    uint16_t cur_seq;
    bool tsn_filled;
    uint32_t ack_tsn;
    uint32_t ack_time;
    uint32_t nack_send_time;
    uint32_t media_ssrc;
    buf_mngr_t *cache;
} rtp_jitter_t;

_Static_assert(sizeof(rtp_jitter_t) == 48, "vendor jitter state size");
_Static_assert(offsetof(rtp_jitter_t, cache) == 44, "vendor jitter cache offset");

static uint32_t get_tsn(rtp_jitter_t *jitter, uint16_t seq_num)
{
    if (jitter->cur_seq > 0xff00 && seq_num < 0x100) {
        return ((jitter->base_seq + 1) << 16) + seq_num;
    } else if (jitter->cur_seq < 0x100 && seq_num > 0xff00) {
        return ((jitter->base_seq - 1) << 16) + seq_num;
    }
    return (jitter->base_seq << 16) + seq_num;
}

static void update_acked_tsn(rtp_jitter_t *jitter, uint32_t tsn, uint32_t access_time)
{
    jitter->ack_time = access_time;
    jitter->ack_tsn = tsn;
    jitter->base_seq = tsn >> 16;
    jitter->cur_seq = tsn;
}

int rtp_jitter_create(rtp_jitter_cfg_t *cfg, rtp_jitter_handle_t *h)
{
    rtp_jitter_t *jitter = (rtp_jitter_t *)media_lib_calloc(1, sizeof(rtp_jitter_t));
    if (jitter == NULL) {
        return ESP_PEER_ERR_NO_MEM;
    }
    int limit_size = cfg->size ? cfg->size : 100 * 1024;
    int timeout = cfg->timeout ? cfg->timeout : 100;
    jitter->max_timeout = timeout;
    jitter->rtp_callback = cfg->rtp_callback;
    jitter->ctx = cfg->ctx;
    jitter->src_ssrc = cfg->src_ssrc;
    jitter->resend_delay = cfg->resend_delay ? cfg->resend_delay : 20;
    do {
        jitter->cache = buf_mngr_create(limit_size, timeout);
        if (jitter->cache == NULL) {
            break;
        }
        *h = jitter;
        return ESP_PEER_ERR_NONE;
    } while (0);
    rtp_jitter_destroy(jitter);
    return ESP_PEER_ERR_NO_MEM;
}

static inline bool rtp_seq_is_large_jump(uint16_t from_seq, uint16_t to_seq)
{
    uint16_t udelta = to_seq - from_seq;
    return udelta >= 3000 && udelta <= 65536 - 100;
}

int rtp_jitter_add(rtp_jitter_handle_t h, uint8_t *payload, uint32_t size)
{
    rtp_jitter_t *jitter = (rtp_jitter_t *)h;
    uint32_t tsn = get_tsn(jitter, rtp_packet_get_seq_number(payload));
    int ret = buf_mngr_add(jitter->cache, tsn, 0, 0, payload, size);
    if (ret == 1) {
        return ESP_PEER_ERR_NONE;
    }
    if (jitter->tsn_filled == false) {
        jitter->ack_tsn = tsn;
        jitter->ack_time = esp_timer_get_time() / 1000;
        jitter->media_ssrc = rtp_get_ssrc(payload);
        jitter->tsn_filled = true;
    }
    buf_mngr_check_overdue(jitter->cache);

    uint32_t head_tsn = 0;
    bool ok = buf_mngr_get_head_tsn(jitter->cache, &head_tsn, NULL);
    if (ok == false) {
        return ESP_PEER_ERR_NONE;
    }
    buf_info_t buf_info = {0};
    buf_mngr_get_buffer(jitter->cache, head_tsn, &buf_info);
    uint32_t cur = esp_timer_get_time() / 1000;
    if (head_tsn != jitter->ack_tsn && head_tsn != jitter->ack_tsn + 1) {
        bool timeout = jitter->ack_time + jitter->max_timeout < cur;
        buf_info_t nxt = {0};
        bool consecutive_run = buf_mngr_get_buffer(jitter->cache, head_tsn + 1, &nxt);
        bool large_jump = rtp_seq_is_large_jump(jitter->ack_tsn, head_tsn);

        if (!timeout && !(large_jump && consecutive_run)) {
            return ESP_PEER_ERR_NONE;
        }
        update_acked_tsn(jitter, head_tsn, buf_info.add_time);
    }

    uint32_t ack_tsn;
    uint32_t ack_time;
    do {
        ack_tsn = head_tsn;
        ack_time = buf_info.add_time;
        jitter->rtp_callback(buf_info.buffer, buf_info.size, jitter->ctx);
        buf_mngr_remove(jitter->cache, buf_info.buf_inst);
        ok = buf_mngr_get_buffer(jitter->cache, ++head_tsn, &buf_info);
    } while (ok);
    update_acked_tsn(jitter, ack_tsn, ack_time);
    return ESP_PEER_ERR_NONE;
}

int rtp_jitter_get_nack(rtp_jitter_handle_t h, uint8_t *payload, uint32_t size)
{
    rtp_jitter_t *jitter = (rtp_jitter_t *)h;
    uint32_t cur = esp_timer_get_time() / 1000;

    if (cur < jitter->nack_send_time + jitter->resend_delay) {
        return ESP_PEER_ERR_NOT_EXISTS;
    }

    uint32_t head_tsn = 0;
    bool ok = buf_mngr_get_head_tsn(jitter->cache, &head_tsn, NULL);
    if (ok == false) {
        return ESP_PEER_ERR_NOT_EXISTS;
    }
    int pos = sizeof(rtcp_fb_t);
    int n = (size - pos) / 4;
    uint32_t *nack_entry = (uint32_t *)(payload + pos);
    int nack_num = buf_mngr_get_nack_data(jitter->cache, nack_entry, n);
    if (nack_num > 0) {
        rtcp_fb_t *fb = (rtcp_fb_t *)payload;
        rtcp_get_fb(fb, jitter->media_ssrc, jitter->media_ssrc, nack_num);
        jitter->nack_send_time = cur;
        pos += nack_num * 4;
        return pos;
    }
    return ESP_PEER_ERR_NOT_EXISTS;
}

int rtp_jitter_destroy(rtp_jitter_handle_t h)
{
    rtp_jitter_t *jitter = (rtp_jitter_t *)h;
    if (jitter == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    if (jitter->cache) {
        buf_mngr_destroy(jitter->cache);
        jitter->cache = NULL;
    }
    media_lib_free(jitter);
    return ESP_PEER_ERR_NONE;
}

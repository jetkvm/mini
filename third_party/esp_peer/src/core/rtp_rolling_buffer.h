#ifndef ESP_PEER_CORE_RTP_ROLLING_BUFFER_H
#define ESP_PEER_CORE_RTP_ROLLING_BUFFER_H

#include <stddef.h>
#include <stdint.h>

typedef void *rtp_rolling_buf_handle_t;

typedef struct {
    uint32_t pool_size;
    uint16_t packet_count;
    uint16_t meta_size;
    uint16_t max_resend_limit;
} rtp_rolling_buf_cfg_t;

typedef struct {
    uint16_t packet_idx;
    uint8_t *data;
    size_t size;
    uint8_t *meta_data;
} rtp_rolling_buf_pkt_t;

rtp_rolling_buf_handle_t rtp_rolling_buffer_create(rtp_rolling_buf_cfg_t *cfg);
int rtp_rolling_buffer_insert(rtp_rolling_buf_handle_t handle, uint32_t uuid,
                              uint8_t *data, size_t size, uint8_t *meta_data);
int rtp_rolling_buffer_acquire(rtp_rolling_buf_handle_t handle, uint32_t uuid,
                               rtp_rolling_buf_pkt_t *pkt);
int rtp_rolling_buffer_release(rtp_rolling_buf_handle_t handle, rtp_rolling_buf_pkt_t *pkt);
void rtp_rolling_buffer_destroy(rtp_rolling_buf_handle_t handle);

_Static_assert(sizeof(rtp_rolling_buf_cfg_t) == 12, "vendor rolling buffer config size");
_Static_assert(sizeof(rtp_rolling_buf_pkt_t) == 16, "vendor rolling packet size");
_Static_assert(offsetof(rtp_rolling_buf_pkt_t, data) == 4, "vendor rolling packet data offset");
_Static_assert(offsetof(rtp_rolling_buf_pkt_t, size) == 8, "vendor rolling packet size offset");
_Static_assert(offsetof(rtp_rolling_buf_pkt_t, meta_data) == 12, "vendor rolling packet meta offset");

#endif

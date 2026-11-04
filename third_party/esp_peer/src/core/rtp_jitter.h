#ifndef ESP_PEER_CORE_RTP_JITTER_H
#define ESP_PEER_CORE_RTP_JITTER_H

#include <stddef.h>
#include <stdint.h>

typedef void *rtp_jitter_handle_t;

typedef struct {
    uint32_t size;
    uint32_t timeout;
    uint32_t resend_delay;
    uint32_t src_ssrc;
    int (*rtp_callback)(uint8_t *payload, uint32_t size, void *ctx);
    void *ctx;
} rtp_jitter_cfg_t;

_Static_assert(sizeof(rtp_jitter_cfg_t) == 24, "vendor jitter config size");
_Static_assert(offsetof(rtp_jitter_cfg_t, rtp_callback) == 16, "vendor jitter callback offset");

int rtp_jitter_create(rtp_jitter_cfg_t *cfg, rtp_jitter_handle_t *h);
int rtp_jitter_add(rtp_jitter_handle_t h, uint8_t *payload, uint32_t size);
int rtp_jitter_get_nack(rtp_jitter_handle_t h, uint8_t *payload, uint32_t size);
int rtp_jitter_destroy(rtp_jitter_handle_t h);

#endif

#ifndef ESP_PEER_CORE_RTCP_H
#define ESP_PEER_CORE_RTCP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t rc : 5;
    uint16_t padding : 1;
    uint16_t version : 2;
    uint16_t type : 8;
    uint16_t length : 16;
} rtcp_header_t;

typedef struct {
    uint32_t ssrc;
    uint32_t flcnpl;
    uint32_t ehsnr;
    uint32_t jitter;
    uint32_t lsr;
    uint32_t dlsr;
} rtcp_report_block_t;

typedef struct {
    rtcp_header_t header;
    uint32_t ssrc;
    rtcp_report_block_t report_block[1];
} rtcp_rr_t;

typedef struct {
    uint32_t ssrc;
    uint32_t seqnr;
} rtcp_fir_t;

typedef struct {
    rtcp_header_t header;
    uint32_t ssrc;
    uint32_t media;
} rtcp_fb_t;

enum {
    RTCP_RTPFB = 205,
    RTCP_PSFB = 206,
    RTCP_FEEDBACK_NACK = 1,
    RTCP_FEEDBACK_AFB = 15,
};

int rtcp_probe(uint8_t *packet, size_t size);
int rtcp_get_pli(uint8_t *packet, int len, uint32_t ssrc);
int rtcp_get_fir(uint8_t *packet, int len, int *seqnr);
int rtcp_get_fb(rtcp_fb_t *fb, uint32_t ssrc, uint32_t media_ssrc, int lost_cnt);
int rtcp_parse_header(uint8_t *data, int len, rtcp_header_t *header);
int rtcp_parse_remb(const uint8_t *packet, int len, uint32_t *bitrate, uint32_t *ssrc);
int rtcp_parse_fb(uint8_t *packet, int len, rtcp_fb_t *fb);
int rtcp_parse_rr(uint8_t *packet, int len, rtcp_rr_t *rr);

#endif

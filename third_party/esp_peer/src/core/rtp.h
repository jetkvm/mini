#ifndef ESP_PEER_CORE_RTP_H
#define ESP_PEER_CORE_RTP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { PT_PCMU=0, PT_PCMA=8, PT_G722=9, PT_JPEG=26,
               PT_H264=102, PT_H264_1=106, PT_OPUS=111 } rtp_payload_type_t;
enum { SSRC_H264=1, SSRC_JPEG=2, SSRC_PCMA=4, SSRC_PCMU=5, SSRC_OPUS=6 };
typedef struct {
    uint16_t csrccount:4, extension:1, padding:1, version:2, type:7, markerbit:1;
    uint16_t seq_number;
    uint32_t timestamp, ssrc, csrc[0];
} rtp_header_t;
typedef struct { rtp_header_t header; uint8_t payload[0]; } rtp_packet_t;
typedef void (*rtp_encoder_done_cb)(uint8_t *,size_t,void *);
typedef void (*rtc_decoder_done_cb)(rtp_header_t *,uint8_t *,size_t,void *);
typedef struct rtp_decoder_t {
    rtp_payload_type_t type;
    rtc_decoder_done_cb on_packet;
    int (*decode_func)(struct rtp_decoder_t *,uint8_t *,size_t);
    void *user_data;
    uint8_t *frame_buf;
    uint32_t frame_size,frame_capacity,last_timestamp;
    uint16_t last_seq;
    bool has_last_seq,has_last_ts;
    uint32_t jpeg_hdr_size;
} rtp_decoder_t;
typedef struct rtp_encoder_t {
    rtp_payload_type_t type;
    rtp_encoder_done_cb on_packet;
    int (*encode_func)(struct rtp_encoder_t *,uint8_t *,size_t);
    void *user_data;
    uint16_t seq_number;
    uint32_t ssrc,timestamp;
    uint16_t jpeg_width,jpeg_height;
    uint8_t jpeg_type;
    bool jpeg_info_ready;
    uint8_t ext_reserve;
    uint8_t buf[1428];
} rtp_encoder_t;
typedef struct {
    bool used,synced;
    uint32_t ssrc;
    uint16_t max_seq;
    uint8_t probation;
    uint8_t *held;
    uint32_t held_size,held_cap;
} rtp_seq_src_t;
typedef struct { rtp_seq_src_t src[3]; } rtp_seq_tracker_t;
int rtp_packet_validate(uint8_t *,size_t);
uint32_t rtp_get_ssrc(uint8_t *);
uint16_t rtp_packet_get_seq_number(uint8_t *);
int rtp_seq_filter(rtp_seq_tracker_t *,uint8_t *,uint32_t,uint8_t **,uint32_t *);
void rtp_seq_tracker_reset(rtp_seq_tracker_t *);
void rtp_seq_tracker_deinit(rtp_seq_tracker_t *);
void rtp_encoder_init(rtp_encoder_t *,rtp_payload_type_t,rtp_encoder_done_cb,void *);
void rtp_encoder_set_codec(rtp_encoder_t *,rtp_payload_type_t);
void rtp_encoder_set_ext_reserve(rtp_encoder_t *,uint8_t);
void rtp_encoder_set_jpeg_info(rtp_encoder_t *,uint16_t,uint16_t);
int rtp_encoder_encode(rtp_encoder_t *,uint32_t,uint8_t *,size_t);
void rtp_encoder_deinit(rtp_encoder_t *);
void rtp_decoder_set_codec(rtp_decoder_t *,rtp_payload_type_t);
void rtp_decoder_init(rtp_decoder_t *,rtp_payload_type_t,rtc_decoder_done_cb,void *);
int rtp_decoder_decode(rtp_decoder_t *,uint8_t *,size_t);
void rtp_decoder_deinit(rtp_decoder_t *);
_Static_assert(sizeof(rtp_header_t) == 12, "vendor RTP header size");
_Static_assert(sizeof(rtp_encoder_t) == 1464, "vendor RTP encoder size");
_Static_assert(offsetof(rtp_encoder_t, buf) == 35, "vendor RTP encoder buffer offset");
_Static_assert(sizeof(rtp_decoder_t) == 40, "vendor RTP decoder size");
_Static_assert(sizeof(rtp_seq_src_t) == 24, "vendor RTP sequence source size");
_Static_assert(sizeof(rtp_seq_tracker_t) == 72, "vendor RTP sequence tracker size");
#endif

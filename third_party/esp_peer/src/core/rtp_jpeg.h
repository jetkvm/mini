#ifndef ESP_PEER_CORE_RTP_JPEG_H
#define ESP_PEER_CORE_RTP_JPEG_H
#include "rtp.h"
int rtp_jpeg_encode(rtp_encoder_t *, uint8_t *, size_t);
int rtp_jpeg_decode(rtp_decoder_t *, uint8_t *, size_t);
#endif

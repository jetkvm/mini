/*
 * RTP packetization adapted from libpeer (MIT).
 * Copyright (c) 2021 sepfy
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "peer_platform.h"
#include <string.h>
#include "rtp.h"
#include "rtp_jpeg.h"
#define CONFIG_MTU 1300
#define RTP_PAYLOAD_SIZE (CONFIG_MTU-sizeof(rtp_header_t)-rtp_encoder->ext_reserve)
#define FU_PAYLOAD_SIZE (CONFIG_MTU-sizeof(rtp_header_t)-sizeof(FuHeader)-sizeof(NaluHeader)-rtp_encoder->ext_reserve)
typedef enum RtpH264Type { NALU=23,FU_A=28 } RtpH264Type;
typedef struct NaluHeader { uint8_t type:5,nri:2,f:1; } NaluHeader;
typedef struct FuHeader { uint8_t type:5,r:1,e:1,s:1; } FuHeader;

int rtp_packet_validate(uint8_t *packet,size_t size) {
    if (size<8) return 0;
    return packet[0]>=128 && packet[0]<192;
}
uint32_t rtp_get_ssrc(uint8_t *packet) { rtp_header_t *rtp_header=(rtp_header_t *)packet; return ntohl(rtp_header->ssrc); }
uint16_t rtp_packet_get_seq_number(uint8_t *packet) { rtp_header_t *rtp_header=(rtp_header_t *)packet; return ntohs(rtp_header->seq_number); }
static rtp_seq_src_t *rtp_seq_get_src(rtp_seq_tracker_t *t,uint32_t ssrc) {
    rtp_seq_src_t *free_slot=NULL;
    for (int i=0;i<3;i++) {
        if (t->src[i].used && t->src[i].ssrc==ssrc) return &t->src[i];
        if (!free_slot && !t->src[i].used) free_slot=&t->src[i];
    }
    return free_slot;
}
static void rtp_seq_src_free_held(rtp_seq_src_t *src) {
    if (src->held) { media_lib_free(src->held); src->held=NULL; }
    src->held_size=0; src->held_cap=0;
}
static int rtp_seq_hold(rtp_seq_src_t *src,uint8_t *pkt,uint32_t size) {
    if (size>src->held_cap) {
        uint8_t *buf=media_lib_realloc(src->held,size);
        if (!buf) return -1;
        src->held=buf; src->held_cap=size;
    }
    memcpy(src->held,pkt,size); src->held_size=size; return 0;
}
int rtp_seq_filter(rtp_seq_tracker_t *t,uint8_t *pkt,uint32_t size,uint8_t **held,uint32_t *held_size) {
    if (!t || !pkt || size<12) return 1;
    uint32_t ssrc=rtp_get_ssrc(pkt);
    uint16_t seq=rtp_packet_get_seq_number(pkt);
    rtp_seq_src_t *src=rtp_seq_get_src(t,ssrc);
    if (!src) return 1;
    if (!src->used) {
        src->used=true; src->synced=false; src->ssrc=ssrc; src->max_seq=seq; src->probation=1;
        if (rtp_seq_hold(src,pkt,size)!=0) { src->used=false; return 1; }
        return 0;
    }
    if (src->synced) {
        rtp_seq_src_free_held(src);
        if ((int16_t)(seq-src->max_seq)>0) src->max_seq=seq;
        return 1;
    }
    if (seq==(uint16_t)(src->max_seq+1)) {
        src->probation--; src->max_seq=seq;
        if (src->probation==0) {
            src->synced=true;
            if (held && held_size && src->held && src->held_size) {
                *held=src->held; *held_size=src->held_size; return 2;
            }
            rtp_seq_src_free_held(src); return 1;
        }
        if (rtp_seq_hold(src,pkt,size)!=0) {
            src->synced=true; rtp_seq_src_free_held(src); return 1;
        }
        return 0;
    }
    src->probation=1; src->max_seq=seq;
    if (rtp_seq_hold(src,pkt,size)!=0) {
        src->synced=true; rtp_seq_src_free_held(src); return 1;
    }
    return 0;
}
void rtp_seq_tracker_reset(rtp_seq_tracker_t *t) {
    if (!t) return;
    for (int i=0;i<3;i++) {
        rtp_seq_src_t *src=&t->src[i];
        rtp_seq_src_free_held(src); src->used=false; src->synced=false;
        src->ssrc=0; src->max_seq=0; src->probation=0;
    }
}
void rtp_seq_tracker_deinit(rtp_seq_tracker_t *t) { rtp_seq_tracker_reset(t); }
#define INIT_HEADER(header) do { (header)->version=2; (header)->padding=0; (header)->extension=0; (header)->csrccount=0; (header)->markerbit=0; (header)->type=rtp_encoder->type; } while (0)
static int rtp_encoder_encode_h264_single(rtp_encoder_t *rtp_encoder,uint8_t *buf,size_t size) {
    rtp_packet_t *rtp_packet=(rtp_packet_t *)rtp_encoder->buf;
    INIT_HEADER(&rtp_packet->header);
    rtp_packet->header.seq_number=htons(rtp_encoder->seq_number++);
    rtp_packet->header.timestamp=htonl(rtp_encoder->timestamp);
    rtp_packet->header.ssrc=htonl(rtp_encoder->ssrc);
    if ((*buf&0x1f)==5 || (*buf&0x1f)==1) rtp_packet->header.markerbit=1;
    memcpy(rtp_packet->payload+rtp_encoder->ext_reserve,buf,size);
    rtp_encoder->on_packet(rtp_encoder->buf,size+sizeof(rtp_header_t)+rtp_encoder->ext_reserve,rtp_encoder->user_data);
    return 0;
}
static int rtp_encoder_encode_h264_fu_a(rtp_encoder_t *rtp_encoder,uint8_t *buf,size_t size) {
    rtp_packet_t *rtp_packet=(rtp_packet_t *)rtp_encoder->buf;
    INIT_HEADER(&rtp_packet->header);
    rtp_packet->header.timestamp=htonl(rtp_encoder->timestamp);
    rtp_packet->header.ssrc=htonl(rtp_encoder->ssrc);
    uint8_t type=buf[0]&0x1f;
    uint8_t nri=(buf[0]&0x60)>>5;
    buf=buf+1; size=size-1;
    NaluHeader *fu_indicator=(NaluHeader *)(rtp_packet->payload+rtp_encoder->ext_reserve);
    FuHeader *fu_header=(FuHeader *)(rtp_packet->payload+rtp_encoder->ext_reserve+sizeof(NaluHeader));
    fu_header->s=1;
    while (size>0) {
        fu_indicator->type=FU_A; fu_indicator->nri=nri; fu_indicator->f=0;
        fu_header->type=type; fu_header->r=0;
        rtp_packet->header.extension=0;
        rtp_packet->header.seq_number=htons(rtp_encoder->seq_number++);
        if (size<=FU_PAYLOAD_SIZE) {
            fu_header->e=1; rtp_packet->header.markerbit=1;
            memcpy(rtp_packet->payload+sizeof(NaluHeader)+sizeof(FuHeader)+rtp_encoder->ext_reserve,buf,size);
            rtp_encoder->on_packet(rtp_encoder->buf,size+sizeof(rtp_header_t)+sizeof(NaluHeader)+sizeof(FuHeader)+rtp_encoder->ext_reserve,rtp_encoder->user_data);
            break;
        }
        fu_header->e=0;
        memcpy(rtp_packet->payload+sizeof(NaluHeader)+sizeof(FuHeader)+rtp_encoder->ext_reserve,buf,FU_PAYLOAD_SIZE);
        rtp_encoder->on_packet(rtp_encoder->buf,CONFIG_MTU,rtp_encoder->user_data);
        size-=FU_PAYLOAD_SIZE; buf+=FU_PAYLOAD_SIZE; fu_header->s=0;
    }
    return 0;
}
static uint8_t *h264_find_nalu(uint8_t *buf_start,uint8_t *buf_end) {
    uint8_t *p=buf_start+2;
    while (p<buf_end) { if (*(p-2)==0 && *(p-1)==0 && *p==1) return p+1; p++; }
    return buf_end;
}
static int rtp_encoder_encode_h264(rtp_encoder_t *rtp_encoder,uint8_t *buf,size_t size) {
    uint8_t *buf_end=buf+size; uint8_t *pstart,*pend; size_t nalu_size;
    for (pstart=h264_find_nalu(buf,buf_end);pstart<buf_end;pstart=pend) {
        pend=h264_find_nalu(pstart,buf_end); nalu_size=pend-pstart;
        if (pend!=buf_end) nalu_size--;
        while (pstart[nalu_size-1]==0) nalu_size--;
        if (nalu_size<=RTP_PAYLOAD_SIZE) rtp_encoder_encode_h264_single(rtp_encoder,pstart,nalu_size);
        else rtp_encoder_encode_h264_fu_a(rtp_encoder,pstart,nalu_size);
    }
    return 0;
}
static int rtp_encoder_encode_generic(rtp_encoder_t *rtp_encoder,uint8_t *buf,size_t size) {
    rtp_header_t *rtp_header=(rtp_header_t *)rtp_encoder->buf;
    INIT_HEADER(rtp_header);
    rtp_header->seq_number=htons(rtp_encoder->seq_number++);
    rtp_header->timestamp=htonl(rtp_encoder->timestamp);
    rtp_header->ssrc=htonl(rtp_encoder->ssrc);
    memcpy(rtp_encoder->buf+sizeof(rtp_header_t)+rtp_encoder->ext_reserve,buf,size);
    rtp_encoder->on_packet(rtp_encoder->buf,size+sizeof(rtp_header_t)+rtp_encoder->ext_reserve,rtp_encoder->user_data);
    return 0;
}
void rtp_encoder_init(rtp_encoder_t *rtp_encoder,rtp_payload_type_t codec,rtp_encoder_done_cb on_packet,void *user_data) {
    rtp_encoder->on_packet=on_packet; rtp_encoder->user_data=user_data;
    rtp_encoder->timestamp=0; rtp_encoder->seq_number=0;
    rtp_encoder->jpeg_width=0; rtp_encoder->jpeg_height=0; rtp_encoder->jpeg_type=0; rtp_encoder->jpeg_info_ready=false; rtp_encoder->ext_reserve=0;
    rtp_encoder->type=codec;
    switch (codec) {
    case PT_H264: case PT_H264_1: rtp_encoder->ssrc=SSRC_H264; rtp_encoder->encode_func=rtp_encoder_encode_h264; break;
    case PT_JPEG: rtp_encoder->ssrc=SSRC_JPEG; rtp_encoder->encode_func=rtp_jpeg_encode; break;
    case PT_PCMA: rtp_encoder->ssrc=SSRC_PCMA; rtp_encoder->encode_func=rtp_encoder_encode_generic; break;
    case PT_PCMU: rtp_encoder->ssrc=SSRC_PCMU; rtp_encoder->encode_func=rtp_encoder_encode_generic; break;
    case PT_OPUS: rtp_encoder->ssrc=SSRC_OPUS; rtp_encoder->encode_func=rtp_encoder_encode_generic; break;
    default: if (codec>=96 && codec<=127) { rtp_encoder->ssrc=SSRC_H264; rtp_encoder->encode_func=rtp_encoder_encode_h264; } break;
    }
}
void rtp_encoder_set_codec(rtp_encoder_t *rtp_encoder,rtp_payload_type_t codec) { rtp_encoder->type=codec; }
void rtp_encoder_set_ext_reserve(rtp_encoder_t *rtp_encoder,uint8_t bytes) { if (rtp_encoder) rtp_encoder->ext_reserve=bytes; }
void rtp_encoder_set_jpeg_info(rtp_encoder_t *rtp_encoder,uint16_t width,uint16_t height) { rtp_encoder->jpeg_width=width; rtp_encoder->jpeg_height=height; rtp_encoder->jpeg_info_ready=false; }
static uint32_t calc_timestamp(const rtp_encoder_t *enc,uint32_t time_stamp) {
    switch (enc->ssrc) { case SSRC_PCMA: case SSRC_PCMU: return time_stamp*8; case SSRC_OPUS: return time_stamp*48; default: return time_stamp*90; }
}
int rtp_encoder_encode(rtp_encoder_t *rtp_encoder,uint32_t time_stamp,uint8_t *buf,size_t size) {
    if (!rtp_encoder->encode_func) return -1;
    rtp_encoder->timestamp=calc_timestamp(rtp_encoder,time_stamp);
    return rtp_encoder->encode_func(rtp_encoder,buf,size);
}
static int rtp_decoder_append_frame(rtp_decoder_t *rtp_decoder,uint8_t *data,size_t size) {
    if (rtp_decoder->frame_size+size>rtp_decoder->frame_capacity) {
        uint32_t new_capacity=rtp_decoder->frame_capacity?rtp_decoder->frame_capacity*2:4096;
        while (new_capacity<rtp_decoder->frame_size+size) new_capacity=new_capacity*3/2;
        uint8_t *new_buf=media_lib_realloc(rtp_decoder->frame_buf,new_capacity);
        if (!new_buf) return -1;
        rtp_decoder->frame_buf=new_buf; rtp_decoder->frame_capacity=new_capacity;
    }
    memcpy(rtp_decoder->frame_buf+rtp_decoder->frame_size,data,size); rtp_decoder->frame_size+=size; return 0;
}
static int rtp_decode_h264(rtp_decoder_t *rtp_decoder,uint8_t *buf,size_t size) {
    rtp_packet_t *rtp_packet=(rtp_packet_t *)buf;
    if (size<sizeof(rtp_header_t)) return -1;
    size_t header_size=sizeof(rtp_header_t)+rtp_packet->header.csrccount*4;
    if (rtp_packet->header.extension) {
        if (size<header_size+4) return -1;
        uint16_t ext_len=ntohs(*(uint16_t *)(buf+header_size+2));
        header_size+=(ext_len+1)*4;
    }
    if (size<header_size) return -1;
    uint16_t seq=ntohs(rtp_packet->header.seq_number);
    uint32_t ts=ntohl(rtp_packet->header.timestamp);
    uint8_t *payload=buf+header_size;
    size_t payload_size=size-header_size;
    if (rtp_decoder->has_last_seq && (uint16_t)(seq-rtp_decoder->last_seq)!=1) rtp_decoder->frame_size=0;
    rtp_decoder->last_seq=seq; rtp_decoder->has_last_seq=true;
    if (rtp_decoder->has_last_ts && rtp_decoder->last_timestamp!=ts) rtp_decoder->frame_size=0;
    rtp_decoder->last_timestamp=ts; rtp_decoder->has_last_ts=true;
    if (payload_size==0) return -1;
    uint8_t type=payload[0]&0x1f;
    if (type>=1 && type<=23) {
        uint8_t start_code[4]={0,0,0,1};
        if (rtp_decoder_append_frame(rtp_decoder,start_code,4)!=0 || rtp_decoder_append_frame(rtp_decoder,payload,payload_size)!=0) {
            rtp_decoder->frame_size=0; return -1;
        }
    } else if (type==24) {
        size_t offset=1;
        while (offset+2<=payload_size) {
            uint16_t nalu_size=(payload[offset]<<8)|payload[offset+1];
            offset+=2;
            if (offset+nalu_size>payload_size) break;
            uint8_t start_code[4]={0,0,0,1};
            if (rtp_decoder_append_frame(rtp_decoder,start_code,4)!=0 || rtp_decoder_append_frame(rtp_decoder,payload+offset,nalu_size)!=0) {
                rtp_decoder->frame_size=0; return -1;
            }
            offset+=nalu_size;
        }
    } else if (type==FU_A) {
        if (payload_size<2) return -1;
        uint8_t fu_indicator=payload[0],fu_header=payload[1];
        bool s=(fu_header>>7)&1;
        uint8_t nalu_type=fu_header&0x1f;
        uint8_t nri=(fu_indicator>>5)&3;
        if (s) {
            uint8_t start_code[4]={0,0,0,1};
            if (rtp_decoder_append_frame(rtp_decoder,start_code,4)!=0) { rtp_decoder->frame_size=0; return -1; }
            uint8_t reconstructed_nalu_header=(nri<<5)|nalu_type;
            if (rtp_decoder_append_frame(rtp_decoder,&reconstructed_nalu_header,1)!=0) { rtp_decoder->frame_size=0; return -1; }
        } else if (rtp_decoder->frame_size==0) return 0;
        if (rtp_decoder_append_frame(rtp_decoder,payload+2,payload_size-2)!=0) { rtp_decoder->frame_size=0; return -1; }
    }
    if (rtp_packet->header.markerbit) {
        rtp_packet->header.seq_number=seq; rtp_packet->header.timestamp=ts; rtp_packet->header.ssrc=ntohl(rtp_packet->header.ssrc);
        rtp_decoder->on_packet(&rtp_packet->header,rtp_decoder->frame_buf,rtp_decoder->frame_size,rtp_decoder->user_data);
        rtp_decoder->frame_size=0; rtp_decoder->has_last_ts=false;
    }
    return 0;
}
static int rtp_decode_generic(rtp_decoder_t *rtp_decoder,uint8_t *buf,size_t size) {
    rtp_packet_t *rtp_packet=(rtp_packet_t *)buf;
    if (size<sizeof(rtp_header_t)) return -1;
    size_t header_size=sizeof(rtp_header_t)+rtp_packet->header.csrccount*4;
    if (rtp_packet->header.extension) {
        if (size<header_size+4) return -1;
        uint16_t ext_len=ntohs(*(uint16_t *)(buf+header_size+2));
        header_size+=(ext_len+1)*4;
    }
    if (size<header_size) return -1;
    rtp_packet->header.seq_number=ntohs(rtp_packet->header.seq_number);
    rtp_packet->header.timestamp=ntohl(rtp_packet->header.timestamp);
    rtp_packet->header.ssrc=ntohl(rtp_packet->header.ssrc);
    rtp_decoder->on_packet(&rtp_packet->header,buf+header_size,size-header_size,rtp_decoder->user_data);
    return 0;
}
void rtp_encoder_deinit(rtp_encoder_t *rtp_encoder) { }
void rtp_decoder_set_codec(rtp_decoder_t *rtp_decoder,rtp_payload_type_t codec) {
    if (!rtp_decoder) return;
    rtp_decoder->type=codec; rtp_decoder->decode_func=NULL; rtp_decoder->frame_size=0;
    rtp_decoder->jpeg_hdr_size=0; rtp_decoder->has_last_seq=false; rtp_decoder->has_last_ts=false;
    switch (codec) {
    case PT_H264: case PT_H264_1: rtp_decoder->decode_func=rtp_decode_h264; break;
    case PT_JPEG: rtp_decoder->decode_func=rtp_jpeg_decode; break;
    case PT_PCMA: case PT_PCMU: case PT_OPUS: rtp_decoder->decode_func=rtp_decode_generic; break;
    default: if (codec>=96 && codec<=127) rtp_decoder->decode_func=rtp_decode_h264; break;
    }
}
void rtp_decoder_init(rtp_decoder_t *rtp_decoder,rtp_payload_type_t codec,rtc_decoder_done_cb on_packet,void *user_data) {
    memset(rtp_decoder,0,sizeof(*rtp_decoder)); rtp_decoder->on_packet=on_packet; rtp_decoder->user_data=user_data;
    rtp_decoder_set_codec(rtp_decoder,codec);
}
int rtp_decoder_decode(rtp_decoder_t *rtp_decoder,uint8_t *buf,size_t size) {
    if (rtp_decoder->decode_func==NULL) return -1;
    return rtp_decoder->decode_func(rtp_decoder,buf,size);
}
void rtp_decoder_deinit(rtp_decoder_t *rtp_decoder) {
    if (rtp_decoder->frame_buf) { media_lib_free(rtp_decoder->frame_buf); rtp_decoder->frame_buf=NULL; }
}

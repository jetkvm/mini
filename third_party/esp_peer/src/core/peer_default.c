/* peer default implementation.
 * See LICENSE and LICENSES/ for copyright and licensing notices.
 */
#include "peer_default.h"
#include "agent.h"
#include "rtp.h"
#include "sctp.h"
#include "sdp.h"
#include "rtp_jitter.h"
#include "rtp_rolling_buffer.h"
#include "twcc.h"
#include "rtcp.h"
#include "utils.h"
#include "peer_platform.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "dtls_srtp.h"

/* Module-private state recovered from the reference object. */
typedef struct {
    esp_peer_rtp_transform_cb_t cb;
    void *ctx;
    uint8_t *encoded_data;
    uint32_t encoded_capacity;
} peer_rtp_transformer_t;
typedef struct {
    esp_peer_cfg_t cfg;
    esp_peer_default_cfg_t extra_cfg;
    esp_peer_state_t state;
    agent_handle_t agent;
    dtls_srtp_t *dtls_srtp;
    sctp_handle_t sctp;
    uint8_t dtls_buf[1300];
    uint8_t agent_buf[1300];
    int agent_ret;
    int b_offer_created;
    int b_remote_sdp_got;
    int remote_candidate_count;
    rtp_encoder_t artp_encoder;
    rtp_encoder_t vrtp_encoder;
    rtp_decoder_t vrtp_decoder;
    rtp_decoder_t artp_decoder;
    uint32_t remote_assrc;
    uint32_t remote_vssrc;
    uint32_t remote_vrtx_ssrc;
    uint8_t aud_mid;
    uint8_t vid_mid;
    uint8_t data_mid;
    uint8_t stream_count;
    bool data_channel_accepted;
    int aud_rtp_port;
    int vid_rtp_port;
    rtp_payload_type_t peer_vid_fmt;
    rtp_payload_type_t peer_vid_rtx_fmt;
    rtp_payload_type_t peer_vid_rtx_apt;
    uint32_t peer_vid_profile;
    dtls_srtp_role_t peer_role;
    esp_peer_media_dir_t remote_audio_dir;
    esp_peer_media_dir_t remote_video_dir;
    char *remote_fingerprint;
    rtp_rolling_buf_handle_t *rtp_cache;
    rtp_jitter_handle_t rtp_aud_jitter;
    rtp_jitter_handle_t rtp_vid_jitter;
    rtp_seq_tracker_t rtp_seq;
    bool looping;
    media_lib_mutex_handle_t api_lock;
    peer_rtp_transformer_t rtp_send_transformer;
    peer_rtp_transformer_t rtp_recv_transformer;
    uint16_t send_num;
    uint16_t receive_num;
    uint32_t send_size;
    uint32_t receive_size;
    uint32_t data_send_size;
    uint32_t data_recv_size;
    uint16_t video_pli_send_interval;
    uint32_t video_pli_send_time;
    twcc_ctx_t *twcc;
    bool twcc_negotiated;
    uint8_t twcc_ext_id;
    bool rtp_rx_pending;
    uint32_t nack_due_ms;
    uint8_t rtp_drop_pct;
    uint32_t rtp_drop_count;
    uint8_t codec_fmt_id[5];
    uint8_t prefer_codec_caps;
    uint8_t aud_first_fmt;
    uint8_t vid_first_fmt;
    uint32_t recv_audio_sample_rate;
    bool audio_info_done;
    bool video_info_done;
} peer_t;
typedef struct {
    const char *ufrag;
    const char *pwd;
    const char *fingerprint;
    esp_peer_media_dir_t audio_dir;
    esp_peer_media_dir_t video_dir;
    uint8_t aud_mid;
    uint8_t vid_mid;
    uint8_t data_mid;
    uint8_t stream_count;
    bool data_channel_accepted;
    rtp_payload_type_t peer_vid_fmt;
    rtp_payload_type_t peer_vid_rtx_fmt;
    rtp_payload_type_t peer_vid_rtx_apt;
    uint32_t peer_vid_profile;
    uint32_t aud_ssrc;
    uint32_t vssrc;
    uint32_t vrtx_ssrc;
    int aud_rtp_port;
    int vid_rtp_port;
    dtls_srtp_role_t peer_role;
    uint8_t twcc_ext_id;
    uint8_t codec_fmt_id[5];
    uint8_t aud_first_fmt;
    uint8_t vid_first_fmt;
} peer_sdp_info_t;

/* Retain the recovered helper surface for independent function comparison. */
#define LOCAL static

#define esp_log(level, ...) esp_log((esp_log_config_t){.data=(level)}, __VA_ARGS__)
int pc_data_channel_on_write(void *ctx, const uint8_t *buf, int len)
{
    peer_t *pc=ctx;
    int ret=dtls_srtp_write(pc->dtls_srtp,buf,len);
    if (ret<0) esp_log(1,"PEER_DEF","E (%lu) %s: Write fail -%x\n",esp_log_timestamp(),"PEER_DEF",-ret);
    sctp_check_data(pc->sctp,buf,len);
    return ret;
}

bool peer_tracing;
LOCAL int dtls_send(void *ctx, const uint8_t *buf, size_t len)
{
    peer_t *pc=((dtls_srtp_t *)ctx)->ctx;
    measure_start("agent_send");
    int ret=agent_send(pc->agent,buf,len);
    measure_stop("agent_send");
    pc->send_num++;
    if (ret>0) pc->send_size+=ret;
    if (peer_tracing) printf("Send %d\n",ret);
    if (ret==-200) return -0x6880;
    return ret;
}

enum { PEER_FMT_AUD_G711A, PEER_FMT_AUD_G711U, PEER_FMT_AUD_OPUS,
       PEER_FMT_VID_H264, PEER_FMT_VID_MJPEG, PEER_FMT_N };
typedef enum { PEER_MEDIA_AUDIO, PEER_MEDIA_VIDEO } peer_media_type_t;
typedef struct {
    uint32_t ssrc;
    uint8_t codec, caps, fmt_idx, default_pt;
} peer_codec_desc_t;
static const peer_codec_desc_t s_peer_codecs[] = {
    {4, ESP_PEER_AUDIO_CODEC_G711A, 1, PEER_FMT_AUD_G711A, PT_PCMA},
    {5, ESP_PEER_AUDIO_CODEC_G711U, 2, PEER_FMT_AUD_G711U, PT_PCMU},
    {6, ESP_PEER_AUDIO_CODEC_OPUS, 4, PEER_FMT_AUD_OPUS, PT_OPUS},
    {1, ESP_PEER_VIDEO_CODEC_H264, 8, PEER_FMT_VID_H264, PT_H264},
    {2, ESP_PEER_VIDEO_CODEC_MJPEG, 16, PEER_FMT_VID_MJPEG, PT_JPEG},
};

LOCAL bool peer_fmt_ok(uint8_t v) { return v < 128; }
LOCAL peer_media_type_t peer_codec_media(const peer_codec_desc_t *desc)
{ return (desc->caps & 7) ? PEER_MEDIA_AUDIO : PEER_MEDIA_VIDEO; }
LOCAL const peer_codec_desc_t *peer_find_codec(peer_media_type_t media, uint8_t codec)
{
    for (unsigned i = 0; i < PEER_FMT_N; i++) {
        const peer_codec_desc_t *desc = &s_peer_codecs[i];
        if (peer_codec_media(desc) == media && desc->codec == codec) return desc;
    }
    return NULL;
}
LOCAL const peer_codec_desc_t *peer_find_codec_by_pt(const peer_t *pc,
    peer_media_type_t media, uint8_t pt)
{
    for (unsigned i = 0; i < PEER_FMT_N; i++) {
        const peer_codec_desc_t *desc = &s_peer_codecs[i];
        if (peer_codec_media(desc) == media && pc->codec_fmt_id[desc->fmt_idx] == pt) return desc;
    }
    return NULL;
}
LOCAL uint8_t peer_aud_caps(const peer_t *pc)
{
    if (!pc->cfg.audio_dir) return 0;
    const peer_codec_desc_t *desc = peer_find_codec(PEER_MEDIA_AUDIO, pc->cfg.audio_info.codec);
    return (pc->cfg.codec_caps & 7) | (desc ? (unsigned)desc->caps : 0);
}
LOCAL uint8_t peer_vid_caps(const peer_t *pc)
{
    if (!pc->cfg.video_dir) return 0;
    const peer_codec_desc_t *desc = peer_find_codec(PEER_MEDIA_VIDEO, pc->cfg.video_info.codec);
    return (pc->cfg.codec_caps & 24) | (desc ? (unsigned)desc->caps : 0);
}
LOCAL uint8_t peer_fmt_pt(const uint8_t *ids, uint8_t slot, uint8_t fallback)
{ if (slot < PEER_FMT_N && peer_fmt_ok(ids[slot])) return ids[slot]; return fallback; }
LOCAL void peer_reset_codec_fmt(uint8_t *ids)
{ for (int i = 0; i < PEER_FMT_N; i++) ids[i] = 255; }
LOCAL dtls_srtp_role_t peer_get_local_dtls_role(peer_t *pc)
{
    if (!pc->b_remote_sdp_got) return DTLS_SRTP_ROLE_SERVER;
    if (pc->peer_role == DTLS_SRTP_ROLE_SERVER) return DTLS_SRTP_ROLE_CLIENT;
    return DTLS_SRTP_ROLE_SERVER;
}
LOCAL rtp_payload_type_t get_video_codec(esp_peer_video_codec_t codec)
{ const peer_codec_desc_t *desc = peer_find_codec(PEER_MEDIA_VIDEO, codec); return desc ? desc->default_pt : 0; }
LOCAL rtp_payload_type_t get_audio_codec(esp_peer_audio_codec_t codec)
{ const peer_codec_desc_t *desc = peer_find_codec(PEER_MEDIA_AUDIO, codec); return desc ? desc->default_pt : 0; }
LOCAL uint32_t calc_audio_pts(peer_t *pc, uint32_t timestamp)
{
    uint32_t sample_rate = pc->recv_audio_sample_rate;
    if (!sample_rate) sample_rate = pc->cfg.audio_info.sample_rate;
    if (!sample_rate) sample_rate = 8000;
    return (uint64_t)timestamp * 1000 / sample_rate;
}
LOCAL void on_audio(rtp_header_t *header, uint8_t *payload, size_t size, void *userdata)
{
    peer_t *pc = userdata;
    if (pc->cfg.on_audio_data) {
        esp_peer_audio_frame_t frame = {calc_audio_pts(pc, header->timestamp), payload, size};
        pc->cfg.on_audio_data(&frame, pc->cfg.ctx);
    }
}
LOCAL uint32_t calc_video_pts(uint32_t timestamp) { return timestamp / 90; }
LOCAL void on_video(rtp_header_t *header, uint8_t *payload, size_t size, void *userdata)
{
    peer_t *pc = userdata;
    if (pc->cfg.on_video_data) {
        esp_peer_video_frame_t frame = {calc_video_pts(header->timestamp), payload, size};
        pc->cfg.on_video_data(&frame, pc->cfg.ctx);
    }
}
LOCAL esp_peer_media_dir_t get_compatible_direction(esp_peer_media_dir_t offer_dir,
    esp_peer_media_dir_t local_dir)
{
    if (offer_dir == ESP_PEER_MEDIA_DIR_SEND_RECV) return local_dir;
    if (offer_dir == ESP_PEER_MEDIA_DIR_SEND_ONLY) {
        if (local_dir == ESP_PEER_MEDIA_DIR_RECV_ONLY || local_dir == ESP_PEER_MEDIA_DIR_SEND_RECV) return ESP_PEER_MEDIA_DIR_RECV_ONLY;
        return ESP_PEER_MEDIA_DIR_NONE;
    }
    if (offer_dir == ESP_PEER_MEDIA_DIR_RECV_ONLY) {
        if (local_dir == ESP_PEER_MEDIA_DIR_SEND_ONLY || local_dir == ESP_PEER_MEDIA_DIR_SEND_RECV) return ESP_PEER_MEDIA_DIR_SEND_ONLY;
        return ESP_PEER_MEDIA_DIR_NONE;
    }
    return ESP_PEER_MEDIA_DIR_NONE;
}
LOCAL bool verify_config(esp_peer_cfg_t *cfg)
{
    if (cfg->audio_dir && cfg->audio_info.codec == ESP_PEER_AUDIO_CODEC_NONE) return false;
    if (cfg->video_dir && cfg->video_info.codec == ESP_PEER_VIDEO_CODEC_NONE) return false;
    if (cfg->server_num == 0 && cfg->server_lists == NULL) return true;
    if (cfg->server_num == 0) return false;
    if (cfg->server_lists == NULL) return false;
    if (cfg->extra_cfg && cfg->extra_size != sizeof(esp_peer_default_cfg_t)) return false;
    for (int i = 0; i < cfg->server_num; i++) if (cfg->server_lists[i].stun_url == NULL) return false;
    if (!cfg->on_msg) return false;
    return true;
}
LOCAL uint32_t get_aud_ssrc(esp_peer_cfg_t *cfg)
{ const peer_codec_desc_t *desc = peer_find_codec(PEER_MEDIA_AUDIO, cfg->audio_info.codec); return desc ? desc->ssrc : 0; }
LOCAL uint32_t get_vid_ssrc(esp_peer_cfg_t *cfg)
{ const peer_codec_desc_t *desc = peer_find_codec(PEER_MEDIA_VIDEO, cfg->video_info.codec); return desc ? desc->ssrc : 0; }
LOCAL int peer_set_state(peer_t *pc, esp_peer_state_t state)
{ pc->state = state; if (pc->cfg.on_state) pc->cfg.on_state(state, pc->cfg.ctx); return 0; }
LOCAL int peer_new_connection(void *handle)
{ peer_t *pc = handle; if (!pc) return -1; peer_set_state(pc, ESP_PEER_STATE_NEW_CONNECTION); pc->b_offer_created = 0; return 0; }
LOCAL bool peer_remote_track_enabled(esp_peer_media_dir_t dir)
{ return dir & ESP_PEER_MEDIA_DIR_SEND_ONLY; }
LOCAL bool same_str(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b && *a != '\r' && *b != '\r') {
        if (*a != *b) return false;
        a++; b++;
    }
    return (!*a || *a == '\r') && (!*b || *b == '\r');
}
LOCAL bool peer_twcc_advertise(peer_t *pc)
{ if (!pc->extra_cfg.twcc_cfg.enable) return false; if (!pc->b_remote_sdp_got) return true; return pc->twcc_negotiated; }
LOCAL uint8_t peer_twcc_ext_id(peer_t *pc)
{ if (pc->twcc_negotiated && pc->twcc_ext_id) return pc->twcc_ext_id; return 3; }
LOCAL bool peer_remote_session_unchanged(peer_sdp_info_t *old_info, peer_sdp_info_t *new_info)
{ return same_str(old_info->ufrag,new_info->ufrag) && same_str(old_info->pwd,new_info->pwd) && same_str(old_info->fingerprint,new_info->fingerprint); }
LOCAL void peer_notify_remote_track_change(peer_t *pc, peer_sdp_info_t *old_info, peer_sdp_info_t *new_info)
{
    if (!pc->cfg.on_state) return;
    bool old_audio = peer_remote_track_enabled(old_info->audio_dir);
    bool new_audio = peer_remote_track_enabled(new_info->audio_dir);
    if (old_audio != new_audio) pc->cfg.on_state(new_audio ? ESP_PEER_STATE_REMOTE_AUDIO_TRACK_ADDED : ESP_PEER_STATE_REMOTE_AUDIO_TRACK_REMOVED, pc->cfg.ctx);
    bool old_video = peer_remote_track_enabled(old_info->video_dir);
    bool new_video = peer_remote_track_enabled(new_info->video_dir);
    if (old_video != new_video) pc->cfg.on_state(new_video ? ESP_PEER_STATE_REMOTE_VIDEO_TRACK_ADDED : ESP_PEER_STATE_REMOTE_VIDEO_TRACK_REMOVED, pc->cfg.ctx);
}
LOCAL esp_peer_media_dir_t peer_limit_send_direction(esp_peer_media_dir_t dir, const uint8_t *ids, uint8_t send_slot)
{
    if ((dir & ESP_PEER_MEDIA_DIR_SEND_ONLY) && (send_slot >= PEER_FMT_N || !peer_fmt_ok(ids[send_slot]))) dir &= ~ESP_PEER_MEDIA_DIR_SEND_ONLY;
    return dir;
}
LOCAL bool get_default_aud_info(esp_peer_audio_codec_t codec, esp_peer_audio_stream_info_t *aud_info)
{
    if (!aud_info) return false;
    *aud_info = (esp_peer_audio_stream_info_t){0}; aud_info->codec = codec;
    switch (codec) {
    case ESP_PEER_AUDIO_CODEC_G711A: case ESP_PEER_AUDIO_CODEC_G711U:
        aud_info->sample_rate=8000; aud_info->channel=1; return true;
    case ESP_PEER_AUDIO_CODEC_OPUS: aud_info->sample_rate=48000; aud_info->channel=2; return true;
    default: return false;
    }
}
_Static_assert(sizeof(peer_t) == 6080, "vendor peer ABI size");
_Static_assert(offsetof(peer_t, codec_fmt_id) == 6064, "vendor format map offset");
_Static_assert(sizeof(peer_sdp_info_t) == 76, "vendor SDP snapshot size");

LOCAL bool peer_remote_has_fmt(const peer_t *pc, uint8_t fmt_idx)
{ return !pc->b_remote_sdp_got || peer_fmt_ok(pc->codec_fmt_id[fmt_idx]); }
LOCAL uint32_t peer_sdp_h264_profile(const peer_t *pc)
{ if (pc->peer_vid_profile) return pc->peer_vid_profile; if (pc->b_remote_sdp_got) return 0x4d001f; return 0; }
LOCAL void pc_data_channel_connected(bool connected, void *ctx)
{
    peer_t *pc = ctx;
    if (pc->cfg.on_state) pc->cfg.on_state(connected ? ESP_PEER_STATE_DATA_CHANNEL_CONNECTED : ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED, pc->cfg.ctx);
}
LOCAL void pc_data_channel_opened(esp_peer_data_channel_info_t *ch, void *ctx)
{
    peer_t *pc = ctx;
    if (pc->cfg.on_channel_open) pc->cfg.on_channel_open(ch,pc->cfg.ctx);
    if (pc->cfg.on_state) pc->cfg.on_state(ESP_PEER_STATE_DATA_CHANNEL_OPENED,pc->cfg.ctx);
}
LOCAL void pc_data_channel_closed(esp_peer_data_channel_info_t *ch, void *ctx)
{
    peer_t *pc = ctx;
    if (pc->cfg.on_channel_close) pc->cfg.on_channel_close(ch,pc->cfg.ctx);
    if (pc->cfg.on_state) pc->cfg.on_state(ESP_PEER_STATE_DATA_CHANNEL_CLOSED,pc->cfg.ctx);
}
LOCAL void pc_data_channel_on_msg(sctp_data_ppid_t ppid, uint16_t stream_id, uint8_t *msg, int len, void *ctx)
{
    peer_t *pc = ctx;
    esp_peer_data_frame_t frame = {ppid == 51 ? ESP_PEER_DATA_CHANNEL_STRING : ESP_PEER_DATA_CHANNEL_DATA, stream_id, msg, len};
    if (pc->cfg.on_data) pc->cfg.on_data(&frame,pc->cfg.ctx);
}
LOCAL int peer_set_rtp_transformer(esp_peer_handle_t handle, esp_peer_rtp_transform_role_t role,
    esp_peer_rtp_transform_cb_t *transform_cb, void *ctx)
{
    peer_t *pc = handle;
    if (!pc) return ESP_PEER_ERR_INVALID_ARG;
    if (transform_cb && (!transform_cb->transform || !transform_cb->get_encoded_size)) return ESP_PEER_ERR_INVALID_ARG;
    if (role > 1) return ESP_PEER_ERR_INVALID_ARG;
    if (pc->state == ESP_PEER_STATE_CONNECTED) return ESP_PEER_ERR_WRONG_STATE;
    peer_rtp_transformer_t *transformer = role == 0 ? &pc->rtp_send_transformer : &pc->rtp_recv_transformer;
    if (transform_cb) { transformer->cb=*transform_cb; transformer->ctx=ctx; }
    else transformer->cb.transform=NULL;
    return 0;
}
LOCAL void peer_backup_sdp_info(peer_t *pc, peer_sdp_info_t *info)
{
    info->audio_dir=pc->remote_audio_dir;
    info->video_dir=pc->remote_video_dir;
    info->fingerprint=pc->remote_fingerprint;
    memcpy(info->codec_fmt_id,pc->codec_fmt_id,sizeof(info->codec_fmt_id));
    agent_get_remote_ice(pc->agent,&info->ufrag,&info->pwd);
}
LOCAL void peer_reset_changed_codec_info(peer_t *pc, const peer_sdp_info_t *old_info,
    const peer_sdp_info_t *new_info)
{
    if (memcmp(old_info->codec_fmt_id,new_info->codec_fmt_id,3)) {pc->audio_info_done=false;pc->recv_audio_sample_rate=0;}
    if (memcmp(old_info->codec_fmt_id+3,new_info->codec_fmt_id+3,2)) pc->video_info_done=false;
}
LOCAL char *dup_str(const char *str)
{
    int n=0;
    while (str[n] && str[n]!='\r') n++;
    char *copy=media_lib_malloc(n+1);
    if (copy) {memcpy(copy,str,n);copy[n]=0;}
    return copy;
}
LOCAL void peer_set_twcc_reserve(peer_t *pc, uint8_t bytes)
{
    rtp_encoder_set_ext_reserve(&pc->artp_encoder,bytes);
    rtp_encoder_set_ext_reserve(&pc->vrtp_encoder,bytes);
    if (pc->twcc) twcc_set_ext_reserved(pc->twcc,bytes != 0);
}
int peer_default_get_paired_addr(esp_peer_handle_t handle, esp_peer_addr_t *addr)
{
    peer_t *pc=handle;
    if (!pc) return ESP_PEER_ERR_INVALID_ARG;
    if (pc->state < ESP_PEER_STATE_PAIRED) return ESP_PEER_ERR_WRONG_STATE;
    return agent_get_paired(pc->agent,addr);
}
int peer_default_set_peer_prefer_codec(esp_peer_handle_t handle, esp_peer_codec_caps_t codec_caps)
{
    peer_t *pc=handle;
    if (!pc) return ESP_PEER_ERR_INVALID_ARG;
    pc->prefer_codec_caps=codec_caps;
    return 0;
}
LOCAL int peer_parse_sdp_pt(const char *line, const char *prefix)
{
    const char *value=line+strlen(prefix);
    char *end=NULL;
    long pt=strtol(value,&end,10);
    return end == value || *end != ' ' || pt < 0 || pt > 127 ? -1 : pt;
}
#include <strings.h>
LOCAL bool peer_rtpmap_is(const char *line, const char *encoding_name)
{
    const char *encoding=strchr(line,' ');
    if (!encoding) return false;
    encoding++;
    size_t name_len=strlen(encoding_name);
    return strcspn(encoding," ") == name_len && strncasecmp(encoding,encoding_name,name_len)==0;
}
LOCAL bool peer_mline_has_pt(const char *line, uint8_t wanted_pt)
{
    const char *value=line;
    for (int i=0; i<3; i++) {
        value=strchr(value,' ');
        if (!value) return false;
        while (*value==' ') value++;
    }
    while (*value) {
        char *end=NULL;
        long pt=strtol(value,&end,10);
        if (end==value) return false;
        if (pt==wanted_pt) return true;
        value=end;
        while (*value==' ') value++;
    }
    return false;
}
LOCAL void peer_gather_add_video(peer_t *pc, sdp_video_codec_t *out, int *idx, const peer_codec_desc_t *desc)
{
    out[*idx].codec=(esp_peer_video_codec_t)desc->codec;
    out[*idx].pt=peer_fmt_pt(pc->codec_fmt_id,desc->fmt_idx,desc->default_pt);
    out[*idx].h264_profile=desc->codec == ESP_PEER_VIDEO_CODEC_H264 ? peer_sdp_h264_profile(pc) : 0;
    (*idx)++;
}
LOCAL void peer_update_rtp_encoder(peer_t *pc)
{
    if (pc->cfg.audio_info.codec) {
        const peer_codec_desc_t *desc=peer_find_codec(PEER_MEDIA_AUDIO,pc->cfg.audio_info.codec);
        if (desc) rtp_encoder_set_codec(&pc->artp_encoder,peer_fmt_pt(pc->codec_fmt_id,desc->fmt_idx,desc->default_pt));
    }
    if (pc->cfg.video_info.codec) {
        const peer_codec_desc_t *desc=peer_find_codec(PEER_MEDIA_VIDEO,pc->cfg.video_info.codec);
        if (desc) rtp_encoder_set_codec(&pc->vrtp_encoder,peer_fmt_pt(pc->codec_fmt_id,desc->fmt_idx,desc->default_pt));
    }
}
LOCAL void free_server_cfg(peer_t *pc)
{
    esp_peer_ice_server_cfg_t *servers=pc->cfg.server_lists;
    if (servers) {
        for (int i=0; i<pc->cfg.server_num; i++) {
            if (servers[i].stun_url) {media_lib_free(servers[i].stun_url);servers[i].stun_url=NULL;}
            if (servers[i].user) {media_lib_free(servers[i].user);servers[i].user=NULL;}
            if (servers[i].psw) {media_lib_free(servers[i].psw);servers[i].psw=NULL;}
        }
        if (pc->cfg.server_lists) {media_lib_free(pc->cfg.server_lists);pc->cfg.server_lists=NULL;}
        pc->cfg.server_num=0;
    }
}
LOCAL void clear_peer_buf(peer_t *pc)
{
    pc->agent_ret=0;
    pc->remote_candidate_count=0;
    pc->receive_num=0;
    pc->send_num=0;
    pc->send_size=0;
    pc->receive_size=0;
    pc->data_send_size=0;
    pc->data_recv_size=0;
    pc->aud_mid=255;
    pc->vid_mid=255;
    pc->data_mid=255;
    pc->stream_count=0;
    pc->data_channel_accepted=false;
    pc->peer_vid_fmt=0;
    pc->peer_vid_rtx_apt=0;
    pc->peer_vid_profile=0;
    peer_reset_codec_fmt(pc->codec_fmt_id);
    pc->aud_first_fmt=255;
    pc->vid_first_fmt=255;
    pc->recv_audio_sample_rate=0;
    pc->audio_info_done=false;
    pc->video_info_done=false;
    pc->aud_rtp_port=9;
    pc->vid_rtp_port=9;
    if (pc->remote_fingerprint) media_lib_free(pc->remote_fingerprint);
    pc->remote_fingerprint=NULL;
}
LOCAL uint8_t peer_single_codec(peer_media_type_t media, const uint8_t *ids, uint8_t caps)
{
    const peer_codec_desc_t *found = NULL;
    if (!caps) {
        for (size_t i = 0; i < PEER_FMT_N; i++) {
            const peer_codec_desc_t *desc = &s_peer_codecs[i];
            if (peer_codec_media(desc) != media || !peer_fmt_ok(ids[desc->fmt_idx])) continue;
            if (found) return 0;
            found = desc;
        }
        if (found) return found->codec;
        return 0;
    }
    if ((caps & (caps - 1)) == 0) {
        for (size_t i = 0; i < PEER_FMT_N; i++) {
            const peer_codec_desc_t *desc = &s_peer_codecs[i];
            if (peer_codec_media(desc) == media && desc->caps == caps) return desc->codec;
        }
    }
    return 0;
}
LOCAL void peer_notify_audio(peer_t *pc, esp_peer_audio_codec_t codec)
{
    if (pc->audio_info_done || !codec) return;
    const peer_codec_desc_t *desc=peer_find_codec(PEER_MEDIA_AUDIO,codec);
    esp_peer_audio_stream_info_t info={0};
    if (!desc || !get_default_aud_info(codec,&info)) return;
    pc->recv_audio_sample_rate=info.sample_rate;
    if (pc->artp_decoder.on_packet) rtp_decoder_set_codec(&pc->artp_decoder,desc->default_pt);
    pc->audio_info_done=true;
    if (pc->cfg.on_audio_info) pc->cfg.on_audio_info(&info,pc->cfg.ctx);
}
LOCAL void peer_notify_video(peer_t *pc, esp_peer_video_codec_t codec)
{
    if (pc->video_info_done || !codec) return;
    const peer_codec_desc_t *desc=peer_find_codec(PEER_MEDIA_VIDEO,codec);
    if (!desc) return;
    esp_peer_video_stream_info_t info=pc->cfg.video_info;
    info.codec=codec;
    if (pc->vrtp_decoder.on_packet) rtp_decoder_set_codec(&pc->vrtp_decoder,desc->default_pt);
    pc->video_info_done=true;
    if (pc->cfg.on_video_info) pc->cfg.on_video_info(&info,pc->cfg.ctx);
}
LOCAL void peer_notify_if_single(peer_t *pc)
{
    uint8_t aud_caps=pc->b_remote_sdp_got ? 0 : peer_aud_caps(pc);
    uint8_t vid_caps=pc->b_remote_sdp_got ? 0 : peer_vid_caps(pc);
    peer_notify_audio(pc,peer_single_codec(PEER_MEDIA_AUDIO,pc->codec_fmt_id,aud_caps));
    peer_notify_video(pc,peer_single_codec(PEER_MEDIA_VIDEO,pc->codec_fmt_id,vid_caps));
}
LOCAL void peer_apply_sdp_info(peer_t *pc, peer_sdp_info_t *info)
{
    pc->remote_audio_dir=info->audio_dir;
    pc->remote_video_dir=info->video_dir;
    if (pc->remote_fingerprint) {media_lib_free(pc->remote_fingerprint);pc->remote_fingerprint=NULL;}
    if (info->fingerprint) pc->remote_fingerprint=dup_str(info->fingerprint);
    pc->peer_role=info->peer_role;
    pc->stream_count=info->stream_count;
    pc->aud_mid=info->aud_mid;
    pc->vid_mid=info->vid_mid;
    pc->data_mid=info->data_mid;
    pc->data_channel_accepted=info->data_channel_accepted;
    pc->aud_rtp_port=info->aud_rtp_port;
    pc->vid_rtp_port=info->vid_rtp_port;
    pc->remote_assrc=info->aud_ssrc;
    pc->remote_vssrc=info->vssrc;
    pc->remote_vrtx_ssrc=info->vrtx_ssrc;
    pc->peer_vid_fmt=info->peer_vid_fmt;
    pc->peer_vid_profile=info->peer_vid_profile;
    pc->peer_vid_rtx_fmt=info->peer_vid_rtx_fmt;
    pc->peer_vid_rtx_apt=info->peer_vid_rtx_apt;
    memcpy(pc->codec_fmt_id,info->codec_fmt_id,sizeof(pc->codec_fmt_id));
    pc->aud_first_fmt=info->aud_first_fmt;
    pc->vid_first_fmt=info->vid_first_fmt;
}
LOCAL int peer_set_data_sdp_rejected(peer_t *pc, sdp_t *sdp)
{ sdp_append_datachannel_rejected(sdp,pc->data_mid); return 0; }
LOCAL int peer_set_data_sdp(peer_t *pc, sdp_t *sdp)
{ sdp_append_datachannel(sdp,pc->data_mid,pc->cfg.enable_data_channel); return 0; }
LOCAL int peer_do_disconnect(peer_t *pc, bool inner_call)
{
    if (pc->state == ESP_PEER_STATE_DISCONNECTED) return 0;
    peer_set_state(pc,ESP_PEER_STATE_DISCONNECTED);
    pc->b_remote_sdp_got=0;
    pc->b_offer_created=0;
    pc->remote_candidate_count=0;
    agent_disconnect(pc->agent);
    while (!inner_call && pc->looping) media_lib_thread_sleep(10);
    if (pc->sctp) {sctp_destroy(pc->sctp);pc->sctp=NULL;}
    rtp_seq_tracker_reset(&pc->rtp_seq);
    if (!pc->extra_cfg.keep_role) pc->cfg.role=0;
    return 0;
}
LOCAL int peer_disconnect(esp_peer_handle_t handle)
{
    peer_t *pc=handle;
    if (!pc) return ESP_PEER_ERR_INVALID_ARG;
    media_lib_mutex_lock(pc->api_lock,UINT32_MAX);
    peer_do_disconnect(pc,false);
    clear_peer_buf(pc);
    media_lib_mutex_unlock(pc->api_lock);
    return 0;
}
LOCAL void peer_append_dtls_setup(peer_t *pc, sdp_t *local_sdp, dtls_srtp_role_t role, bool update)
{
    if (!pc->b_remote_sdp_got && !update) sdp_append(local_sdp,"a=setup:actpass");
    else if (role == DTLS_SRTP_ROLE_CLIENT) sdp_append(local_sdp,"a=setup:active");
    else sdp_append(local_sdp,"a=setup:passive");
}
LOCAL bool peer_notify_from_rtp(peer_t *pc, const uint8_t *payload, uint32_t size, bool is_audio)
{
    if (!payload || size < 12) return false;
    uint8_t pt=((const rtp_header_t *)payload)->type;
    if (is_audio) {
        const peer_codec_desc_t *desc=peer_find_codec_by_pt(pc,PEER_MEDIA_AUDIO,pt);
        if (!desc) return false;
        if (pc->audio_info_done) return pc->artp_decoder.type == desc->default_pt;
        peer_notify_audio(pc,desc->codec);
    } else {
        const peer_codec_desc_t *desc=peer_find_codec_by_pt(pc,PEER_MEDIA_VIDEO,pt);
        if (!desc) return false;
        if (pc->video_info_done) return pc->vrtp_decoder.type == desc->default_pt;
        peer_notify_video(pc,desc->codec);
    }
    return true;
}
LOCAL int rtp_data_reached(uint8_t *payload, uint32_t size, void *ctx)
{
    peer_t *pc=ctx;
    uint32_t ssrc=rtp_get_ssrc(payload);
    if (ssrc==pc->remote_assrc) {
        if (peer_notify_from_rtp(pc,payload,size,true)) rtp_decoder_decode(&pc->artp_decoder,payload,size);
    } else if (ssrc==pc->remote_vssrc) {
        if (peer_notify_from_rtp(pc,payload,size,false)) rtp_decoder_decode(&pc->vrtp_decoder,payload,size);
    }
    return 0;
}
LOCAL int check_rtp_transformer_capacity(peer_t *pc, peer_rtp_transformer_t *transformer, uint32_t new_size)
{
    if (new_size <= transformer->encoded_capacity) return 0;
    if (new_size > 1428) return ESP_PEER_ERR_NO_MEM;
    uint32_t capacity=new_size*3/2;
    if (capacity>1428) capacity=1428;
    uint8_t *buffer=media_lib_realloc(transformer->encoded_data,capacity);
    if (!buffer) return ESP_PEER_ERR_NO_MEM;
    transformer->encoded_data=buffer;
    transformer->encoded_capacity=capacity;
    return 0;
}
LOCAL int peer_close_data_channel(esp_peer_handle_t handle, const char *label)
{
    peer_t *pc=handle;
    if (!pc || !label) return ESP_PEER_ERR_INVALID_ARG;
    if (!pc->sctp) return ESP_PEER_ERR_WRONG_STATE;
    return sctp_close_channel(pc->sctp,label);
}
LOCAL int peer_create_data_channel(esp_peer_handle_t handle, esp_peer_data_channel_cfg_t *ch_cfg)
{
    peer_t *pc=handle;
    if (!pc) return ESP_PEER_ERR_INVALID_ARG;
    if (!pc->sctp) return ESP_PEER_ERR_WRONG_STATE;
    return sctp_create_channel(pc->sctp,ch_cfg);
}
LOCAL int peer_send_audio(esp_peer_handle_t handle, esp_peer_audio_frame_t *info)
{
    peer_t *pc=handle;
    if (!pc) return ESP_PEER_ERR_INVALID_ARG;
    const peer_codec_desc_t *desc=peer_find_codec(PEER_MEDIA_AUDIO,pc->cfg.audio_info.codec);
    uint8_t slot=desc ? desc->fmt_idx : PEER_FMT_N;
    if (pc->state != ESP_PEER_STATE_CONNECTED || !(pc->cfg.audio_dir & ESP_PEER_MEDIA_DIR_SEND_ONLY)) return ESP_PEER_ERR_WRONG_STATE;
    if (pc->b_remote_sdp_got && (slot>=PEER_FMT_N || !peer_fmt_ok(pc->codec_fmt_id[slot]))) return ESP_PEER_ERR_WRONG_STATE;
    return rtp_encoder_encode(&pc->artp_encoder,info->pts,info->data,info->size);
}
LOCAL int peer_send_video(esp_peer_handle_t handle, esp_peer_video_frame_t *info)
{
    peer_t *pc=handle;
    if (!pc) return ESP_PEER_ERR_INVALID_ARG;
    const peer_codec_desc_t *desc=peer_find_codec(PEER_MEDIA_VIDEO,pc->cfg.video_info.codec);
    uint8_t slot=desc ? desc->fmt_idx : PEER_FMT_N;
    if (pc->state != ESP_PEER_STATE_CONNECTED || !(pc->cfg.video_dir & ESP_PEER_MEDIA_DIR_SEND_ONLY)) return ESP_PEER_ERR_WRONG_STATE;
    if (pc->b_remote_sdp_got && (slot>=PEER_FMT_N || !peer_fmt_ok(pc->codec_fmt_id[slot]))) return ESP_PEER_ERR_WRONG_STATE;
    return rtp_encoder_encode(&pc->vrtp_encoder,info->pts,info->data,info->size);
}
LOCAL int peer_gather_audio(peer_t *pc, sdp_audio_codec_t *codecs, int max)
{
    uint8_t aud_caps = peer_aud_caps(pc);
    int idx = 0;
    uint8_t prefer = pc->prefer_codec_caps & aud_caps;
    for (size_t i = 0; prefer && idx < max && i < PEER_FMT_N; i++) {
        const peer_codec_desc_t *desc = &s_peer_codecs[i];
        if (peer_codec_media(desc) != PEER_MEDIA_AUDIO || !(prefer & desc->caps)) continue;
        prefer &= ~desc->caps;
        aud_caps &= ~desc->caps;
        if (!peer_remote_has_fmt(pc, desc->fmt_idx)) continue;
        codecs[idx].codec = (esp_peer_audio_codec_t)desc->codec;
        codecs[idx].pt = peer_fmt_pt(pc->codec_fmt_id, desc->fmt_idx, desc->default_pt);
        idx++;
    }
    const peer_codec_desc_t *send = peer_find_codec(PEER_MEDIA_AUDIO, pc->cfg.audio_info.codec);
    if (send && (aud_caps & send->caps)) {
        if (peer_remote_has_fmt(pc, send->fmt_idx)) {
            codecs[idx].codec = (esp_peer_audio_codec_t)send->codec;
            codecs[idx].pt = peer_fmt_pt(pc->codec_fmt_id, send->fmt_idx, send->default_pt);
            idx++;
        }
        aud_caps &= ~send->caps;
    }
    for (size_t i = 0; aud_caps && idx < max && i < PEER_FMT_N; i++) {
        const peer_codec_desc_t *desc = &s_peer_codecs[i];
        if (peer_codec_media(desc) != PEER_MEDIA_AUDIO || !(aud_caps & desc->caps)) continue;
        aud_caps &= ~desc->caps;
        if (!peer_remote_has_fmt(pc, desc->fmt_idx)) continue;
        codecs[idx].codec = (esp_peer_audio_codec_t)desc->codec;
        codecs[idx].pt = peer_fmt_pt(pc->codec_fmt_id, desc->fmt_idx, desc->default_pt);
        idx++;
    }
    return idx;
}

LOCAL int peer_gather_video(peer_t *pc, sdp_video_codec_t *out, int max)
{
    uint8_t vid_caps = peer_vid_caps(pc);
    int idx = 0;
    uint8_t prefer = pc->prefer_codec_caps & vid_caps;
    for (size_t i = 0; prefer && idx < max && i < PEER_FMT_N; i++) {
        const peer_codec_desc_t *desc = &s_peer_codecs[i];
        if (peer_codec_media(desc) != PEER_MEDIA_VIDEO || !(prefer & desc->caps)) continue;
        prefer &= ~desc->caps;
        vid_caps &= ~desc->caps;
        if (!peer_remote_has_fmt(pc, desc->fmt_idx)) continue;
        peer_gather_add_video(pc, out, &idx, desc);
    }
    const peer_codec_desc_t *send = peer_find_codec(PEER_MEDIA_VIDEO, pc->cfg.video_info.codec);
    if (send && (vid_caps & send->caps)) {
        if (peer_remote_has_fmt(pc, send->fmt_idx)) {
            peer_gather_add_video(pc, out, &idx, send);
        }
        vid_caps &= ~send->caps;
    }
    for (size_t i = 0; vid_caps && idx < max && i < PEER_FMT_N; i++) {
        const peer_codec_desc_t *desc = &s_peer_codecs[i];
        if (peer_codec_media(desc) != PEER_MEDIA_VIDEO || !(vid_caps & desc->caps)) continue;
        vid_caps &= ~desc->caps;
        if (!peer_remote_has_fmt(pc, desc->fmt_idx)) continue;
        peer_gather_add_video(pc, out, &idx, desc);
    }
    return idx;
}
int peer_default_get_ice_summary(esp_peer_handle_t handle, esp_peer_ice_summary_t *out)
{
    peer_t *pc=handle;
    if (!pc || !out) return ESP_PEER_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));
    agent_ice_selected_summary_t summary={0};
    int ret=agent_get_ice_selected_summary(pc->agent,&summary);
    if (ret) return ret;
    out->valid=summary.valid;
    out->local_candidate_type=summary.local_candidate_type;
    out->local_transport=summary.local_transport;
    out->pair_candidate_type=summary.pair_type;
    out->uses_tcp_transport=agent_is_tcp_connected(pc->agent);
    return 0;
}
LOCAL void peer_query(esp_peer_handle_t handle)
{
    peer_t *pc=handle;
    if (!pc || pc->state != ESP_PEER_STATE_CONNECTED) return;
    esp_log(3,"PEER_DEF","I (%lu) %s: Send %d:%d d_out:%d receive %d:%d din:%d\n",esp_log_timestamp(),"PEER_DEF",pc->send_num,(int)pc->send_size,(int)pc->data_send_size,pc->receive_num,(int)pc->receive_size,(int)pc->data_recv_size);
    pc->receive_num=0;
    pc->send_num=0;
    pc->data_recv_size=0;
    pc->data_send_size=0;
    pc->receive_size=0;
    pc->send_size=0;
    if (pc->sctp) sctp_query(pc->sctp);
}
LOCAL int peer_send_data(esp_peer_handle_t handle, esp_peer_data_frame_t *info)
{
    peer_t *pc=handle;
    if (!pc) return ESP_PEER_ERR_INVALID_ARG;
    if (!pc->sctp) return ESP_PEER_ERR_WRONG_STATE;
    if (!sctp_is_connected(pc->sctp)) {
        esp_log(1,"PEER_DEF","E (%lu) %s: sctp not connected\n",esp_log_timestamp(),"PEER_DEF");
        return ESP_PEER_ERR_INVALID_ARG;
    }
    if (info->type==ESP_PEER_DATA_CHANNEL_STRING) {
        int ret=sctp_outgoing_data(pc->sctp,info->data,info->size,info->stream_id,51);
        pc->data_send_size+=info->size;
        return ret;
    }
    int ret=sctp_outgoing_data(pc->sctp,info->data,info->size,info->stream_id,53);
    pc->data_send_size+=info->size;
    return ret;
}
LOCAL esp_peer_ice_server_cfg_t *copy_server_cfg(esp_peer_ice_server_cfg_t *servers, uint8_t num)
{
    esp_peer_ice_server_cfg_t *copy=media_lib_calloc(1,sizeof(*copy)*num);
    if (copy) {
        memcpy(copy,servers,sizeof(*copy)*num);
        for (int i=0; i<num; i++) {
            if (servers[i].stun_url) copy[i].stun_url=media_lib_strdup(servers[i].stun_url);
            if (servers[i].user) copy[i].user=media_lib_strdup(servers[i].user);
            if (servers[i].psw) copy[i].psw=media_lib_strdup(servers[i].psw);
        }
    }
    return copy;
}
LOCAL int peer_update_ice_info(esp_peer_handle_t peer, esp_peer_role_t role,
    esp_peer_ice_server_cfg_t *servers, int server_num)
{
    peer_t *pc=peer;
    if (!pc || !servers || !server_num) return ESP_PEER_ERR_INVALID_ARG;
    if (pc->state > ESP_PEER_STATE_NEW_CONNECTION) return ESP_PEER_ERR_WRONG_STATE;
    pc->cfg.role=role;
    free_server_cfg(pc);
    pc->cfg.server_lists=copy_server_cfg(servers,server_num);
    if (!pc->cfg.server_lists) return ESP_PEER_ERR_NO_MEM;
    pc->cfg.server_num=server_num;
    if (pc->state == ESP_PEER_STATE_NEW_CONNECTION) pc->b_offer_created=0;
    else esp_log(2,"PEER_DEF","W (%lu) %s: Only support during SDP exchange\n",esp_log_timestamp(),"PEER_DEF");
    return 0;
}
LOCAL void peer_apply_twcc(peer_t *pc, peer_sdp_info_t *info)
{
    if (!pc->extra_cfg.twcc_cfg.enable) {
        pc->twcc_negotiated=false;
        peer_set_twcc_reserve(pc,0);
        return;
    }
    if (!info->twcc_ext_id) {
        esp_log(3,"PEER_DEF","I (%lu) %s: TWCC disabled: peer SDP has no transport-wide-cc extmap\n",esp_log_timestamp(),"PEER_DEF");
        pc->twcc_negotiated=false;
        pc->twcc_ext_id=0;
        peer_set_twcc_reserve(pc,0);
        return;
    }
    if (!pc->twcc) {
        pc->twcc=twcc_create(pc->extra_cfg.twcc_cfg.history_size);
        if (!pc->twcc) {
            esp_log(2,"PEER_DEF","W (%lu) %s: TWCC alloc failed\n",esp_log_timestamp(),"PEER_DEF");
            pc->twcc_negotiated=false;
            return;
        }
    }
    pc->twcc_ext_id=info->twcc_ext_id;
    twcc_set_ext_id(pc->twcc,pc->twcc_ext_id);
    pc->twcc_negotiated=true;
    peer_set_twcc_reserve(pc,8);
    esp_log(3,"PEER_DEF","I (%lu) %s: TWCC negotiated ext_id=%u\n",esp_log_timestamp(),"PEER_DEF",info->twcc_ext_id);
}
LOCAL int peer_close(esp_peer_handle_t handle)
{
    peer_t *pc=handle;
    if (!pc) return ESP_PEER_ERR_INVALID_ARG;
    media_lib_mutex_lock(pc->api_lock,UINT32_MAX);
    if (pc->agent) agent_disconnect(pc->agent);
    while (pc->looping) media_lib_thread_sleep(10);
    if (pc->agent) {agent_deinit(pc->agent);pc->agent=NULL;}
    if (pc->sctp) {sctp_destroy(pc->sctp);pc->sctp=NULL;}
    if (pc->dtls_srtp) {dtls_srtp_deinit(pc->dtls_srtp);pc->dtls_srtp=NULL;}
    if (pc->rtp_cache) {rtp_rolling_buffer_destroy(pc->rtp_cache);pc->rtp_cache=NULL;}
    rtp_decoder_deinit(&pc->artp_decoder);
    rtp_decoder_deinit(&pc->vrtp_decoder);
    rtp_encoder_deinit(&pc->artp_encoder);
    rtp_encoder_deinit(&pc->vrtp_encoder);
    if (pc->rtp_send_transformer.encoded_data) {
        media_lib_free(pc->rtp_send_transformer.encoded_data);
        pc->rtp_send_transformer.encoded_data=NULL;
        pc->rtp_send_transformer.encoded_capacity=0;
    }
    if (pc->rtp_recv_transformer.encoded_data) {
        media_lib_free(pc->rtp_recv_transformer.encoded_data);
        pc->rtp_recv_transformer.encoded_data=NULL;
        pc->rtp_recv_transformer.encoded_capacity=0;
    }
    if (pc->rtp_aud_jitter) {rtp_jitter_destroy(pc->rtp_aud_jitter);pc->rtp_aud_jitter=NULL;}
    if (pc->rtp_vid_jitter) {rtp_jitter_destroy(pc->rtp_vid_jitter);pc->rtp_vid_jitter=NULL;}
    rtp_seq_tracker_deinit(&pc->rtp_seq);
    if (pc->twcc) {twcc_destroy(pc->twcc);pc->twcc=NULL;}
    clear_peer_buf(pc);
    free_server_cfg(pc);
    media_lib_mutex_unlock(pc->api_lock);
    if (pc->api_lock) {media_lib_mutex_destroy(pc->api_lock);pc->api_lock=NULL;}
    media_lib_free(pc);
    return 0;
}
LOCAL void peer_extract_sdp_info(peer_t *pc, char *start, bool update, peer_sdp_info_t *info)
{
    if (strncmp(start, "v=", 2)) return;
    char *line = start;
    char type = 'A';
    uint8_t *mid = NULL;
    esp_peer_media_dir_t *current_dir = NULL;
    info->stream_count = 0;
    info->aud_mid = 255;
    info->vid_mid = 255;
    info->data_mid = 255;
    info->data_channel_accepted = false;
    info->aud_rtp_port = 9;
    info->vid_rtp_port = 9;
    info->audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV;
    info->video_dir = ESP_PEER_MEDIA_DIR_SEND_RECV;
    info->peer_role = DTLS_SRTP_ROLE_SERVER;
    info->peer_vid_fmt = 0;
    peer_reset_codec_fmt(info->codec_fmt_id);
    info->aud_first_fmt = 255;
    info->vid_first_fmt = 255;
    info->peer_vid_profile = 0;
    info->peer_vid_rtx_apt = 0;
    info->twcc_ext_id = 0;
    bool in_video = false;
    bool in_audio = false;
    bool vid_profile_sel = false;
    bool vid_rtx_found = false;
    int vid_format = 0;
    int vid_rtx_format = 0;
    bool is_bundle = false;
    uint8_t aud_want = peer_aud_caps(pc);
    uint8_t vid_want = peer_vid_caps(pc);
    char *buf;
    while ((buf = strstr(line, "\r\n")) != NULL) {
        *buf = 0;
        if (!strncmp(line, "a=group:BUNDLE", 14)) {
            is_bundle = true;
        } else if (!strncmp(line, "m=video", 7)) {
            mid = &info->vid_mid;
            current_dir = &info->video_dir;
            unsigned int first_pt;
            if (sscanf(line, "m=video %*d %*s %u", &first_pt) == 1 && first_pt <= 127) {
                info->vid_first_fmt = first_pt;
            }
            if (!is_bundle) info->vid_rtp_port = atoi(line + 8);
            in_video = true;
            in_audio = false;
            type = 'V';
        } else if (!strncmp(line, "m=audio", 7)) {
            mid = &info->aud_mid;
            current_dir = &info->audio_dir;
            unsigned int first_pt;
            if (sscanf(line, "m=audio %*d %*s %u", &first_pt) == 1 && first_pt <= 127) {
                info->aud_first_fmt = first_pt;
            }
            if ((aud_want & 2) && peer_mline_has_pt(line, 0)) info->codec_fmt_id[PEER_FMT_AUD_G711U] = 0;
            if ((aud_want & 1) && peer_mline_has_pt(line, 8)) info->codec_fmt_id[PEER_FMT_AUD_G711A] = 8;
            if (!is_bundle) info->aud_rtp_port = atoi(line + 8);
            in_video = false;
            in_audio = true;
            type = 'A';
        } else if (!strncmp(line, "m=application", 13)) {
            mid = &info->data_mid;
            int app_port = atoi(line + 14);
            if (app_port) info->data_channel_accepted = true;
            in_video = false;
            in_audio = false;
            current_dir = NULL;
            type = 'D';
        } else if (!strncmp(line, "a=rtpmap:", 9)) {
            int map_pt = peer_parse_sdp_pt(line, "a=rtpmap:");
            if (map_pt >= 0) {
                if (in_video) {
                    if (peer_rtpmap_is(line, "H264/90000")) {
                        if (vid_want & 8) {
                            if (!peer_fmt_ok(info->codec_fmt_id[PEER_FMT_VID_H264])) info->codec_fmt_id[PEER_FMT_VID_H264] = map_pt;
                            if (!info->peer_vid_fmt) info->peer_vid_fmt = map_pt;
                        }
                        vid_format = map_pt;
                    } else if (peer_rtpmap_is(line, "JPEG/90000")) {
                        if (vid_want & 16) {
                            info->codec_fmt_id[PEER_FMT_VID_MJPEG] = map_pt;
                            if (!info->peer_vid_fmt) info->peer_vid_fmt = map_pt;
                        }
                        vid_format = map_pt;
                    } else if (peer_rtpmap_is(line, "rtx/90000")) {
                        vid_rtx_format = map_pt;
                    }
                } else if (in_audio) {
                    if (peer_rtpmap_is(line, "PCMA/8000") && (aud_want & 1)) {
                        info->codec_fmt_id[PEER_FMT_AUD_G711A] = map_pt;
                    } else if (peer_rtpmap_is(line, "PCMU/8000") && (aud_want & 2)) {
                        info->codec_fmt_id[PEER_FMT_AUD_G711U] = map_pt;
                    } else if (peer_rtpmap_is(line, "opus/48000/2") && (aud_want & 4)) {
                        info->codec_fmt_id[PEER_FMT_AUD_OPUS] = map_pt;
                    }
                }
            }
        } else if (!strncmp(line, "a=fmtp:", 7)) {
          if (in_video) {
            int cur_format = peer_parse_sdp_pt(line, "a=fmtp:");
            if (cur_format == vid_format && !vid_profile_sel) {
                char *vid_profile = strstr(line, "profile-level-id=");
                if (vid_profile) {
                    int profile = strtoul(vid_profile + 17, NULL, 16);
                    if (!info->peer_vid_profile) info->peer_vid_profile = profile;
                    if ((profile & 0xff0000) == 0x420000 && (peer_vid_caps(pc) & 8)) {
                        info->peer_vid_profile = profile;
                        info->peer_vid_fmt = cur_format;
                        info->codec_fmt_id[PEER_FMT_VID_H264] = cur_format;
                        esp_log(3, "PEER_DEF", "I (%lu) %s: Sel H264 profile %x format %d\n", esp_log_timestamp(), "PEER_DEF", profile, cur_format);
                        vid_profile_sel = true;
                    }
                }
            } else if (cur_format == vid_rtx_format) {
                char *apt = strstr(line, "apt=");
                if (apt) {
                    int rtx_apt = atoi(apt + 4);
                    if (rtx_apt == info->peer_vid_fmt || rtx_apt == vid_format) {
                        info->peer_vid_rtx_fmt = cur_format;
                        info->peer_vid_rtx_apt = rtx_apt;
                        vid_rtx_found = true;
                    }
                }
            }
          }
        } else if (in_video && !strncmp(line, "a=ssrc-group:FID ", 17)) {
            uint32_t primary_ssrc = 0;
            uint32_t rtx_ssrc = 0;
            if (sscanf(line + 17, "%lu %lu", &primary_ssrc, &rtx_ssrc) == 2) {
                info->vssrc = primary_ssrc;
                info->vrtx_ssrc = rtx_ssrc;
            }
        } else if (!strncmp(line, "a=ssrc:", 7)) {
            uint32_t parsed_ssrc = strtoul(line + 7, NULL, 10);
            if (in_video) {
                if (!info->vssrc) info->vssrc = parsed_ssrc;
                else if (info->vssrc != parsed_ssrc && !info->vrtx_ssrc) info->vrtx_ssrc = parsed_ssrc;
            } else if (in_audio) info->aud_ssrc = parsed_ssrc;
        } else if (!strncmp(line, "a=mid:", 6)) {
            info->stream_count++;
            uint8_t parsed_mid = strtoul(line + 6, NULL, 10);
            if (mid && (pc->cfg.role == ESP_PEER_ROLE_CONTROLLED || update)) *mid = parsed_mid;
        } else if (!strncmp(line, "a=setup:", 8)) {
            if (strstr(line, "passive") || strstr(line, "actpass")) info->peer_role = DTLS_SRTP_ROLE_SERVER;
            else info->peer_role = DTLS_SRTP_ROLE_CLIENT;
            esp_log(3, "PEER_DEF", "I (%lu) %s: Get peer role %d\n", esp_log_timestamp(), "PEER_DEF", info->peer_role);
        } else if (!strncmp(line, "a=fingerprint:", 14)) {
            info->fingerprint = line + 14;
        } else if (!strncmp(line, "a=sendrecv", 10)) {
            if (current_dir) *current_dir = ESP_PEER_MEDIA_DIR_SEND_RECV;
        } else if (!strncmp(line, "a=sendonly", 10)) {
            if (current_dir) *current_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY;
        } else if (!strncmp(line, "a=recvonly", 10)) {
            if (current_dir) *current_dir = ESP_PEER_MEDIA_DIR_RECV_ONLY;
        } else if (!strncmp(line, "a=inactive", 10)) {
            if (current_dir) {
                *current_dir = ESP_PEER_MEDIA_DIR_NONE;
                esp_log(3, "PEER_DEF", "I (%lu) %s: %c direction: inactive\n", esp_log_timestamp(), "PEER_DEF", type);
            }
        } else if (!strncmp(line, "a=ice-ufrag:", 12)) {
            info->ufrag = line + 12;
        } else if (!strncmp(line, "a=ice-pwd:", 10)) {
            info->pwd = line + 10;
        } else if (!strncmp(line, "a=extmap:", 9)) {
            if (strstr(line, "draft-holmer-rmcat-transport-wide-cc-extensions-01") || strstr(line, "transport-wide-cc")) {
                int id = atoi(line + 9);
                if (id > 0 && id < 15) info->twcc_ext_id = id;
            }
        }
        *buf = '\r';
        line = buf + 2;
    }
    if (!vid_rtx_found) info->peer_vid_rtx_fmt = 0;
    esp_log(3, "PEER_DEF", "I (%lu) %s: Extract SDP: stream_count=%d, audio_mid=%d (dir=%d), video_mid=%d (dir=%d), data_mid=%d, assrc=%lu, vssrc=%lu, vrtx_ssrc=%lu, vfmt=%d, vrtx_fmt=%d\n", esp_log_timestamp(), "PEER_DEF", info->stream_count, info->aud_mid, info->audio_dir, info->vid_mid, info->video_dir, info->data_mid, info->aud_ssrc, info->vssrc, info->vrtx_ssrc, info->peer_vid_fmt, info->peer_vid_rtx_fmt);
}

LOCAL void write_rtp_packet(uint8_t *data,size_t size,void *user_data,bool is_audio)
{
    peer_t *pc=user_data;
    peer_rtp_transformer_t *transformer=&pc->rtp_send_transformer;
    if (transformer->cb.transform) {
        rtp_header_t *header=(rtp_header_t *)data;
        esp_peer_rtp_frame_t frame={0};
        frame.orig_data=data;
        frame.orig_size=size;
        frame.payload_type=header->type;
        bool inplace=true;
        int ret=0;
        if (transformer->cb.get_encoded_size) {
            ret=transformer->cb.get_encoded_size(&frame,&inplace,transformer->ctx);
            if (ret==ESP_PEER_ERR_NOT_SUPPORT) goto send_packet;
            if (ret) return;
        }
        if (!inplace) {
            ret=check_rtp_transformer_capacity(pc,transformer,frame.encoded_size);
            if (ret) return;
        }
        frame.encoded_data=inplace?data:transformer->encoded_data;
        ret=transformer->cb.transform(&frame,transformer->ctx);
        if (ret) return;
        data=frame.encoded_data;
        size=frame.encoded_size;
    }
send_packet:
    if (pc->twcc && pc->twcc_negotiated) {
        size_t pkt_size=size;
        size_t cap=1428;
        if (data==transformer->encoded_data && transformer->encoded_capacity) cap=transformer->encoded_capacity;
        if (twcc_on_send(pc->twcc,data,&pkt_size,cap)==0) size=pkt_size;
    }
    if (pc->rtp_drop_pct) {
        uint32_t n=pc->rtp_drop_count++;
        if ((n*pc->rtp_drop_pct)%100<pc->rtp_drop_pct) return;
    }
    measure_start("srtp_encrypt");
    dtls_srtp_encrypt_rtp_packet(pc->dtls_srtp,data,1428,(int *)&size);
    measure_stop("srtp_encrypt");
    measure_start("agent_send");
    int ret;
    int port=is_audio?pc->aud_rtp_port:pc->vid_rtp_port;
    bool tcp_connected=agent_is_tcp_connected(pc->agent);
    if (tcp_connected) ret=agent_send_high_priority(pc->agent,data,size);
    else if (port>0 && port!=9) ret=agent_send_to_port(pc->agent,port,data,size);
    else ret=agent_send(pc->agent,data,size);
    pc->send_num++;
    if (ret>0) pc->send_size+=ret;
    measure_stop("agent_send");
    if (pc->rtp_cache && !tcp_connected) {
        uint16_t ssrc=rtp_get_ssrc(data);
        uint16_t seq_num=rtp_packet_get_seq_number(data);
        uint32_t uuid=(ssrc<<16)|seq_num;
        rtp_rolling_buffer_insert(pc->rtp_cache,uuid,data,size,NULL);
    }
}

LOCAL int peer_insert_rtp_payload(peer_t *pc,uint32_t ssrc,uint8_t *payload,uint32_t size)
{
    peer_rtp_transformer_t *transformer=&pc->rtp_recv_transformer;
    if (transformer->cb.transform) {
        rtp_header_t *header=(rtp_header_t *)payload;
        esp_peer_rtp_frame_t frame={0};
        frame.orig_data=payload;
        frame.orig_size=size;
        frame.payload_type=header->type;
        bool inplace=true;
        int ret=0;
        if (transformer->cb.get_encoded_size) {
            ret=transformer->cb.get_encoded_size(&frame,&inplace,transformer->ctx);
            if (ret==ESP_PEER_ERR_NOT_SUPPORT) goto insert_packet;
            if (ret) return ret;
        }
        if (!inplace) {
            ret=check_rtp_transformer_capacity(pc,transformer,frame.encoded_size);
            if (ret) {
                esp_log(1,"PEER_DEF","E (%lu) %s: Fail to check RTP transformer capacity\n",esp_log_timestamp(),"PEER_DEF");
                return ret;
            }
        }
        frame.encoded_data=inplace?payload:transformer->encoded_data;
        ret=transformer->cb.transform(&frame,transformer->ctx);
        if (ret) {
            esp_log(1,"PEER_DEF","E (%lu) %s: Fail to transform RTP frame\n",esp_log_timestamp(),"PEER_DEF");
            return ret;
        }
        payload=frame.encoded_data;
        size=frame.encoded_size;
    }
insert_packet:
    if (agent_is_tcp_connected(pc->agent)) return rtp_data_reached(payload,size,pc);
    if (ssrc==pc->remote_assrc) {
        if (!peer_notify_from_rtp(pc,payload,size,true)) return 0;
        if (pc->rtp_aud_jitter) rtp_jitter_add(pc->rtp_aud_jitter,payload,size);
        else rtp_decoder_decode(&pc->artp_decoder,payload,size);
    } else if (ssrc==pc->remote_vssrc) {
        if (!peer_notify_from_rtp(pc,payload,size,false)) return 0;
        if (pc->rtp_vid_jitter) rtp_jitter_add(pc->rtp_vid_jitter,payload,size);
        else rtp_decoder_decode(&pc->vrtp_decoder,payload,size);
    }
    return 0;
}
LOCAL void write_rtp_aud_packet(uint8_t *data, size_t size, void *user_data)
{write_rtp_packet(data,size,user_data,true);}
LOCAL void write_rtp_vid_packet(uint8_t *data, size_t size, void *user_data)
{write_rtp_packet(data,size,user_data,false);}
LOCAL int dtls_recv(void *ctx, uint8_t *buf, size_t len)
{
    int recv_count=0;
    int ret=0;
    peer_t *pc=((dtls_srtp_t *)ctx)->ctx;
    int consume=0;
    if (pc->agent_ret) {
        consume = len < pc->agent_ret ? len : pc->agent_ret;
        memcpy(buf,pc->agent_buf,consume);
        pc->agent_ret-=consume;
        if (pc->agent_ret) memmove(pc->agent_buf,pc->agent_buf+consume,pc->agent_ret);
        if (pc->state==ESP_PEER_STATE_CONNECTED) return consume;
        buf+=consume;
        len-=consume;
    }
    if (pc->state==ESP_PEER_STATE_CONNECTED) return -0x6900;
    while (len) {
        ret=agent_recv(pc->agent,buf,len,false);
        if (ret>0) {consume+=ret;break;}
        else if (ret==0) {
            recv_count++;
            if (recv_count>10) {
                esp_log(1,"PEER_DEF","E (%lu) %s: agent_recv timeout\n",esp_log_timestamp(),"PEER_DEF");
                return -0x7880;
            }
        } else if (ret<0) {
            esp_log(1,"PEER_DEF","E (%lu) %s: agent_recv error: %d\n",esp_log_timestamp(),"PEER_DEF",ret);
            return -0x7880;
        }
    }
    return consume>0 ? consume : ret<0 ? ret : -1;
}
LOCAL int ressemble_rtx_rtp_packet(peer_t *, uint8_t *, uint32_t *);
LOCAL int peer_handle_decrypted_rtp(peer_t *pc, uint8_t *payload, int size)
{
    uint32_t ssrc=rtp_get_ssrc(payload);
    if (pc->remote_vrtx_ssrc && ssrc==pc->remote_vrtx_ssrc) {
        uint32_t new_size=size;
        int ret=ressemble_rtx_rtp_packet(pc,payload,&new_size);
        if (ret) return ret;
        peer_insert_rtp_payload(pc,pc->remote_vssrc,payload,new_size);
    } else if (ssrc==pc->remote_assrc || ssrc==pc->remote_vssrc) {
        if (pc->twcc && pc->twcc_negotiated) twcc_on_recv(pc->twcc,payload,size);
        peer_insert_rtp_payload(pc,ssrc,payload,size);
        if (!pc->rtp_rx_pending) {
            pc->nack_due_ms=esp_timer_get_time()/1000+40;
            pc->rtp_rx_pending=true;
        }
    }
    return 0;
}
static esp_peer_ice_server_cfg_t *copy_server_cfg(esp_peer_ice_server_cfg_t *, uint8_t);
static int dtls_send(void *, const unsigned char *, size_t);
static int dtls_recv(void *, unsigned char *, size_t);
static void write_rtp_aud_packet(uint8_t *, size_t, void *);
static void write_rtp_vid_packet(uint8_t *, size_t, void *);
static int peer_set_audio_sdp(peer_t *, sdp_t *);
static int peer_set_video_sdp(peer_t *, sdp_t *);
#define PD_LOGW(format, ...) esp_log(2,"PEER_DEF","W (%lu) %s: " format "\n",esp_log_timestamp(),"PEER_DEF",##__VA_ARGS__)
#define PD_LOGE(format, ...) esp_log(1,"PEER_DEF","E (%lu) %s: " format "\n",esp_log_timestamp(),"PEER_DEF",##__VA_ARGS__)

LOCAL int peer_open(esp_peer_cfg_t *cfg, esp_peer_handle_t *handle)
{
    if (!cfg || !handle || !verify_config(cfg)) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    peer_t *pc = media_lib_calloc(1, sizeof(peer_t));
    if (!pc) {
        return ESP_PEER_ERR_NO_MEM;
    }
    int ret = ESP_PEER_ERR_NO_MEM;
    memcpy(&pc->cfg, cfg, sizeof(esp_peer_cfg_t));
    pc->cfg.server_lists = copy_server_cfg(cfg->server_lists, cfg->server_num);
    if (cfg->server_num && !pc->cfg.server_lists) {
        goto _exit;
    }
    pc->cfg.server_num = cfg->server_num;
    agent_mode_t mode = cfg->role == ESP_PEER_ROLE_CONTROLLED ? AGENT_MODE_CONTROLLED : AGENT_MODE_CONTROLLING;
    if (cfg->extra_cfg) {
        memcpy(&pc->extra_cfg, cfg->extra_cfg, sizeof(esp_peer_default_cfg_t));
    }
    if (!pc->agent) {
        agent_cfg_t agent_cfg = {
            .mode = mode,
            .ipv6_support = pc->extra_cfg.ipv6_support,
            .tcp_support = pc->extra_cfg.tcp_support,
            .max_candidates = pc->extra_cfg.max_candidates,
            .max_tcp_connections = pc->extra_cfg.max_tcp_connections,
            .alive_binding_retries = pc->extra_cfg.alive_binding_retries,
            .ice_use_lite_mode = pc->extra_cfg.ice_use_lite_mode,
            .turn_skip_cert_verify = pc->extra_cfg.insecure_skip_turn_cert_verify,
        };
        pc->agent = agent_init(&agent_cfg);
    }
    if (!pc->agent) {
        goto _exit;
    }
    if (pc->extra_cfg.agent_recv_timeout) {
        agent_set_recv_timeout(pc->agent, pc->extra_cfg.agent_recv_timeout);
    }
    agent_set_ice_policy(pc->agent, cfg->ice_trans_policy == ESP_PEER_ICE_TRANS_POLICY_RELAY);
    media_lib_mutex_create(&pc->api_lock);
    if (!pc->api_lock) {
        goto _exit;
    }
    dtls_srtp_cfg_t dtls_cfg = {
        .role = DTLS_SRTP_ROLE_SERVER,
        .udp_send = dtls_send,
        .udp_recv = dtls_recv,
        .ctx = pc,
    };
    pc->dtls_srtp = dtls_srtp_init(&dtls_cfg);
    if (!pc->dtls_srtp) {
        goto _exit;
    }
    if (cfg->audio_info.codec != ESP_PEER_AUDIO_CODEC_NONE) {
        rtp_encoder_init(&pc->artp_encoder, get_audio_codec(cfg->audio_info.codec), write_rtp_aud_packet, pc);
    }
    if (cfg->audio_dir == ESP_PEER_MEDIA_DIR_RECV_ONLY || cfg->audio_dir == ESP_PEER_MEDIA_DIR_SEND_RECV) {
        rtp_decoder_init(&pc->artp_decoder, get_audio_codec(cfg->audio_info.codec), on_audio, pc);
        rtp_jitter_cfg_t jitter_cfg = {
            .size = pc->extra_cfg.rtp_cfg.audio_recv_jitter.cache_size ? pc->extra_cfg.rtp_cfg.audio_recv_jitter.cache_size : 100 * 1024,
            .timeout = pc->extra_cfg.rtp_cfg.audio_recv_jitter.cache_timeout,
            .resend_delay = pc->extra_cfg.rtp_cfg.audio_recv_jitter.resend_delay,
            .src_ssrc = get_aud_ssrc(cfg),
            .rtp_callback = rtp_data_reached,
            .ctx = pc,
        };
        rtp_jitter_create(&jitter_cfg, &pc->rtp_aud_jitter);
        if (!pc->rtp_aud_jitter) {
            PD_LOGW("Create cache for audio receive jitter buffer");
        }
    }
    if (cfg->video_info.codec != ESP_PEER_VIDEO_CODEC_NONE) {
        rtp_encoder_init(&pc->vrtp_encoder, get_video_codec(cfg->video_info.codec), write_rtp_vid_packet, pc);
        if (cfg->video_info.codec == ESP_PEER_VIDEO_CODEC_MJPEG) {
            rtp_encoder_set_jpeg_info(&pc->vrtp_encoder, cfg->video_info.width, cfg->video_info.height);
        }
    }
    if (cfg->video_dir == ESP_PEER_MEDIA_DIR_RECV_ONLY || cfg->video_dir == ESP_PEER_MEDIA_DIR_SEND_RECV) {
        pc->video_pli_send_interval = pc->extra_cfg.rtp_cfg.video_recv_jitter.pli_send_interval;
        rtp_decoder_init(&pc->vrtp_decoder, get_video_codec(cfg->video_info.codec), on_video, pc);
        rtp_jitter_cfg_t jitter_cfg = {
            .size = pc->extra_cfg.rtp_cfg.video_recv_jitter.cache_size ? pc->extra_cfg.rtp_cfg.video_recv_jitter.cache_size : 400 * 1024,
            .timeout = pc->extra_cfg.rtp_cfg.video_recv_jitter.cache_timeout,
            .resend_delay = pc->extra_cfg.rtp_cfg.video_recv_jitter.resend_delay,
            .src_ssrc = get_vid_ssrc(cfg),
            .rtp_callback = rtp_data_reached,
            .ctx = pc,
        };
        rtp_jitter_create(&jitter_cfg, &pc->rtp_vid_jitter);
        if (!pc->rtp_vid_jitter) {
            PD_LOGW("Create cache for audio receive jitter buffer");
        }
    }
    rtp_rolling_buf_cfg_t cache_cfg = {0};
    cache_cfg.pool_size = pc->extra_cfg.rtp_cfg.send_pool_size ? pc->extra_cfg.rtp_cfg.send_pool_size : 400 * 1024;
    cache_cfg.packet_count = pc->extra_cfg.rtp_cfg.send_queue_num;
    cache_cfg.max_resend_limit = pc->extra_cfg.rtp_cfg.max_resend_count;
    pc->rtp_cache = rtp_rolling_buffer_create(&cache_cfg);
    if (!pc->rtp_cache) {
        PD_LOGW("Create cache for RTP packets fail, resent logic is disabled");
    }
    clear_peer_buf(pc);
    *handle = pc;
    return ESP_PEER_ERR_NONE;
_exit:
    if (pc->api_lock) {
        media_lib_mutex_destroy(pc->api_lock);
        pc->api_lock = NULL;
    }
    if (pc->dtls_srtp) {
        dtls_srtp_deinit(pc->dtls_srtp);
        pc->dtls_srtp = NULL;
    }
    if (pc->agent) {
        agent_deinit(pc->agent);
        pc->agent = NULL;
    }
    free_server_cfg(pc);
    media_lib_free(pc);
    return ret;
}

LOCAL int peer_prepare_connection(peer_t *pc, bool update)
{
    char *description = media_lib_calloc(1, 1300);
    sdp_t *local_sdp = media_lib_calloc(1, sizeof(sdp_t));
    int ret = ESP_PEER_ERR_NO_MEM;
    if (!description || !local_sdp) {
        goto _exit;
    }
    dtls_srtp_role_t role = peer_get_local_dtls_role(pc);
    if (!update) {
        agent_mode_t mode = pc->cfg.role == ESP_PEER_ROLE_CONTROLLED ? AGENT_MODE_CONTROLLED : AGENT_MODE_CONTROLLING;
        if (pc->agent) {
            if (pc->extra_cfg.ice_use_lite_mode) {
                mode = AGENT_MODE_CONTROLLING;
            }
            agent_change_mode(pc->agent, mode);
        }
        if (pc->cfg.server_num == 0) {
            agent_gather_candidate(pc->agent, NULL, NULL, NULL);
        } else {
            for (int i = 0; i < pc->cfg.server_num; i++) {
                esp_peer_ice_server_cfg_t *stun_cfg = &pc->cfg.server_lists[i];
                if (!stun_cfg->user) {
                    agent_gather_candidate(pc->agent, stun_cfg->stun_url, stun_cfg->user, stun_cfg->psw);
                }
            }
            for (int i = 0; i < pc->cfg.server_num; i++) {
                esp_peer_ice_server_cfg_t *stun_cfg = &pc->cfg.server_lists[i];
                if (stun_cfg->user) {
                    agent_gather_candidate(pc->agent, stun_cfg->stun_url, stun_cfg->user, stun_cfg->psw);
                }
            }
        }
    }
    ret = agent_get_local_description(pc->agent, description, 1300);
    if (ret) {
        PD_LOGE("Fail to get local description");
        goto _exit;
    }
    if (pc->cfg.role == ESP_PEER_ROLE_CONTROLLING && !update) {
        int mid = 0;
        pc->aud_mid = pc->vid_mid = pc->data_mid = 255;
        if (peer_aud_caps(pc)) {
            pc->aud_mid = mid++;
        }
        if (peer_vid_caps(pc)) {
            pc->vid_mid = mid++;
        }
        if (pc->cfg.enable_data_channel) {
            pc->data_mid = mid++;
        }
        pc->stream_count = mid;
    } else if (pc->cfg.role == ESP_PEER_ROLE_CONTROLLED && pc->stream_count == 0) {
        int mid = 0;
        pc->aud_mid = pc->vid_mid = pc->data_mid = 255;
        if (peer_aud_caps(pc)) {
            pc->aud_mid = mid++;
        }
        if (peer_vid_caps(pc)) {
            pc->vid_mid = mid++;
        }
        pc->stream_count = mid;
    }
    sdp_create(local_sdp, pc->stream_count);
    for (int i = 0; i < pc->stream_count; i++) {
        int ret = 0;
        if (i == pc->aud_mid) {
            ret = peer_set_audio_sdp(pc, local_sdp);
        } else if (i == pc->vid_mid) {
            ret = peer_set_video_sdp(pc, local_sdp);
        } else if (i == pc->data_mid) {
            if (!pc->cfg.enable_data_channel) {
                ret = peer_set_data_sdp_rejected(pc, local_sdp);
            } else {
                ret = peer_set_data_sdp(pc, local_sdp);
            }
        }
        if (ret == 0) {
            sdp_append(local_sdp, "a=fingerprint:sha-256 %s", dtls_srtp_get_local_fingerprint(pc->dtls_srtp));
            peer_append_dtls_setup(pc, local_sdp, role, update);
            sdp_append(local_sdp, description);
        }
    }
    peer_notify_if_single(pc);
    pc->b_offer_created = 1;
    if (pc->cfg.on_msg) {
        esp_peer_msg_t msg = {
            .type = ESP_PEER_MSG_TYPE_SDP,
            .data = (uint8_t *)local_sdp->content,
            .size = strlen(local_sdp->content),
        };
        ret = pc->cfg.on_msg(&msg, pc->cfg.ctx);
        if (ret) {
            peer_do_disconnect(pc, true);
        }
    }
_exit:
    if (local_sdp) {
        media_lib_free(local_sdp);
    }
    if (description) {
        media_lib_free(description);
    }
    return ret;
}

LOCAL void rtp_nack_resend(peer_t *pc,uint16_t ssrc,uint16_t seq_num)
{
    if (agent_is_tcp_connected(pc->agent)) return;
    uint32_t uuid=(ssrc<<16)|seq_num;
    rtp_rolling_buf_pkt_t pkt;
    int ret=rtp_rolling_buffer_acquire(pc->rtp_cache,uuid,&pkt);
    if (ret) return;
    measure_start("agent_send");
    agent_send(pc->agent,pkt.data,pkt.size);
    measure_stop("agent_send");
    rtp_rolling_buffer_release(pc->rtp_cache,&pkt);
}

LOCAL int rtp_nack_parse_and_resend(peer_t *pc,uint16_t ssrc,uint8_t *payload,uint32_t payload_length)
{
    if (!pc->rtp_cache || agent_is_tcp_connected(pc->agent)) return -1;
    for (int i=0;i<payload_length;i+=4) {
        uint16_t base_seq_num=__builtin_bswap16(*(uint16_t *)(payload+i));
        uint16_t BLP=__builtin_bswap16(*(uint16_t *)(payload+i+2));
        rtp_nack_resend(pc,ssrc,base_seq_num);
        for (int j=0;j<16;j++) {
            if ((BLP>>j)&1) {
                rtp_nack_resend(pc,ssrc,(uint16_t)(base_seq_num+j)+1);
                esp_log(3,"PEER_DEF","I (%lu) %s: r %d\n",esp_log_timestamp(),"PEER_DEF",base_seq_num+j+1);
            }
        }
    }
    return 0;
}

LOCAL int ressemble_rtx_rtp_packet(peer_t *pc,uint8_t *payload,uint32_t *size)
{
    if (!pc || *size<14) return -1;
    rtp_header_t *header=(rtp_header_t *)payload;
    uint32_t header_size=sizeof(rtp_header_t)+header->csrccount*4;
    if (header->extension) {
        if (*size<header_size+4) return -1;
        uint16_t ext_len=__builtin_bswap16(*(uint16_t *)(payload+header_size+2));
        header_size+=(ext_len+1)*4;
    }
    if (*size<header_size+2) return -1;
    uint16_t origin_seq=*(uint16_t *)(payload+header_size);
    memmove(payload+header_size,payload+header_size+2,*size-header_size-2);
    *size-=2;
    header->ssrc=__builtin_bswap32(pc->remote_vssrc);
    header->seq_number=origin_seq;
    if (pc->peer_vid_rtx_apt) header->type=pc->peer_vid_rtx_apt;
    return 0;
}


LOCAL void process_rtcp(peer_t *pc, uint8_t *buf, size_t len)
{
    rtcp_header_t *rtcp_header;
    size_t pos = 0;
    int ret;
    while (pos < len) {
        rtcp_header = (rtcp_header_t *)(buf + pos);
        switch (rtcp_header->type) {
        case 203:
            esp_log(3, "PEER_DEF", "I (%lu) %s: Got RTCP_BYE\n", esp_log_timestamp(), "PEER_DEF");
            break;
        case 201:
            if (rtcp_header->rc > 0) {
                rtcp_rr_t rtcp_rr;
                ret = rtcp_parse_rr(buf + pos, len - pos, &rtcp_rr);
            }
            break;
        case 205: {
            rtcp_fb_t rtcp_fb;
            ret = rtcp_parse_fb(buf + pos, len - pos, &rtcp_fb);
            if (ret) break;
            if (rtcp_fb.header.rc == 15) {
                if (pc->twcc && pc->cfg.on_twcc) {
                    esp_peer_twcc_report_t report;
                    int pkt_len = (__builtin_bswap16(rtcp_header->length) + 1) * 4;
                    if (pkt_len > 0 && pos + pkt_len <= len &&
                        twcc_parse_feedback(pc->twcc, buf + pos, pkt_len, &report) == 0) {
                        pc->cfg.on_twcc(&report, pc->cfg.ctx);
                    }
                }
                break;
            }
            if (rtcp_fb.header.rc != 1 || rtcp_fb.header.length <= 2) {
                esp_log(2, "PEER_DEF", "W (%lu) %s: Unhandled RTCP_RTPFB Type %d\n", esp_log_timestamp(), "PEER_DEF", rtcp_fb.header.rc);
                break;
            }
            uint8_t *payload = buf + pos + 12;
            uint32_t payload_length = (rtcp_fb.header.length + 1) * sizeof(uint32_t) - sizeof(rtcp_fb_t);
            if (payload + payload_length > buf + len) break;
            rtp_nack_parse_and_resend(pc, rtcp_fb.media, payload, payload_length);
            break;
        }
        case 206: {
            rtcp_fb_t rtcp_fb;
            ret = rtcp_parse_fb(buf + pos, len - pos, &rtcp_fb);
            if (ret) break;
            if (rtcp_fb.header.rc == 1) {
                esp_log(3, "PEER_DEF", "I (%lu) %s: Got RTCP PLI media_ssrc=%u\n", esp_log_timestamp(), "PEER_DEF", rtcp_fb.media);
                if (pc->cfg.on_state) pc->cfg.on_state(ESP_PEER_STATE_VIDEO_PLI_RECEIVED, pc->cfg.ctx);
            } else if (rtcp_fb.header.rc == 15 && pc->cfg.on_remb) {
                uint32_t pkt_len = (__builtin_bswap16(rtcp_header->length) + 1) * 4;
                uint32_t bitrate = 0;
                uint32_t remb_ssrc = 0;
                if (pkt_len > 0 && pkt_len + pos <= len &&
                    rtcp_parse_remb(buf + pos, pkt_len, &bitrate, &remb_ssrc) == 0) {
                    esp_peer_remb_report_t report = {bitrate, remb_ssrc};
                    pc->cfg.on_remb(&report, pc->cfg.ctx);
                }
            }
            break;
        }
        }
        pos += (__builtin_bswap16(rtcp_header->length) + 1) * 4;
    }
}

LOCAL int peer_recv_streams(peer_t *pc)
{
    bool no_wait = false;
    while (1) {
        pc->agent_buf[0] = 0;
        measure_start("agent_recv");
        pc->agent_ret = agent_recv(pc->agent, pc->agent_buf, sizeof(pc->agent_buf), no_wait);
        if (pc->agent_ret < 0) {
            measure_stop("agent_recv");
            esp_log(1, "PEER_DEF", "E (%lu) %s: agent_recv failed %d\n", esp_log_timestamp(), "PEER_DEF", pc->agent_ret);
            return pc->agent_ret;
        }
        pc->receive_num++;
        if (pc->agent_ret > 0) pc->receive_size += pc->agent_ret;
        measure_stop("agent_recv");
        if (pc->agent_ret <= 0) return pc->agent_ret;
        if (peer_tracing) printf("recv %d %02x\n", pc->agent_ret, pc->agent_buf[0]);
        measure_start("Process");
        if (dtls_srtp_probe(pc->agent_buf)) {
            measure_start("srtp_read");
            int ret = dtls_srtp_read(pc->dtls_srtp, pc->dtls_buf, sizeof(pc->dtls_buf));
            measure_stop("srtp_read");
            if (ret > 0) {
                sctp_check_data(pc->sctp, pc->dtls_buf, ret);
                pc->data_recv_size += ret;
                measure_start("sctp_in");
                if (pc->sctp) sctp_incoming_data(pc->sctp, pc->dtls_buf, ret);
                measure_stop("sctp_in");
            } else if (ret < 0) {
                if (ret == -0x7880) return -1;
                return 0;
            }
        } else if (rtp_packet_validate(pc->agent_buf, pc->agent_ret)) {
            if (rtcp_probe(pc->agent_buf, pc->agent_ret)) {
                measure_start("srtp_decrypt");
                dtls_srtp_decrypt_rtcp_packet(pc->dtls_srtp, pc->agent_buf, &pc->agent_ret);
                measure_stop("srtp_decrypt");
                process_rtcp(pc, pc->agent_buf, pc->agent_ret);
            } else {
                uint8_t *held = NULL;
                uint32_t held_size = 0;
                int n = rtp_seq_filter(&pc->rtp_seq, pc->agent_buf, pc->agent_ret, &held, &held_size);
                if (n <= 0) {
                    measure_stop("Process");
                    no_wait = true;
                    continue;
                }
                if (n == 2 && held && held_size) {
                    int hlen = held_size;
                    measure_start("srtp_decrypt");
                    int hret = dtls_srtp_decrypt_rtp_packet(pc->dtls_srtp, held, &hlen);
                    measure_stop("srtp_decrypt");
                    if (hret == 0) peer_handle_decrypted_rtp(pc, held, hlen);
                }
                measure_start("srtp_decrypt");
                int ret = dtls_srtp_decrypt_rtp_packet(pc->dtls_srtp, pc->agent_buf, &pc->agent_ret);
                measure_stop("srtp_decrypt");
                if (ret) return 0;
                peer_handle_decrypted_rtp(pc, pc->agent_buf, pc->agent_ret);
            }
        }
        measure_stop("Process");
        no_wait = true;
    }
}
LOCAL int peer_send_rtp_nack(peer_t *pc)
{
    if (!pc->rtp_rx_pending) return 0;
    uint32_t now=esp_timer_get_time()/1000;
    if (now < pc->nack_due_ms) return 0;
    int ret=0;
    int size=0;
    bool tcp_connected=agent_is_tcp_connected(pc->agent);
    if (tcp_connected) {pc->rtp_rx_pending=false;return 0;}
    if (pc->rtp_aud_jitter) {
        size=rtp_jitter_get_nack(pc->rtp_aud_jitter,pc->dtls_buf,sizeof(pc->dtls_buf));
        if (size>0) {
            dtls_srtp_encrypt_rctp_packet(pc->dtls_srtp,pc->dtls_buf,sizeof(pc->dtls_buf),&size);
            ret=agent_send(pc->agent,pc->dtls_buf,size);
        }
    }
    if (pc->rtp_vid_jitter) {
        size=rtp_jitter_get_nack(pc->rtp_vid_jitter,pc->dtls_buf,sizeof(pc->dtls_buf));
        if (size>0) {
            dtls_srtp_encrypt_rctp_packet(pc->dtls_srtp,pc->dtls_buf,sizeof(pc->dtls_buf),&size);
            ret=agent_send(pc->agent,pc->dtls_buf,size);
        }
    }
    pc->rtp_rx_pending=false;
    return ret;
}
LOCAL int peer_send_pli(peer_t *pc)
{
    if (!pc->video_pli_send_interval || !pc->remote_vssrc) return 0;
    uint32_t cur=esp_timer_get_time()/1000;
    if (cur < pc->video_pli_send_time+pc->video_pli_send_interval) return 0;
    int size=rtcp_get_pli(pc->dtls_buf,12,pc->remote_vssrc);
    int ret=0;
    if (size>0) {
        dtls_srtp_encrypt_rctp_packet(pc->dtls_srtp,pc->dtls_buf,sizeof(pc->dtls_buf),&size);
        ret=agent_send(pc->agent,pc->dtls_buf,size);
    }
    pc->video_pli_send_time=cur;
    return ret;
}
LOCAL int peer_send_twcc_feedback(peer_t *pc)
{
    if (!pc->twcc || !pc->twcc_negotiated) return 0;
    uint32_t cur=esp_timer_get_time()/1000;
    uint16_t period=pc->extra_cfg.twcc_cfg.send_period;
    uint32_t sender_ssrc=get_vid_ssrc(&pc->cfg);
    if (!sender_ssrc) sender_ssrc=get_aud_ssrc(&pc->cfg);
    int size=twcc_build_feedback(pc->twcc,pc->dtls_buf,sizeof(pc->dtls_buf),sender_ssrc,0,cur,period);
    if (size<=0) return 0;
    dtls_srtp_encrypt_rctp_packet(pc->dtls_srtp,pc->dtls_buf,sizeof(pc->dtls_buf),&size);
    return agent_send(pc->agent,pc->dtls_buf,size);
}
LOCAL int peer_send_rtcp_feedback(peer_t *pc)
{
    int nack_ret=peer_send_rtp_nack(pc);
    int pli_ret=peer_send_pli(pc);
    int twcc_ret=peer_send_twcc_feedback(pc);
    if (nack_ret) return nack_ret;
    if (pli_ret) return pli_ret;
    return twcc_ret;
}
LOCAL int peer_send_msg(esp_peer_handle_t, esp_peer_msg_t *);
LOCAL int peer_main_loop(esp_peer_handle_t);
const esp_peer_ops_t *esp_peer_get_default_impl(void)
{
    static const esp_peer_ops_t ops={
        .open=peer_open,
        .new_connection=peer_new_connection,
        .update_ice_info=peer_update_ice_info,
        .send_msg=peer_send_msg,
        .send_video=peer_send_video,
        .send_audio=peer_send_audio,
        .send_data=peer_send_data,
        .create_data_channel=peer_create_data_channel,
        .close_data_channel=peer_close_data_channel,
        .main_loop=peer_main_loop,
        .disconnect=peer_disconnect,
        .query=peer_query,
        .close=peer_close,
        .set_rtp_transformer=peer_set_rtp_transformer,
    };
    return &ops;
}
LOCAL int peer_send_msg(esp_peer_handle_t handle, esp_peer_msg_t *info)
{
    if (!handle || !info || !info->size) return ESP_PEER_ERR_INVALID_ARG;
    peer_t *pc = handle;
    bool has_candidate = false;
    if (info->type == ESP_PEER_MSG_TYPE_SDP) {
        char *sdp = (char *)info->data;
        if (pc->state == ESP_PEER_STATE_CONNECTING && pc->b_remote_sdp_got) {
            esp_log(3, "PEER_DEF", "I (%lu) %s: Ignore duplicated SDP while connecting\n", esp_log_timestamp(), "PEER_DEF");
            return 0;
        }
        bool renegotiate = pc->state == ESP_PEER_STATE_CONNECTED;
        peer_sdp_info_t new_info = {0};
        peer_extract_sdp_info(pc, sdp, renegotiate, &new_info);
        if (renegotiate) {
            peer_sdp_info_t old_info = {0};
            peer_backup_sdp_info(pc, &old_info);
            if (peer_remote_session_unchanged(&old_info, &new_info)) {
                esp_log(3, "PEER_DEF", "I (%lu) %s: Handle SDP renegotiation without ICE restart\n", esp_log_timestamp(), "PEER_DEF");
                peer_notify_remote_track_change(pc, &old_info, &new_info);
                peer_reset_changed_codec_info(pc, &old_info, &new_info);
                peer_apply_sdp_info(pc, &new_info);
                peer_apply_twcc(pc, &new_info);
                peer_update_rtp_encoder(pc);
                agent_set_remote_description(pc->agent, sdp);
                return peer_prepare_connection(pc, true);
            }
            esp_log(3, "PEER_DEF", "I (%lu) %s: Handle SDP renegotiation with ICE restart\n", esp_log_timestamp(), "PEER_DEF");
            peer_do_disconnect(pc, false);
            clear_peer_buf(pc);
            peer_new_connection(pc);
        }
        peer_apply_sdp_info(pc, &new_info);
        peer_apply_twcc(pc, &new_info);
        has_candidate = strstr(sdp, "candidate:") != NULL;
        peer_update_rtp_encoder(pc);
    } else if (info->type == ESP_PEER_MSG_TYPE_CANDIDATE) {
        has_candidate = true;
    }
    int remote_num = agent_set_remote_description(pc->agent, (char *)info->data);
    pc->remote_candidate_count = remote_num;
    bool sdp_has_offer = false;
    if (info->type == ESP_PEER_MSG_TYPE_SDP && pc->stream_count) {
        sdp_has_offer = strstr((char *)info->data, "a=ice-ufrag:") != NULL;
    }
    if ((has_candidate && remote_num > 0) || sdp_has_offer) {
        pc->b_remote_sdp_got = 1;
        if (info->type == ESP_PEER_MSG_TYPE_SDP && pc->b_offer_created) peer_notify_if_single(pc);
        if (pc->b_offer_created && pc->remote_candidate_count > 0) {
            if (pc->state <= ESP_PEER_STATE_PAIRED || pc->state == ESP_PEER_STATE_CONNECT_FAILED) peer_set_state(pc, ESP_PEER_STATE_PAIRING);
        }
    }
    return 0;
}

LOCAL int peer_set_audio_sdp(peer_t *pc, sdp_t *sdp)
{
    esp_peer_media_dir_t audio_dir = pc->cfg.audio_dir;
    if (pc->b_remote_sdp_got) {
        const peer_codec_desc_t *send = peer_find_codec(PEER_MEDIA_AUDIO, pc->cfg.audio_info.codec);
        audio_dir = get_compatible_direction(pc->remote_audio_dir, audio_dir);
        audio_dir = peer_limit_send_direction(audio_dir, pc->codec_fmt_id, send ? send->fmt_idx : PEER_FMT_N);
        esp_log(3, "PEER_DEF", "I (%lu) %s: Audio direction: offer=%d, local=%d, answer=%d\n", esp_log_timestamp(), "PEER_DEF", pc->remote_audio_dir, pc->cfg.audio_dir, audio_dir);
    }
    sdp_audio_codec_t codecs[4];
    int n = peer_gather_audio(pc, codecs, 4);
    if (n <= 0) {
        if (pc->b_remote_sdp_got) {
            int pt = peer_fmt_ok(pc->aud_first_fmt) ? pc->aud_first_fmt : 0;
            sdp_append_media_rejected(sdp, "audio", pc->aud_mid, pt);
            return 0;
        }
        return -1;
    }
    sdp_append_audio_codecs(sdp, pc->aud_mid, audio_dir, codecs, n, pc->cfg.audio_info.codec);
    if (peer_twcc_advertise(pc)) {
        sdp_append_twcc(sdp, peer_twcc_ext_id(pc));
        sdp_append_transport_cc(sdp, codecs[0].pt);
    }
    return 0;
}

LOCAL int peer_set_video_sdp(peer_t *pc, sdp_t *sdp)
{
    esp_peer_media_dir_t video_dir = pc->cfg.video_dir;
    if (pc->b_remote_sdp_got) {
        const peer_codec_desc_t *send = peer_find_codec(PEER_MEDIA_VIDEO, pc->cfg.video_info.codec);
        video_dir = get_compatible_direction(pc->remote_video_dir, video_dir);
        video_dir = peer_limit_send_direction(video_dir, pc->codec_fmt_id, send ? send->fmt_idx : PEER_FMT_N);
        esp_log(3, "PEER_DEF", "I (%lu) %s: Video direction: offer=%d, local=%d, answer=%d\n", esp_log_timestamp(), "PEER_DEF", pc->remote_video_dir, pc->cfg.video_dir, video_dir);
    }
    sdp_video_codec_t codecs[4];
    int n = peer_gather_video(pc, codecs, 4);
    if (n > 0) {
        sdp_append_video_codecs(sdp, pc->vid_mid, video_dir, codecs, n, pc->cfg.video_info.codec);
    } else if (pc->b_remote_sdp_got) {
        int pt = peer_fmt_ok(pc->vid_first_fmt) ? pc->vid_first_fmt : 0;
        sdp_append_media_rejected(sdp, "video", pc->vid_mid, pt);
        return 0;
    } else {
        return -1;
    }
    if (peer_twcc_advertise(pc)) {
        sdp_append_twcc(sdp, peer_twcc_ext_id(pc));
        if (pc->cfg.video_info.codec == ESP_PEER_VIDEO_CODEC_MJPEG) {
            int jpeg_pt = PT_JPEG;
            for (int i = 0; i < n; i++) {
                if (codecs[i].codec == ESP_PEER_VIDEO_CODEC_MJPEG) {
                    jpeg_pt = codecs[i].pt;
                    break;
                }
            }
            sdp_append_transport_cc(sdp, jpeg_pt);
        }
    }
    return 0;
}

LOCAL int peer_prepare(peer_t *pc)
{
    int ret = 0;
    if (!pc->b_offer_created && (pc->cfg.role == ESP_PEER_ROLE_CONTROLLING || pc->b_remote_sdp_got)) {
        peer_set_state(pc, ESP_PEER_STATE_CANDIDATE_GATHERING);
        ret = peer_prepare_connection(pc, false);
        if (ret) {
            esp_log(1, "PEER_DEF", "E (%lu) %s: Fail to new connection\n", esp_log_timestamp(), "PEER_DEF");
            peer_set_state(pc, ESP_PEER_STATE_DISCONNECTED);
            return ret;
        }
        if (!pc->b_offer_created) {
            return ret;
        }
    }
    if (pc->b_remote_sdp_got && pc->remote_candidate_count > 0) {
        peer_set_state(pc, ESP_PEER_STATE_PAIRING);
    }
    return 0;
}

LOCAL void peer_on_pairing(peer_t *pc)
{
    if (agent_select_candidate_pair(pc->agent) < 0) {
        peer_set_state(pc, ESP_PEER_STATE_CONNECT_FAILED);
        return;
    }
    if (agent_connectivity_check(pc->agent)) {
        dtls_srtp_role_t role = peer_get_local_dtls_role(pc);
        peer_set_state(pc, ESP_PEER_STATE_PAIRED);
        esp_log(3, "PEER_DEF", "I (%lu) %s: Start DTLS role as %d\n", esp_log_timestamp(), "PEER_DEF", role);
        dtls_srtp_reset_session(pc->dtls_srtp, role);
        rtp_seq_tracker_reset(&pc->rtp_seq);
        peer_set_state(pc, ESP_PEER_STATE_CONNECTING);
    }
}

LOCAL int peer_on_connecting(peer_t *pc)
{
    if (dtls_srtp_handshake(pc->dtls_srtp) == 0) {
        esp_log(3, "PEER_DEF", "I (%lu) %s: DTLS handshake success\n", esp_log_timestamp(), "PEER_DEF");
        if (pc->cfg.enable_data_channel && pc->data_channel_accepted) {
            esp_log(3, "PEER_DEF", "I (%lu) %s: Sctp role as %d\n", esp_log_timestamp(), "PEER_DEF", dtls_srtp_get_role(pc->dtls_srtp) == DTLS_SRTP_ROLE_CLIENT);
            sctp_cfg_t sctp_cfg = {0};
            sctp_cfg.role = dtls_srtp_get_role(pc->dtls_srtp) == DTLS_SRTP_ROLE_CLIENT;
            sctp_cfg.manual_ch_create = pc->cfg.manual_ch_create;
            sctp_cfg.send_cache_size = pc->extra_cfg.data_ch_cfg.send_cache_size ? pc->extra_cfg.data_ch_cfg.send_cache_size : 100 * 1024;
            sctp_cfg.recv_cache_size = pc->extra_cfg.data_ch_cfg.recv_cache_size ? pc->extra_cfg.data_ch_cfg.recv_cache_size : 100 * 1024;
            sctp_cfg.buf_time_out = pc->extra_cfg.data_ch_cfg.cache_timeout;
            sctp_cfg.reliable_transport = agent_is_tcp_connected(pc->agent);
            sctp_cfg.on_connected = pc_data_channel_connected;
            sctp_cfg.on_open = pc_data_channel_opened;
            sctp_cfg.on_message = pc_data_channel_on_msg;
            sctp_cfg.on_close = pc_data_channel_closed;
            sctp_cfg.write_packet = pc_data_channel_on_write;
            sctp_cfg.ctx = pc;
            pc->sctp = sctp_create(&sctp_cfg);
            if (!pc->sctp) {
                esp_log(1, "PEER_DEF", "E (%lu) %s: Fail to create SCTP\n", esp_log_timestamp(), "PEER_DEF");
            }
        } else if (pc->cfg.enable_data_channel) {
            esp_log(2, "PEER_DEF", "W (%lu) %s: Skip SCTP: peer SDP no active m=application\n", esp_log_timestamp(), "PEER_DEF");
        }
        pc->video_pli_send_time = esp_timer_get_time() / 1000;
        peer_set_state(pc, ESP_PEER_STATE_CONNECTED);
    }
    return 0;
}

LOCAL int peer_main_loop(esp_peer_handle_t handle)
{
    peer_t *pc = handle;
    if (!pc) return ESP_PEER_ERR_INVALID_ARG;
    pc->looping = true;
    bool still_alive = false;
    if (pc->b_offer_created) still_alive = agent_check_alive(pc->agent) == 0;
    switch (pc->state) {
    case ESP_PEER_STATE_NEW_CONNECTION:
        peer_prepare(pc);
        break;
    case ESP_PEER_STATE_PAIRING:
        peer_on_pairing(pc);
        break;
    case ESP_PEER_STATE_CONNECTING:
        peer_on_connecting(pc);
        break;
    case ESP_PEER_STATE_CONNECTED:
        if (!still_alive || peer_recv_streams(pc) < 0) {
            esp_log(1, "PEER_DEF", "E (%lu) %s: Peer disconnected\n", esp_log_timestamp(), "PEER_DEF");
            peer_do_disconnect(pc, true);
            if (!pc->cfg.no_auto_reconnect) {
                esp_log(1, "PEER_DEF", "E (%lu) %s: Try to rebuild connection\n", esp_log_timestamp(), "PEER_DEF");
                peer_new_connection(pc);
            }
        } else {
            peer_send_rtcp_feedback(pc);
            if (pc->sctp) {
                measure_start("sctp_loop");
                sctp_main_loop(pc->sctp);
                measure_stop("sctp_loop");
                if (peer_tracing) esp_log(3, "PEER_DEF", "I (%lu) %s: sctp running\n", esp_log_timestamp(), "PEER_DEF");
            }
        }
        break;
    default:
        break;
    }
    pc->looping = false;
    return 0;
}

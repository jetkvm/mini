/* sdp interface.
 * Derived from sepfy/libpeer, Copyright (c) 2021 sepfy.
 * See LICENSE and LICENSES/ for copyright and licensing notices.
 */
#ifndef ESP_PEER_INTERNAL_SDP_H
#define ESP_PEER_INTERNAL_SDP_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "peer_types.h"

typedef struct {
    char content[10240];
    int filled;
} sdp_t;
typedef struct {
    uint8_t pt;
    esp_peer_audio_codec_t codec;
} sdp_audio_codec_t;
typedef struct {
    uint8_t pt;
    esp_peer_video_codec_t codec;
    uint32_t h264_profile;
} sdp_video_codec_t;

int sdp_append(sdp_t *sdp, const char *format, ...);
void sdp_reset(sdp_t *sdp);
void sdp_append_h264(sdp_t *sdp, int mid, esp_peer_media_dir_t dir);
void sdp_append_h264_with_pt(sdp_t *sdp, int mid, esp_peer_media_dir_t dir,
                            int pt, int profile);
void sdp_append_jpeg(sdp_t *sdp, int mid, esp_peer_media_dir_t dir);
void sdp_append_pcma(sdp_t *sdp, int mid, esp_peer_media_dir_t dir);
void sdp_append_pcmu(sdp_t *sdp, int mid, esp_peer_media_dir_t dir);
void sdp_append_opus(sdp_t *sdp, int mid, esp_peer_media_dir_t dir,
                     esp_peer_audio_stream_info_t *info);
void sdp_append_audio_codecs(sdp_t *sdp, int mid, esp_peer_media_dir_t dir,
                             const sdp_audio_codec_t *codecs, int n,
                             esp_peer_audio_codec_t send_codec);
void sdp_append_video_codecs(sdp_t *sdp, int mid, esp_peer_media_dir_t dir,
                             const sdp_video_codec_t *codecs, int n,
                             esp_peer_video_codec_t send_codec);
void sdp_append_media_rejected(sdp_t *sdp, const char *media, int mid, int pt);
void sdp_append_datachannel(sdp_t *sdp, int mid, bool active);
void sdp_append_datachannel_rejected(sdp_t *sdp, int mid);
void sdp_append_twcc(sdp_t *sdp, int ext_id);
void sdp_append_transport_cc(sdp_t *sdp, int pt);
void sdp_create(sdp_t *sdp, int stream_count);
_Static_assert(sizeof(sdp_t) == 10244, "vendor SDP size");
_Static_assert(offsetof(sdp_t, filled) == 10240, "vendor SDP offset");
_Static_assert(sizeof(sdp_audio_codec_t) == 8, "vendor audio codec size");
_Static_assert(sizeof(sdp_video_codec_t) == 12, "vendor video codec size");
#endif

/*
 * MIT License
 *
 * Copyright (c) 2021 sepfy
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "sdp.h"

enum { PT_PCMU = 0, PT_PCMA = 8, PT_G722 = 9, PT_JPEG = 26,
       PT_H264 = 102, PT_H264_1 = 106, PT_OPUS = 111 };
enum { SSRC_H264 = 1, SSRC_JPEG = 2, SSRC_PCMA = 4,
       SSRC_PCMU = 5, SSRC_OPUS = 6 };

static const char *get_dir_str(esp_peer_media_dir_t dir)
{
    switch (dir) {
    case ESP_PEER_MEDIA_DIR_SEND_ONLY: return "sendonly";
    case ESP_PEER_MEDIA_DIR_RECV_ONLY: return "recvonly";
    case ESP_PEER_MEDIA_DIR_SEND_RECV: return "sendrecv";
    default: return "inactive";
    }
}

int sdp_append(sdp_t *sdp, const char *format, ...)
{
    int left = sizeof(sdp->content) - sdp->filled;
    char *attr = sdp->content + sdp->filled;
    va_list argptr;
    va_start(argptr, format);
    int n = vsnprintf(attr, left, format, argptr);
    va_end(argptr);
    if (n + 3 >= left) {
        return -1;
    }
    sdp->filled += n;
    if (sdp->content[sdp->filled - 1] != '\n') {
        strcpy(sdp->content + sdp->filled, "\r\n");
        sdp->filled += 2;
    }
    return 0;
}

void sdp_reset(sdp_t *sdp)
{
    sdp->filled = 0;
    memset(sdp->content, 0, sizeof(sdp->content));
}

void sdp_append_h264(sdp_t *sdp, int mid, esp_peer_media_dir_t dir)
{
    sdp_append(sdp, "m=video 9 UDP/TLS/RTP/SAVPF %d %d", PT_H264, PT_H264_1);
    sdp_append(sdp, "a=rtpmap:%d H264/90000", PT_H264);
    sdp_append(sdp, "a=rtcp-fb:%d goog-remb", PT_H264);
    sdp_append(sdp, "a=rtcp-fb:%d transport-cc", PT_H264);
    sdp_append(sdp, "a=rtcp-fb:%d ccm fir", PT_H264);
    sdp_append(sdp, "a=rtcp-fb:%d nack", PT_H264);
    sdp_append(sdp, "a=rtcp-fb:%d nack pli", PT_H264);
    sdp_append(sdp, "a=fmtp:%d level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d001f", PT_H264);
    sdp_append(sdp, "a=rtpmap:%d H264/90000", PT_H264_1);
    sdp_append(sdp, "a=rtcp-fb:%d goog-remb", PT_H264_1);
    sdp_append(sdp, "a=rtcp-fb:%d transport-cc", PT_H264_1);
    sdp_append(sdp, "a=rtcp-fb:%d ccm fir", PT_H264_1);
    sdp_append(sdp, "a=rtcp-fb:%d nack", PT_H264_1);
    sdp_append(sdp, "a=rtcp-fb:%d nack pli", PT_H264_1);
    sdp_append(sdp, "a=fmtp:%d level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d001f", PT_H264_1);
    sdp_append(sdp, "a=ssrc:%d cname:webrtc-h264", SSRC_H264);
    sdp_append(sdp, "a=ssrc:%d msid:esp_stream v0", SSRC_H264);
    sdp_append(sdp, "a=%s", get_dir_str(dir));
    sdp_append(sdp, "a=mid:%d", mid);
    sdp_append(sdp, "a=msid:esp_stream v0");
    sdp_append(sdp, "c=IN IP4 0.0.0.0");
    sdp_append(sdp, "a=rtcp-mux");
}

void sdp_append_h264_with_pt(sdp_t *sdp, int mid, esp_peer_media_dir_t dir,
                            int pt, int profile)
{
    sdp_append(sdp, "m=video 9 UDP/TLS/RTP/SAVPF %d", pt);
    sdp_append(sdp, "a=rtpmap:%d H264/90000", pt);
    sdp_append(sdp, "a=rtcp-fb:%d goog-remb", pt);
    sdp_append(sdp, "a=rtcp-fb:%d transport-cc", pt);
    sdp_append(sdp, "a=rtcp-fb:%d ccm fir", pt);
    sdp_append(sdp, "a=rtcp-fb:%d nack", pt);
    sdp_append(sdp, "a=rtcp-fb:%d nack pli", pt);
    sdp_append(sdp, "a=fmtp:%d level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=%x", pt, profile);
    sdp_append(sdp, "a=ssrc:%d cname:webrtc-h264", SSRC_H264);
    sdp_append(sdp, "a=ssrc:%d msid:esp_stream v0", SSRC_H264);
    sdp_append(sdp, "a=%s", get_dir_str(dir));
    sdp_append(sdp, "a=mid:%d", mid);
    sdp_append(sdp, "a=msid:esp_stream v0");
    sdp_append(sdp, "c=IN IP4 0.0.0.0");
    sdp_append(sdp, "a=rtcp-mux");
}

void sdp_append_jpeg(sdp_t *sdp, int mid, esp_peer_media_dir_t dir)
{
    sdp_append(sdp, "m=video 9 UDP/TLS/RTP/SAVPF %d", PT_JPEG);
    sdp_append(sdp, "a=rtpmap:%d JPEG/90000", PT_JPEG);
    sdp_append(sdp, "a=ssrc:%d cname:webrtc-jpeg", SSRC_JPEG);
    sdp_append(sdp, "a=ssrc:%d msid:esp_stream v0", SSRC_JPEG);
    sdp_append(sdp, "a=%s", get_dir_str(dir));
    sdp_append(sdp, "a=mid:%d", mid);
    sdp_append(sdp, "a=msid:esp_stream v0");
    sdp_append(sdp, "c=IN IP4 0.0.0.0");
    sdp_append(sdp, "a=rtcp-mux");
}

void sdp_append_pcma(sdp_t *sdp, int mid, esp_peer_media_dir_t dir)
{
    sdp_append(sdp, "m=audio 9 UDP/TLS/RTP/SAVPF 8");
    sdp_append(sdp, "c=IN IP4 0.0.0.0");
    sdp_append(sdp, "a=mid:%d", mid);
    sdp_append(sdp, "a=msid:esp_stream a0");
    sdp_append(sdp, "a=rtcp-mux");
    sdp_append(sdp, "a=rtpmap:8 PCMA/8000");
    sdp_append(sdp, "a=ssrc:%d cname:EspSSpbA0", SSRC_PCMA);
    sdp_append(sdp, "a=ssrc:%d msid:esp_stream a0", SSRC_PCMA);
    sdp_append(sdp, "a=%s", get_dir_str(dir));
}

void sdp_append_pcmu(sdp_t *sdp, int mid, esp_peer_media_dir_t dir)
{
    sdp_append(sdp, "m=audio 9 UDP/TLS/RTP/SAVP 0");
    sdp_append(sdp, "a=rtpmap:0 PCMU/8000");
    sdp_append(sdp, "a=ssrc:%d cname:webrtc-pcmu", SSRC_PCMU);
    sdp_append(sdp, "a=ssrc:%d msid:esp_stream a0", SSRC_PCMU);
    sdp_append(sdp, "a=mid:%d", mid);
    sdp_append(sdp, "a=msid:esp_stream a0");
    sdp_append(sdp, "c=IN IP4 0.0.0.0");
    sdp_append(sdp, "a=rtcp-mux");
    sdp_append(sdp, "a=%s", get_dir_str(dir));
}

void sdp_append_opus(sdp_t *sdp, int mid, esp_peer_media_dir_t dir,
                     esp_peer_audio_stream_info_t *info)
{
    sdp_append(sdp, "m=audio 9 UDP/TLS/RTP/SAVP 111");
    char fmt[32];
    snprintf(fmt, sizeof(fmt) - 1, "a=rtpmap:111 opus/%d/%d", 48000, 2);
    sdp_append(sdp, fmt);
    sdp_append(sdp, "a=ssrc:%d cname:webrtc-opus", SSRC_OPUS);
    sdp_append(sdp, "a=ssrc:%d msid:esp_stream a0", SSRC_OPUS);
    sdp_append(sdp, "a=mid:%d", mid);
    sdp_append(sdp, "a=msid:esp_stream a0");
    sdp_append(sdp, "c=IN IP4 0.0.0.0");
    sdp_append(sdp, "a=rtcp-mux");
    sdp_append(sdp, "a=%s", get_dir_str(dir));
}

static uint32_t sdp_audio_ssrc(esp_peer_audio_codec_t codec)
{
    switch (codec) {
    case ESP_PEER_AUDIO_CODEC_G711U: return SSRC_PCMU;
    case ESP_PEER_AUDIO_CODEC_OPUS: return SSRC_OPUS;
    default: return SSRC_PCMA;
    }
}

static uint32_t sdp_video_ssrc(esp_peer_video_codec_t codec)
{
    return codec == ESP_PEER_VIDEO_CODEC_MJPEG ? SSRC_JPEG : SSRC_H264;
}

static void sdp_append_audio_rtpmap(sdp_t *sdp, int pt, esp_peer_audio_codec_t codec)
{
    switch (codec) {
    case ESP_PEER_AUDIO_CODEC_G711U:
        sdp_append(sdp, "a=rtpmap:%d PCMU/8000", pt);
        break;
    case ESP_PEER_AUDIO_CODEC_OPUS:
        sdp_append(sdp, "a=rtpmap:%d opus/48000/2", pt);
        break;
    default:
        sdp_append(sdp, "a=rtpmap:%d PCMA/8000", pt);
        break;
    }
}

static void sdp_append_h264_attrs(sdp_t *sdp, int pt, uint32_t profile)
{
    if (profile == 0) {
        profile = 0x4d001f;
    }
    sdp_append(sdp, "a=rtpmap:%d H264/90000", pt);
    sdp_append(sdp, "a=rtcp-fb:%d goog-remb", pt);
    sdp_append(sdp, "a=rtcp-fb:%d transport-cc", pt);
    sdp_append(sdp, "a=rtcp-fb:%d ccm fir", pt);
    sdp_append(sdp, "a=rtcp-fb:%d nack", pt);
    sdp_append(sdp, "a=rtcp-fb:%d nack pli", pt);
    sdp_append(sdp, "a=fmtp:%d level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=%x", pt, profile);
}

static void sdp_append_video_common(sdp_t *sdp, int mid, esp_peer_media_dir_t dir,
                                    esp_peer_video_codec_t codec)
{
    uint32_t ssrc = sdp_video_ssrc(codec);
    const char *cname = codec == ESP_PEER_VIDEO_CODEC_MJPEG ? "webrtc-jpeg" : "webrtc-h264";
    sdp_append(sdp, "a=ssrc:%d cname:%s", ssrc, cname);
    sdp_append(sdp, "a=ssrc:%d msid:esp_stream v0", ssrc);
    sdp_append(sdp, "a=%s", get_dir_str(dir));
    sdp_append(sdp, "a=mid:%d", mid);
    sdp_append(sdp, "a=msid:esp_stream v0");
    sdp_append(sdp, "c=IN IP4 0.0.0.0");
    sdp_append(sdp, "a=rtcp-mux");
}

void sdp_append_audio_codecs(sdp_t *sdp, int mid, esp_peer_media_dir_t dir,
                             const sdp_audio_codec_t *codecs, int n,
                             esp_peer_audio_codec_t send_codec)
{
    if (!sdp || !codecs || n <= 0) {
        return;
    }
    char mline[96];
    int pos = snprintf(mline, sizeof(mline), "m=audio 9 UDP/TLS/RTP/SAVPF");
    for (int i = 0; i < n && pos > 0 && pos < (int)sizeof(mline) - 8; ++i) {
        pos += snprintf(mline + pos, sizeof(mline) - pos, " %d", codecs[i].pt);
    }
    sdp_append(sdp, "%s", mline);
    sdp_append(sdp, "c=IN IP4 0.0.0.0");
    sdp_append(sdp, "a=mid:%d", mid);
    sdp_append(sdp, "a=msid:esp_stream a0");
    sdp_append(sdp, "a=rtcp-mux");
    for (int i = 0; i < n; ++i) {
        sdp_append_audio_rtpmap(sdp, codecs[i].pt, codecs[i].codec);
    }
    uint32_t ssrc = sdp_audio_ssrc(send_codec != ESP_PEER_AUDIO_CODEC_NONE ? send_codec : codecs[0].codec);
    sdp_append(sdp, "a=ssrc:%d cname:EspSSpbA0", ssrc);
    sdp_append(sdp, "a=ssrc:%d msid:esp_stream a0", ssrc);
    sdp_append(sdp, "a=%s", get_dir_str(dir));
}

void sdp_append_video_codecs(sdp_t *sdp, int mid, esp_peer_media_dir_t dir,
                             const sdp_video_codec_t *codecs, int n,
                             esp_peer_video_codec_t send_codec)
{
    if (!sdp || !codecs || n <= 0) {
        return;
    }
    esp_peer_video_codec_t ssrc_codec = send_codec != ESP_PEER_VIDEO_CODEC_NONE ? send_codec : codecs[0].codec;
    if (n == 1) {
        if (codecs[0].codec == ESP_PEER_VIDEO_CODEC_MJPEG) {
            if (codecs[0].pt == PT_JPEG) {
                sdp_append_jpeg(sdp, mid, dir);
            } else {
                sdp_append(sdp, "m=video 9 UDP/TLS/RTP/SAVPF %d", codecs[0].pt);
                sdp_append(sdp, "a=rtpmap:%d JPEG/90000", codecs[0].pt);
                sdp_append_video_common(sdp, mid, dir, ssrc_codec);
            }
        } else if (codecs[0].pt != 0 && (codecs[0].pt != PT_H264 || codecs[0].h264_profile != 0)) {
            sdp_append_h264_with_pt(sdp, mid, dir, codecs[0].pt, codecs[0].h264_profile);
        } else {
            sdp_append_h264(sdp, mid, dir);
        }
        return;
    }
    char mline[96];
    int pos = snprintf(mline, sizeof(mline), "m=video 9 UDP/TLS/RTP/SAVPF");
    for (int i = 0; i < n && pos > 0 && pos < (int)sizeof(mline) - 8; ++i) {
        pos += snprintf(mline + pos, sizeof(mline) - pos, " %d", codecs[i].pt);
    }
    sdp_append(sdp, "%s", mline);
    for (int i = 0; i < n; ++i) {
        if (codecs[i].codec == ESP_PEER_VIDEO_CODEC_MJPEG) {
            sdp_append(sdp, "a=rtpmap:%d JPEG/90000", codecs[i].pt);
        } else {
            sdp_append_h264_attrs(sdp, codecs[i].pt, codecs[i].h264_profile);
        }
    }
    sdp_append_video_common(sdp, mid, dir, ssrc_codec);
}

void sdp_append_media_rejected(sdp_t *sdp, const char *media, int mid, int pt)
{
    sdp_append(sdp, "m=%s 0 UDP/TLS/RTP/SAVPF %d", media, pt);
    sdp_append(sdp, "c=IN IP4 0.0.0.0");
    sdp_append(sdp, "a=mid:%d", mid);
    sdp_append(sdp, "a=inactive");
}

void sdp_append_datachannel(sdp_t *sdp, int mid, bool active)
{
    sdp_append(sdp, "m=application 50712 UDP/DTLS/SCTP webrtc-datachannel");
    sdp_append(sdp, "a=mid:%d", mid);
    sdp_append(sdp, "a=sctp-port:%d", 5000);
    sdp_append(sdp, "c=IN IP4 0.0.0.0");
    sdp_append(sdp, "a=max-message-size:163840");
    if (!active) {
        sdp_append(sdp, "a=inactive");
    }
}

void sdp_append_datachannel_rejected(sdp_t *sdp, int mid)
{
    sdp_append(sdp, "m=application 0 UDP/DTLS/SCTP webrtc-datachannel");
    sdp_append(sdp, "a=mid:%d", mid);
}

void sdp_append_twcc(sdp_t *sdp, int ext_id)
{
    sdp_append(sdp, "a=extmap:%d http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01", ext_id);
}

void sdp_append_transport_cc(sdp_t *sdp, int pt)
{
    sdp_append(sdp, "a=rtcp-fb:%d transport-cc", pt);
}

void sdp_create(sdp_t *sdp, int stream_count)
{
    char bundle[64];
    sdp->filled = 0;
    sdp_append(sdp, "v=0");
    sdp_append(sdp, "o=- 7801614086411001801 2 IN IP4 0.0.0.0");
    sdp_append(sdp, "s=-");
    sdp_append(sdp, "t=0 0");
    sdp_append(sdp, "a=msid-semantic: esp-webrtc");
    memset(bundle, 0, sizeof(bundle));
    strcat(bundle, "a=group:BUNDLE");
    int mid = 0;
    int len = strlen(bundle);
    for (mid = 0; mid < stream_count; ++mid) {
        bundle[len++] = ' ';
        bundle[len++] = '0' + mid;
    }
    sdp_append(sdp, bundle);
}

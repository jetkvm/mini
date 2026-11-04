#include "lwip/def.h"
/* Reconstructed from esp_peer 1.5.5 rtcp.c.obj, with public libpeer ancestry. */
#include <string.h>
#include "rtcp.h"

int rtcp_probe(uint8_t *packet, size_t size)
{
    if (size < 8)
        return -1;
    return packet[1] >= 192 && packet[1] < 224;
}

int rtcp_get_pli(uint8_t *packet, int len, uint32_t ssrc)
{
    if (packet == NULL || len != 12)
        return -1;

    memset(packet, 0, len);
    rtcp_header_t *rtcp_header = (rtcp_header_t *)packet;
    rtcp_header->version = 2;
    rtcp_header->type = RTCP_PSFB;
    rtcp_header->rc = 1;
    rtcp_header->length = htons((len / 4) - 1);
    ssrc = htonl(ssrc);
    memcpy(packet + 4, &ssrc, 4);
    memcpy(packet + 8, &ssrc, 4);
    return len;
}

int rtcp_get_fir(uint8_t *packet, int len, int *seqnr)
{
    if (packet == NULL || len != 20 || seqnr == NULL)
        return -1;

    memset(packet, 0, len);
    rtcp_header_t *rtcp = (rtcp_header_t *)packet;
    *seqnr = *seqnr + 1;
    if (*seqnr < 0 || *seqnr >= 256)
        *seqnr = 0;

    rtcp->version = 2;
    rtcp->type = RTCP_PSFB;
    rtcp->rc = 4;
    rtcp->length = htons((len / 4) - 1);
    /* Preserve the vendor's typed-pointer offset, including its overrun. */
    rtcp_fir_t *fir = (rtcp_fir_t *)((rtcp_fb_t *)rtcp + 8);
    fir->seqnr = htonl(*seqnr << 24);
    return len;
}

int rtcp_get_fb(rtcp_fb_t *fb, uint32_t ssrc, uint32_t media_ssrc, int lost_cnt)
{
    uint8_t *ver = (uint8_t *)fb;
    uint8_t *type = ver + 1;
    *ver = 0x80 | RTCP_FEEDBACK_NACK;
    *type = RTCP_RTPFB;
    uint16_t *length = (uint16_t *)(ver + 2);
    *length = htons(lost_cnt + 2);
    fb->ssrc = htonl(ssrc);
    fb->media = htonl(media_ssrc);
    return 0;
}

int rtcp_parse_header(uint8_t *data, int len, rtcp_header_t *header)
{
    if (data == NULL || len < 12)
        return -1;
    header->version = data[0] >> 6;
    header->padding = (data[0] >> 5) & 1;
    header->rc = ((rtcp_header_t *)data)->rc;
    header->type = data[1];
    header->length = ntohs(((rtcp_header_t *)data)->length);
    return 0;
}

int rtcp_parse_remb(const uint8_t *packet, int len, uint32_t *bitrate, uint32_t *ssrc)
{
    if (packet == NULL || bitrate == NULL || len < 20)
        return -1;
    if ((packet[0] & 0x1f) != RTCP_FEEDBACK_AFB || packet[1] != RTCP_PSFB)
        return -1;
    if (memcmp(packet + 12, "REMB", 4) != 0)
        return -1;

    uint32_t br = ((uint32_t)packet[16] << 24) | ((uint32_t)packet[17] << 16) |
                  ((uint32_t)packet[18] << 8) | packet[19];
    uint8_t num_ssrc = br >> 24;
    uint8_t exp = (br >> 18) & 0x3f;
    uint32_t mantissa = br & 0x3ffff;
    if (len < 20 + num_ssrc * 4)
        return -1;

    uint64_t bps = (uint64_t)mantissa << exp;
    *bitrate = bps > UINT32_MAX ? UINT32_MAX : (uint32_t)bps;
    if (ssrc) {
        *ssrc = 0;
        if (num_ssrc) {
            *ssrc = ((uint32_t)packet[20] << 24) | ((uint32_t)packet[21] << 16) |
                    ((uint32_t)packet[22] << 8) | packet[23];
        }
    }
    return 0;
}

int rtcp_parse_fb(uint8_t *packet, int len, rtcp_fb_t *fb)
{
    if (packet == NULL || len < 12)
        return -1;
    rtcp_parse_header(packet, len, &fb->header);
    memcpy(&fb->ssrc, packet + 4, 4);
    memcpy(&fb->media, packet + 8, 4);
    fb->ssrc = ntohl(fb->ssrc);
    fb->media = ntohl(fb->media);
    return 0;
}

int rtcp_parse_rr(uint8_t *packet, int len, rtcp_rr_t *rr)
{
    if (len < sizeof(rtcp_header_t) + sizeof(rtcp_report_block_t))
        return -1;
    rtcp_parse_header(packet, len, &rr->header);
    memcpy(&rr->report_block[0], packet + 8, 6 * sizeof(uint32_t));
    return 0;
}

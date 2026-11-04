#include "rtcp_feedback.h"

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool webrtc_rtcp_find_report_block(const uint8_t *data, size_t size,
                                   uint32_t video_ssrc,
                                   webrtc_rtcp_report_block_t *block)
{
    if (!data || !block) {
        return false;
    }
    bool found = false;
    while (size > 0) {
        if (size < 4 || (data[0] >> 6) != 2) {
            return false;
        }
        const size_t bytes = (((size_t)data[2] << 8) | data[3]) * 4 + 4;
        if (bytes > size) {
            return false;
        }
        /* SR: 8-byte header + 20-byte sender info; RR: 8-byte header. */
        const size_t first = data[1] == 200 ? 28 : data[1] == 201 ? 8 : 0;
        const size_t count = data[0] & 0x1f;
        if (first) {
            if (first + count * 24 > bytes) {
                return false;
            }
            for (size_t i = 0; i < count; ++i) {
                const uint8_t *rb = data + first + i * 24;
                if (read_be32(rb) != video_ssrc) continue;
                uint32_t lost = ((uint32_t)rb[5] << 16) | ((uint32_t)rb[6] << 8) | rb[7];
                block->fraction_lost = rb[4];
                block->cumulative_lost = (lost & 0x800000U) ? (int32_t)(lost | 0xff000000U) : (int32_t)lost;
                block->highest_sequence = read_be32(rb + 8);
                found = true;
            }
        }
        data += bytes;
        size -= bytes;
    }
    return found;
}

bool webrtc_rtcp_has_video_pli(const uint8_t *data, size_t size,
                               uint32_t video_ssrc)
{
    if (!data || size == 0) {
        return false;
    }
    bool found = false;
    while (size > 0) {
        if (size < 4 || (data[0] >> 6) != 2) {
            return false;
        }
        size_t bytes = (((size_t)data[2] << 8) | data[3]) * 4 + 4;
        if (bytes > size) {
            return false;
        }
        size_t payload_bytes = bytes;
        if (data[0] & 0x20) {
            uint8_t padding = data[bytes - 1];
            if (bytes != size || padding == 0 || padding > bytes - 4) {
                return false;
            }
            payload_bytes -= padding;
        }
        if (data[1] == 206 && (data[0] & 0x1f) == 1) {
            if (payload_bytes != 12) {
                return false;
            }
            uint32_t media_ssrc = ((uint32_t)data[8] << 24) |
                ((uint32_t)data[9] << 16) | ((uint32_t)data[10] << 8) | data[11];
            found |= media_ssrc == video_ssrc;
        }
        data += bytes;
        size -= bytes;
    }
    return found;
}

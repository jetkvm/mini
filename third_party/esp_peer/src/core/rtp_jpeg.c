/* rtp jpeg implementation.
 * See LICENSE and LICENSES/ for copyright and licensing notices.
 */
#include "peer_platform.h"
#include "rtp_jpeg.h"
#include <string.h>

static const uint8_t jpeg_bits_dc_luminance[] = {
    0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,
    0,
};

static const uint8_t jpeg_val_dc[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
};

static const uint8_t jpeg_bits_dc_chrominance[] = {
    0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0,
    0,
};

static const uint8_t jpeg_bits_ac_luminance[] = {
    0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1,
    125,
};

static const uint8_t jpeg_val_ac_luminance[] = {
    1, 2, 3, 0, 4, 17, 5, 18, 33, 49, 65, 6, 19, 81, 97, 7,
    34, 113, 20, 50, 129, 145, 161, 8, 35, 66, 177, 193, 21, 82, 209, 240,
    36, 51, 98, 114, 130, 9, 10, 22, 23, 24, 25, 26, 37, 38, 39, 40,
    41, 42, 52, 53, 54, 55, 56, 57, 58, 67, 68, 69, 70, 71, 72, 73,
    74, 83, 84, 85, 86, 87, 88, 89, 90, 99, 100, 101, 102, 103, 104, 105,
    106, 115, 116, 117, 118, 119, 120, 121, 122, 131, 132, 133, 134, 135, 136, 137,
    138, 146, 147, 148, 149, 150, 151, 152, 153, 154, 162, 163, 164, 165, 166, 167,
    168, 169, 170, 178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195, 196, 197,
    198, 199, 200, 201, 202, 210, 211, 212, 213, 214, 215, 216, 217, 218, 225, 226,
    227, 228, 229, 230, 231, 232, 233, 234, 241, 242, 243, 244, 245, 246, 247, 248,
    249, 250,
};

static const uint8_t jpeg_bits_ac_chrominance[] = {
    0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2,
    119,
};

static const uint8_t jpeg_val_ac_chrominance[] = {
    0, 1, 2, 3, 17, 4, 5, 33, 49, 6, 18, 65, 81, 7, 97, 113,
    19, 34, 50, 129, 8, 20, 66, 145, 161, 177, 193, 9, 35, 51, 82, 240,
    21, 98, 114, 209, 10, 22, 36, 52, 225, 37, 241, 23, 24, 25, 26, 38,
    39, 40, 41, 42, 53, 54, 55, 56, 57, 58, 67, 68, 69, 70, 71, 72,
    73, 74, 83, 84, 85, 86, 87, 88, 89, 90, 99, 100, 101, 102, 103, 104,
    105, 106, 115, 116, 117, 118, 119, 120, 121, 122, 130, 131, 132, 133, 134, 135,
    136, 137, 138, 146, 147, 148, 149, 150, 151, 152, 153, 154, 162, 163, 164, 165,
    166, 167, 168, 169, 170, 178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195,
    196, 197, 198, 199, 200, 201, 202, 210, 211, 212, 213, 214, 215, 216, 217, 218,
    226, 227, 228, 229, 230, 231, 232, 233, 234, 242, 243, 244, 245, 246, 247, 248,
    249, 250,
};

static const uint8_t jpeg_default_quantizers[] = {
    16, 11, 12, 14, 12, 10, 16, 14, 13, 14, 18, 17, 16, 19, 24, 40,
    26, 24, 22, 22, 24, 49, 35, 37, 29, 40, 58, 51, 61, 60, 57, 51,
    56, 55, 64, 72, 92, 78, 64, 68, 87, 69, 55, 56, 80, 109, 81, 87,
    95, 98, 103, 104, 103, 62, 77, 113, 121, 112, 100, 120, 92, 101, 103, 99,
    17, 18, 18, 24, 21, 24, 47, 26, 26, 47, 99, 66, 56, 66, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

static int rtp_decoder_append_frame(rtp_decoder_t *rtp_decoder, const uint8_t *data, size_t size)
{
    if (rtp_decoder->frame_size + size > rtp_decoder->frame_capacity) {
        uint32_t new_capacity = rtp_decoder->frame_capacity ? rtp_decoder->frame_capacity * 2 : 4096;
        while (new_capacity < rtp_decoder->frame_size + size) {
            new_capacity = new_capacity * 3 / 2;
        }
        uint8_t *new_buf = media_lib_realloc(rtp_decoder->frame_buf, new_capacity);
        if (!new_buf) return -1;
        rtp_decoder->frame_buf = new_buf;
        rtp_decoder->frame_capacity = new_capacity;
    }
    memcpy(rtp_decoder->frame_buf + rtp_decoder->frame_size, data, size);
    rtp_decoder->frame_size += size;
    return 0;
}

static void jpeg_put_marker(uint8_t **p, uint8_t code)
{
    *(*p)++ = 0xff;
    *(*p)++ = code;
}

static int jpeg_put_huffman_table(uint8_t **p, int table_class, int table_id,
                                  const uint8_t *bits_table, const uint8_t *value_table)
{
    uint8_t tc = (table_class << 4) | table_id;
    *(*p)++ = tc;
    int n = 0;
    for (int i = 1; i <= 16; i++) {
        n += bits_table[i];
        *(*p)++ = bits_table[i];
    }
    for (int i = 0; i < n; i++) *(*p)++ = value_table[i];
    return 17 + n;
}

static int jpeg_create_header(uint8_t *buf, int size, uint8_t type, uint8_t w_blocks, uint8_t h_blocks,
                              const uint8_t *qtable, int nb_qtable, uint16_t dri)
{
    uint8_t *p = buf;
    uint8_t *end = buf + size;
    uint16_t width = (uint16_t)w_blocks << 3;
    uint16_t height = (uint16_t)h_blocks << 3;
    if (size < 600) return -1;
    jpeg_put_marker(&p, 0xd8);
    jpeg_put_marker(&p, 0xe0);
    *p++ = 0;
    *p++ = 16;
    memcpy(p, "JFIF\0", 5);
    p += 5;
    *p++ = 1;
    *p++ = 2;
    *p++ = 0;
    *p++ = 0;
    *p++ = 1;
    *p++ = 0;
    *p++ = 1;
    *p++ = 0;
    *p++ = 0;
    if (dri) {
        jpeg_put_marker(&p, 0xdd);
        *p++ = 0;
        *p++ = 4;
        *p++ = dri >> 8;
        *p++ = dri;
    }
    jpeg_put_marker(&p, 0xdb);
    uint16_t dqt_len = 2 + nb_qtable * 65;
    *p++ = dqt_len >> 8;
    *p++ = dqt_len;
    for (int i = 0; i < nb_qtable; i++) {
        *p++ = i;
        memcpy(p, qtable + i * 64, 64);
        p += 64;
    }
    jpeg_put_marker(&p, 0xc4);
    uint8_t *dht_len_ptr = p;
    p += 2;
    int dht_size = 2;
    dht_size += jpeg_put_huffman_table(&p, 0, 0, jpeg_bits_dc_luminance, jpeg_val_dc);
    dht_size += jpeg_put_huffman_table(&p, 0, 1, jpeg_bits_dc_chrominance, jpeg_val_dc);
    dht_size += jpeg_put_huffman_table(&p, 1, 0, jpeg_bits_ac_luminance, jpeg_val_ac_luminance);
    dht_size += jpeg_put_huffman_table(&p, 1, 1, jpeg_bits_ac_chrominance, jpeg_val_ac_chrominance);
    dht_len_ptr[0] = dht_size >> 8;
    dht_len_ptr[1] = dht_size;
    jpeg_put_marker(&p, 0xc0);
    *p++ = 0;
    *p++ = 17;
    *p++ = 8;
    *p++ = height >> 8;
    *p++ = h_blocks << 3;
    *p++ = width >> 8;
    *p++ = w_blocks << 3;
    *p++ = 3;
    *p++ = 1;
    *p++ = type ? 0x22 : 0x21;
    *p++ = 0;
    *p++ = 2;
    *p++ = 0x11;
    *p++ = nb_qtable == 2 ? 1 : 0;
    *p++ = 3;
    *p++ = 0x11;
    *p++ = nb_qtable == 2 ? 1 : 0;
    jpeg_put_marker(&p, 0xda);
    *p++ = 0;
    *p++ = 12;
    *p++ = 3;
    *p++ = 1;
    *p++ = 0;
    *p++ = 2;
    *p++ = 0x11;
    *p++ = 3;
    *p++ = 0x11;
    *p++ = 0;
    *p++ = 63;
    *p++ = 0;
    if (p > end) return -1;
    return p - buf;
}

static void create_default_qtables(uint8_t *qtables, uint8_t q)
{
    int factor = q;
    uint16_t S;
    if (factor < 1) factor = 1;
    else if (factor > 99) factor = 99;
    if (factor < 50) S = 5000 / factor;
    else {
        S = 100 - factor;
        S *= 2;
    }
    for (int i = 0; i < 128; i++) {
        int val = (jpeg_default_quantizers[i] * S + 50) / 100;
        if (val < 1) val = 1;
        if (val > 255) val = 255;
        qtables[i] = val;
    }
}

static int jpeg_match_huffman_table(const uint8_t *p, size_t avail, uint8_t table_class, uint8_t table_id,
                                   const uint8_t *bits_table, const uint8_t *value_table, int value_n,
                                   size_t *consumed)
{
    if (avail < 17) return 0;
    if (p[0] != (uint8_t)((table_class << 4) | table_id)) return 0;
    if (memcmp(p + 1, bits_table + 1, 16)) return 0;
    if (avail < 17 + value_n) return 0;
    if (memcmp(p + 17, value_table, value_n)) return 0;
    *consumed = 17 + value_n;
    return 1;
}

static int jpeg_dht_collect_standard(const uint8_t *dht, size_t dht_len, uint8_t *found)
{
    size_t off = 0;
    while (off < dht_len) {
        size_t consumed = 0;
        const uint8_t *p = dht + off;
        size_t avail = dht_len - off;
        if (jpeg_match_huffman_table(p, avail, 0, 0, jpeg_bits_dc_luminance, jpeg_val_dc, 12, &consumed) ||
            jpeg_match_huffman_table(p, avail, 0, 1, jpeg_bits_dc_chrominance, jpeg_val_dc, 12, &consumed) ||
            jpeg_match_huffman_table(p, avail, 1, 0, jpeg_bits_ac_luminance, jpeg_val_ac_luminance, 162, &consumed) ||
            jpeg_match_huffman_table(p, avail, 1, 1, jpeg_bits_ac_chrominance, jpeg_val_ac_chrominance, 162, &consumed)) {
            uint8_t tc = p[0];
            *found |= (uint8_t)(1 << ((((tc >> 4) & 1) * 2) + (tc & 1)));
            off += consumed;
        } else return -1;
    }
    return 0;
}

static int jpeg_prepare_frame(const uint8_t *buf, size_t size, const uint8_t **qtables, int *nb_qtables,
                              bool info_ready, uint8_t *type_inout, uint16_t *width_inout, uint16_t *height_inout,
                              const uint8_t **scan_out, size_t *scan_size)
{
    int nb = 0;
    uint8_t type = info_ready ? *type_inout : 0xff;
    uint8_t huffman_found = info_ready ? 15 : 0;
    uint16_t width = info_ready ? *width_inout : 0;
    uint16_t height = info_ready ? *height_inout : 0;
    size_t i = 0;
    if (size < 4 || buf[0] != 0xff || buf[1] != 0xd8) return -1;
    while (i + 3 < size) {
        if (buf[i] != 0xff) { i++; continue; }
        uint8_t marker = buf[i + 1];
        if (marker == 0 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0xd8) {
            i += 2;
            continue;
        }
        if (marker == 0xd9) return -1;
        uint16_t seg_len = (buf[i + 2] << 8) | buf[i + 3];
        if (seg_len < 2 || i + 2 + seg_len > size) return -1;
        if (marker == 0xdb) {
            size_t off = i + 4;
            size_t end = i + 2 + seg_len;
            while (off + 65 <= end) {
                if (buf[off] > 15 || nb >= 4) return -1;
                qtables[nb++] = buf + off + 1;
                off += 65;
            }
        } else if (marker == 0xc4) {
            if (!info_ready && jpeg_dht_collect_standard(buf + i + 4, seg_len - 2, &huffman_found)) return -1;
        } else if (marker == 0xc0) {
            if (!info_ready) {
                if (seg_len < 17 || buf[i + 4] != 8 || buf[i + 9] != 3 ||
                    buf[i + 14] != 0x11 || buf[i + 17] != 0x11) return -1;
                if (buf[i + 11] == 0x21) type = 0;
                else if (buf[i + 11] == 0x22) type = 1;
                else return -1;
                height = (buf[i + 5] << 8) | buf[i + 6];
                width = (buf[i + 7] << 8) | buf[i + 8];
                if (!width || !height || width > 2040 || height > 2040) return -1;
            }
        } else if (marker == 0xda) {
            size_t scan_start = i + 2 + seg_len;
            size_t scan_end = size;
            for (size_t j = size; j >= 2; j--) {
                if (buf[j - 2] == 0xff && buf[j - 1] == 0xd9) {
                    scan_end = j - 2;
                    break;
                }
            }
            if (scan_end < scan_start || type == 0xff || !width || !height ||
                (!info_ready && huffman_found != 15)) return -1;
            *nb_qtables = nb;
            *type_inout = type;
            *width_inout = width;
            *height_inout = height;
            *scan_out = buf + scan_start;
            *scan_size = scan_end - scan_start;
            return 0;
        }
        i += 2 + seg_len;
    }
    return -1;
}

static void jpeg_write_main_header(uint8_t *p, uint32_t off, uint8_t type, uint8_t q, uint8_t w, uint8_t h)
{
    p[0] = 0;
    p[1] = off >> 16;
    p[2] = off >> 8;
    p[3] = off;
    p[4] = type;
    p[5] = q;
    p[6] = w;
    p[7] = h;
}

static int rtp_jpeg_encode_standard(rtp_encoder_t *rtp_encoder, uint8_t *buf, size_t size)
{
    const uint8_t *qtables[4] = {0};
    int nb_qtables = 0;
    uint8_t type = rtp_encoder->jpeg_type;
    uint16_t width = rtp_encoder->jpeg_width;
    uint16_t height = rtp_encoder->jpeg_height;
    const uint8_t *scan = NULL;
    size_t scan_size = 0;
    if (jpeg_prepare_frame(buf, size, qtables, &nb_qtables, rtp_encoder->jpeg_info_ready,
                           &type, &width, &height, &scan, &scan_size)) return -1;
    if (!rtp_encoder->jpeg_info_ready) {
        rtp_encoder->jpeg_type = type;
        rtp_encoder->jpeg_width = width;
        rtp_encoder->jpeg_height = height;
        rtp_encoder->jpeg_info_ready = true;
    }
    int n = nb_qtables;
    uint8_t w = (width + 7) / 8;
    uint8_t h = (height + 7) / 8;
    uint8_t q = n > 0 ? 255 : 50;
    uint32_t off = 0;
    rtp_packet_t *rtp_packet = (rtp_packet_t *)rtp_encoder->buf;
    while (scan_size) {
        int hdr_size = 8;
        if (off == 0 && nb_qtables > 0) hdr_size += 4 + nb_qtables * 64;
        int max_payload = 1272 - hdr_size - rtp_encoder->ext_reserve;
        if (max_payload <= 0) return -1;
        size_t chunk = scan_size;
        if ((int)chunk > max_payload) chunk = max_payload;
        rtp_packet->header.version = 2;
        rtp_packet->header.padding = 0;
        rtp_packet->header.extension = 0;
        rtp_packet->header.csrccount = 0;
        rtp_packet->header.markerbit = chunk == scan_size;
        rtp_packet->header.type = rtp_encoder->type;
        rtp_packet->header.seq_number = __builtin_bswap16(rtp_encoder->seq_number++);
        rtp_packet->header.timestamp = __builtin_bswap32(rtp_encoder->timestamp);
        rtp_packet->header.ssrc = __builtin_bswap32(rtp_encoder->ssrc);
        uint8_t *payload = rtp_packet->payload + rtp_encoder->ext_reserve;
        jpeg_write_main_header(payload, off, type, q, w, h);
        int payload_off = 8;
        if (off == 0 && nb_qtables > 0) {
            payload[payload_off++] = 0;
            payload[payload_off++] = 0;
            uint16_t qlen = nb_qtables;
            qlen *= 64;
            payload[payload_off++] = qlen >> 8;
            payload[payload_off++] = qlen;
            for (int i = 0; i < nb_qtables; i++) {
                memcpy(payload + payload_off, qtables[i], 64);
                payload_off += 64;
            }
        }
        memcpy(payload + payload_off, scan, chunk);
        rtp_encoder->on_packet(rtp_encoder->buf, sizeof(rtp_header_t) + rtp_encoder->ext_reserve + payload_off + chunk, rtp_encoder->user_data);
        scan += chunk;
        scan_size -= chunk;
        off += chunk;
    }
    return 0;
}

int rtp_jpeg_encode(rtp_encoder_t *rtp_encoder, uint8_t *buf, size_t size)
{
    return rtp_jpeg_encode_standard(rtp_encoder, buf, size);
}

static int rtp_jpeg_parse_rtp_header(uint8_t *payload, size_t payload_size, uint32_t *off, uint8_t *type,
                                     uint8_t *q, uint8_t *w, uint8_t *h, uint16_t *dri,
                                     const uint8_t **qtables, uint16_t *qtable_len, size_t *hdr_consumed)
{
    if (payload_size < 8) return -1;
    *off = (payload[1] << 16) | (payload[2] << 8) | payload[3];
    *type = payload[4];
    *q = payload[5];
    *w = payload[6];
    *h = payload[7];
    *dri = 0;
    *qtables = NULL;
    *qtable_len = 0;
    size_t pos = 8;
    if (*type & 64) {
        if (payload_size < 12) return -1;
        *dri = (payload[8] << 8) | payload[9];
        *type &= ~64;
        pos += 4;
    }
    if (*off == 0 && *q >= 128) {
        if (payload_size < pos + 4) return -1;
        *qtable_len = (payload[pos + 2] << 8) | payload[pos + 3];
        pos += 4;
        if (*qtable_len) {
            if (payload_size < pos + *qtable_len) return -1;
            *qtables = payload + pos;
            pos += *qtable_len;
        }
    }
    *hdr_consumed = pos;
    return 0;
}

static int rtp_jpeg_decode_standard(rtp_decoder_t *rtp_decoder, uint8_t *buf, size_t size)
{
    rtp_packet_t *rtp_packet = (rtp_packet_t *)buf;
    if (size < 12) return -1;
    size_t header_size = sizeof(rtp_header_t) + rtp_packet->header.csrccount * 4;
    if (rtp_packet->header.extension) {
        if (size < header_size + 4) return -1;
        uint16_t ext_len = __builtin_bswap16(*(uint16_t *)(buf + header_size + 2));
        header_size += (ext_len + 1) * 4;
    }
    if (size < header_size) return -1;
    uint16_t seq = __builtin_bswap16(rtp_packet->header.seq_number);
    uint32_t ts = __builtin_bswap32(rtp_packet->header.timestamp);
    uint8_t *payload = buf + header_size;
    size_t payload_size = size - header_size;
    if (rtp_decoder->has_last_seq && (uint16_t)(seq - rtp_decoder->last_seq) != 1) {
        rtp_decoder->frame_size = 0;
        rtp_decoder->jpeg_hdr_size = 0;
    }
    rtp_decoder->last_seq = seq;
    rtp_decoder->has_last_seq = true;
    if (rtp_decoder->has_last_ts && rtp_decoder->last_timestamp != ts) {
        rtp_decoder->frame_size = 0;
        rtp_decoder->jpeg_hdr_size = 0;
    }
    rtp_decoder->last_timestamp = ts;
    rtp_decoder->has_last_ts = true;
    uint32_t off = 0;
    uint8_t type = 0, q = 0, w = 0, h = 0;
    uint16_t dri = 0;
    const uint8_t *qtables = NULL;
    uint16_t qtable_len = 0;
    size_t hdr_consumed = 0;
    if (rtp_jpeg_parse_rtp_header(payload, payload_size, &off, &type, &q, &w, &h, &dri,
                                 &qtables, &qtable_len, &hdr_consumed)) return -1;
    if (type > 1) return -1;
    const uint8_t *scan = payload + hdr_consumed;
    size_t scan_len = payload_size - hdr_consumed;
    if (off == 0) {
        uint8_t new_qtables[128];
        const uint8_t *use_qt;
        int nb_qtable;
        uint8_t hdr[1024];
        rtp_decoder->frame_size = 0;
        rtp_decoder->jpeg_hdr_size = 0;
        if (q >= 128) {
            if (!qtable_len || !qtables || (qtable_len % 64)) return -1;
            use_qt = qtables;
            nb_qtable = qtable_len / 64;
        } else if (q > 0 && q < 100) {
            create_default_qtables(new_qtables, q);
            use_qt = new_qtables;
            nb_qtable = 2;
        } else return -1;
        int hdr_size = jpeg_create_header(hdr, sizeof(hdr), type, w, h, use_qt, nb_qtable, dri);
        if (hdr_size < 0) return -1;
        if (rtp_decoder_append_frame(rtp_decoder, hdr, hdr_size)) {
            rtp_decoder->frame_size = 0;
            return -1;
        }
        rtp_decoder->jpeg_hdr_size = hdr_size;
    }
    if (!rtp_decoder->jpeg_hdr_size) return 0;
    if (rtp_decoder->frame_size - rtp_decoder->jpeg_hdr_size != off) {
        rtp_decoder->frame_size = 0;
        rtp_decoder->jpeg_hdr_size = 0;
        return 0;
    }
    if (rtp_decoder_append_frame(rtp_decoder, scan, scan_len)) {
        rtp_decoder->frame_size = 0;
        rtp_decoder->jpeg_hdr_size = 0;
        return -1;
    }
    if (rtp_packet->header.markerbit) {
        uint8_t eoi[2] = {0xff, 0xd9};
        if (rtp_decoder_append_frame(rtp_decoder, eoi, sizeof(eoi))) {
            rtp_decoder->frame_size = 0;
            rtp_decoder->jpeg_hdr_size = 0;
            return -1;
        }
        rtp_packet->header.seq_number = seq;
        rtp_packet->header.timestamp = ts;
        rtp_packet->header.ssrc = __builtin_bswap32(rtp_packet->header.ssrc);
        rtp_decoder->on_packet(&rtp_packet->header, rtp_decoder->frame_buf, rtp_decoder->frame_size, rtp_decoder->user_data);
        rtp_decoder->frame_size = 0;
        rtp_decoder->jpeg_hdr_size = 0;
        rtp_decoder->has_last_ts = false;
    }
    return 0;
}

int rtp_jpeg_decode(rtp_decoder_t *rtp_decoder, uint8_t *buf, size_t size)
{
    return rtp_jpeg_decode_standard(rtp_decoder, buf, size);
}

/* sctp implementation.
 * libpeer foundation: Copyright (c) 2021 sepfy.
 * See LICENSE and LICENSES/ for copyright and licensing notices.
 */
/* Upstream libpeer notice:
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
#include "utils.h"
#include "peer_platform.h"
#include "sctp.h"
#include <stdio.h>
#include <stdatomic.h>
#include "lwip/def.h"
#include <stdlib.h>
#include <string.h>

/* Association state and wire chunks are private to the SCTP implementation. */
#define SCTP_STREAM_SLOTS 10
#define SCTP_NEGOTIATED_STREAMS (SCTP_STREAM_SLOTS * 2)
#include "buf_mngr.h"

typedef enum { SCTP_DATA=0, SCTP_INIT, SCTP_INIT_ACK, SCTP_SACK, SCTP_HEARTBEAT,
    SCTP_HEARTBEAT_ACK, SCTP_ABORT, SCTP_SHUTDOWN, SCTP_SHUTDOWN_ACK, SCTP_ERROR,
    SCTP_COOKIE_ECHO, SCTP_COOKIE_ACK, SCTP_ECNE, SCTP_CWR, SCTP_SHUTDOWN_COMPLETE,
    SCTP_AUTH, SCTP_ASCONF_ACK=128, SCTP_ASCONF=130, SCTP_FORWARD_TSN=192 } sctp_chunk_type_t;
typedef struct { uint16_t type, length; uint8_t value[0]; } sctp_chunk_param_t;
typedef struct { uint8_t type, flags; uint16_t length; } sctp_chunk_t;
typedef struct { uint16_t param_type, param_len; } sctp_param_t;
typedef struct {
    sctp_chunk_t common;
    sctp_param_t param;
    uint32_t req_seq, resp_seq, last_tsn;
    uint16_t stream_id;
} sctp_recfg_chunk_t;
typedef struct {
    sctp_chunk_t common;
    sctp_param_t param;
    uint32_t req_seq, result;
} sctp_recfg_ack_chunk_t;
typedef struct { uint16_t src_port, dst_port; uint32_t verification_tag, checksum; } sctp_header_t;
typedef struct { sctp_header_t header; sctp_chunk_t chunks[0]; } sctp_packet_t;
typedef struct sctp_sack_chunk_t {
    sctp_chunk_t common;
    uint32_t cumulative_tsn_ack, wnd_size;
    uint16_t ack_block_num, dup_tsn_num;
    uint16_t blocks[0];
} sctp_sack_chunk_t;
typedef struct { sctp_chunk_t common; uint32_t cumulative_tsn; uint16_t stream_info[0]; } sctp_forward_tsn_chunk_t;
typedef struct {
    uint8_t type, payload_type;
    uint16_t length;
    uint32_t tsn;
    uint16_t si, seq_num;
    uint32_t ppid;
    uint8_t data[0];
} sctp_data_chunk_t;
typedef struct {
    sctp_chunk_t common;
    uint32_t initiate_tag, wnd_size;
    uint16_t out_stream_num, in_stream_num;
    uint32_t initial_tsn;
    sctp_chunk_param_t param[0];
} sctp_init_chunk_t;
typedef struct {
    uint8_t msg_type, channel_type;
    uint16_t priority;
    uint32_t reliable;
    uint16_t label_len, protocol_len;
    char label[0];
} sctp_dcep_data_t;
typedef enum { SCTP_STATE_NONE, SCTP_STATE_INIT_SENT, SCTP_STATE_INIT_ACKED,
    SCTP_STATE_COOKIE_ECHO_SENT, SCTP_STATE_COOKIE_ECHO_ACKED, SCTP_STATE_CONNECTED,
    SCTP_STATE_DISCONNECTED } sctp_state_t;
typedef struct sctp_frame_t {
    struct sctp_frame_t *next;
    uint32_t seq_num, min_tsn, max_tsn;
    uint16_t fill_num, ppid;
    uint8_t frame_flag, stream_idx;
} sctp_frame_t;
typedef struct {
    char *label;
    uint16_t stream_id;
    sctp_frame_t *frames, *tail;
    uint32_t start_seq_num, end_seq_num, send_packet_num, recfg_seq;
    uint8_t stream_idx;
    esp_peer_data_channel_reliable_type_t type;
    uint16_t unreliable_limit;
    uint8_t used:1, opened:1, local_closed:1, peer_closed:1, close_notify:1,
        reliable:1, un_ordered:1, frame_sent:1;
    uint32_t last_sent_seq;
    uint8_t *direct_data;
    uint32_t direct_size, direct_capacity;
    uint16_t direct_ppid;
} sctp_stream_t;
typedef struct {
    sctp_cfg_t cfg;
    int local_port, remote_port;
    uint32_t verification_tag, peer_tag;
    uint16_t initial_stream_id;
    uint32_t recfg_seq;
    uint16_t peer_stream_id;
    uint32_t cmd_send_time;
    uint8_t cmd_resend_count;
    sctp_state_t state;
    uint32_t send_tsn, send_ack_tsn;
    uint8_t buf[1200];
    buf_mngr_t *send_buf_mngr;
    uint8_t *frame_data;
    int max_frame_size;
    sctp_frame_t *free_frames;
    /* Keep the established offsets of the ACK, cache and lock fields used by
     * the application adapters. Active stream storage follows those fields. */
    uint8_t reserved_stream_slots[128];
    uint8_t stream_num;
    uint32_t acked_tsn, latest_recv_tsn;
    uint16_t received_packets;
    uint8_t last_sack_count;
    uint32_t sack_send_time;
    buf_mngr_t *recv_buf_mngr;
    uint32_t last_forward_tsn;
    buf_abandon_info_t forward_info;
    bool support_forward_tsn, need_forward, need_abandon;
    atomic_int ref_count;
    void *lock, *send_lock;
    sctp_stream_t streams[SCTP_STREAM_SLOTS];
    uint32_t reset_request, reset_result;
    uint32_t sent_streams, sent_sequence, sent_last_tsn;
    uint32_t resend_at_ms, resend_timeout_ms;
    uint32_t queued_streams, queued_last_tsn;
} sctp_t;

#define LOCAL static

extern void peer_atomic_inc(atomic_int *);
extern int peer_atomic_dec(atomic_int *);
extern int peer_atomic_load(atomic_int *);

#define LOGE(format, ...) ESP_LOGE("SCTP", format, ##__VA_ARGS__)
#define LOGW(format, ...) ESP_LOGW("SCTP", format, ##__VA_ARGS__)
#define LOGI(format, ...) ESP_LOGI("SCTP", format, ##__VA_ARGS__)

bool sctp_tracing;
static bool query_details;

_Static_assert(sizeof(sctp_frame_t) == 24, "frame ABI");
_Static_assert(sizeof(sctp_stream_t) == 64, "stream ABI");
_Static_assert(offsetof(sctp_stream_t, stream_idx) == 32, "stream index ABI");
_Static_assert(offsetof(sctp_stream_t, type) == 36, "stream reliability ABI");
_Static_assert(offsetof(sctp_stream_t, direct_data) == 48, "direct data ABI");
_Static_assert(sizeof(sctp_t) == 2184, "association ABI");
_Static_assert(offsetof(sctp_t, recfg_seq) == 72, "reset sequence ABI");
_Static_assert(offsetof(sctp_t, send_tsn) == 92, "send TSN ABI");
_Static_assert(offsetof(sctp_t, send_ack_tsn) == 96, "send acknowledgement ABI");
_Static_assert(offsetof(sctp_t, buf) == 100, "packet buffer ABI");
_Static_assert(offsetof(sctp_t, free_frames) == 1312, "free frame ABI");
_Static_assert(offsetof(sctp_t, stream_num) == 1444, "stream count ABI");
_Static_assert(offsetof(sctp_t, acked_tsn) == 1448, "receive acknowledgement ABI");
_Static_assert(offsetof(sctp_t, recv_buf_mngr) == 1464, "receive cache ABI");
_Static_assert(offsetof(sctp_t, ref_count) == 1496, "reference count ABI");
_Static_assert(offsetof(sctp_t, lock) == 1500, "association lock ABI");
_Static_assert(offsetof(sctp_t, send_lock) == 1504, "send lock ABI");
_Static_assert(offsetof(sctp_t, streams) == 1508, "stream array ABI");
_Static_assert(offsetof(sctp_t, reset_request) == 2148, "reset state ABI");
_Static_assert(offsetof(sctp_t, queued_last_tsn) == 2180, "queued reset ABI");

/* Supplied by the application's SCTP adapters. The layouts above are their ABI. */
extern sctp_frame_t *jetkvm_sctp_get_frame(sctp_t *, sctp_stream_t *, const void *);
extern void jetkvm_sctp_handle_forward_tsn(sctp_t *, const void *);
extern int jetkvm_sctp_handle_recfg(sctp_t *, const void *);
extern int jetkvm_sctp_send_recfg(sctp_t *, const char *);
extern int jetkvm_sctp_check_sack(sctp_t *);
extern sctp_stream_t *jetkvm_sctp_get_stream(sctp_t *, uint16_t);
extern void jetkvm_sctp_send_dcep_ack(sctp_t *, uint16_t);
extern int jetkvm_sctp_receive_data(sctp_t *, const void *, int);
extern uint32_t jetkvm_sctp_receive_window(buf_mngr_t *);
int jetkvm_vendor_check_sack(sctp_t * sctp);
void sctp_handle_dcep(sctp_t * sctp, sctp_data_chunk_t * chunk, uint16_t chunk_size);
LOCAL int sctp_direct_data_hdlr(sctp_t * sctp, sctp_data_chunk_t * data_chunk, bool add_only);
LOCAL int sctp_direct_append(sctp_stream_t * stream, uint8_t * data, int size);
LOCAL void sctp_data_channel_opened(sctp_t * sctp, uint16_t stream_id);
LOCAL void handle_sack(sctp_t * sctp, sctp_sack_chunk_t * sack, uint16_t chunk_size);
LOCAL bool do_forward_tsn(sctp_t * sctp);
LOCAL void do_sack(sctp_t * sctp);
LOCAL void remove_acked_packets(sctp_t * sctp, uint32_t acked_tsn);
int jetkvm_vendor_stream_closed(sctp_t * sctp, sctp_stream_t * stream);
LOCAL void release_stream(sctp_t * sctp, sctp_stream_t * stream);
LOCAL void sctp_do_connect(sctp_t * sctp);
LOCAL void sctp_set_state(sctp_t * sctp, sctp_state_t state);
LOCAL void sctp_dec_ref(sctp_t * sctp);
LOCAL void sctp_add_ref(sctp_t * sctp);
int jetkvm_vendor_packet_hdlr(sctp_t * sctp, sctp_data_chunk_t * data_chunk, int add_only);
void update_receive_ack_tsn(sctp_t * sctp, bool check_send, int line);
LOCAL int remove_for_full(sctp_t * sctp, sctp_stream_t * stream);
LOCAL int check_send_one_frame(sctp_t * sctp, sctp_stream_t * in_stream, sctp_frame_t * in_frame, uint32_t tsn);
sctp_frame_t * check_send_all_stream(sctp_t * sctp, bool send);
LOCAL void send_all_frames(sctp_t * sctp, sctp_frame_t * frame);
LOCAL bool try_send_one_frame(sctp_t * sctp, sctp_stream_t * stream, sctp_frame_t * frame);
sctp_frame_t * jetkvm_vendor_get_frame(sctp_t * sctp, sctp_stream_t * stream, sctp_data_chunk_t * data_chunk);
LOCAL sctp_frame_t * get_unordered_frames(sctp_t * sctp, sctp_stream_t * stream, sctp_data_chunk_t * data_chunk);
LOCAL sctp_frame_t * insert_between(sctp_t * sctp, sctp_stream_t * stream, sctp_frame_t * frame, sctp_data_chunk_t * data_chunk);
LOCAL sctp_frame_t * merge_next(sctp_t * sctp, sctp_frame_t * frame);
LOCAL sctp_frame_t * insert_next(sctp_frame_t * frame, sctp_data_chunk_t * data_chunk);
LOCAL sctp_frame_t * insert_pre(sctp_frame_t * frame, sctp_data_chunk_t * data_chunk);
LOCAL sctp_frame_t * insert_new(sctp_t * sctp, sctp_stream_t * stream, sctp_frame_t * frame, sctp_data_chunk_t * data_chunk);
LOCAL sctp_frame_t * split_frame(sctp_t * sctp, sctp_stream_t * stream, sctp_frame_t * frame, sctp_data_chunk_t * data_chunk);
LOCAL uint8_t * get_cookie_data(const uint8_t * buf, int * size);
int jetkvm_vendor_send_recfg(sctp_t * sctp, const char * label);
void jetkvm_vendor_send_dcep_ack(sctp_t * sctp, uint16_t stream_id);
LOCAL int sctp_send_dcep(sctp_t * sctp, esp_peer_data_channel_cfg_t * cfg);
LOCAL int sctp_client_send_echo_ack(sctp_t * sctp);
LOCAL int sctp_client_send_echo_chunk(sctp_t * sctp, uint8_t * cookie, int cookie_size);
LOCAL int sctp_client_send_init_chunk(sctp_t * sctp);
LOCAL int sctp_client_send_init_ack_chunk(sctp_t * sctp);
LOCAL void resend_not_sacked(sctp_t * sctp);
LOCAL int resend_packet(sctp_t * sctp, uint32_t start_time, buf_info_t * buf_info, char * reason);
LOCAL int sctp_outgoing_reliable(sctp_t * sctp, sctp_stream_t * stream, sctp_data_chunk_t * chunk, uint8_t * payload, int size);
LOCAL int sctp_outgoing_hdlr(sctp_t * sctp, sctp_stream_t * stream, sctp_data_chunk_t * chunk, uint8_t * payload, int size);
LOCAL int sctp_outgoing_hdlr_ex(sctp_t * sctp, sctp_stream_t * stream, sctp_data_chunk_t * chunk, uint8_t * payload, int size, bool force_reliable);
LOCAL void sctp_update_forward(sctp_t * sctp);
LOCAL bool sctp_verify_packet(sctp_t * sctp, const uint8_t * buf, int len);
LOCAL void sctp_check_need_forward(sctp_t * sctp);
int jetkvm_vendor_fill_header_and_write(sctp_t * sctp, uint8_t * buf, int len);
LOCAL char * chunk_type_to_name(uint8_t type);
LOCAL void parse_data_chunk(sctp_data_chunk_t * data_chunk);
LOCAL void reset_stream_direct_data(sctp_stream_t * stream);
LOCAL void free_frames(sctp_frame_t * head);
LOCAL void return_frame(sctp_t * sctp, sctp_frame_t * frame);
LOCAL void remove_frame(sctp_t * sctp, sctp_stream_t * stream, sctp_frame_t * frame);
LOCAL sctp_frame_t * alloc_frame(sctp_t * sctp, sctp_stream_t * stream, sctp_data_chunk_t * data_chunk);
LOCAL uint16_t get_default_stream_id(sctp_t * sctp);
LOCAL sctp_stream_t * get_stream_by_label(sctp_t * sctp, const char * label);
LOCAL sctp_stream_t * get_stream_by_stream_id(sctp_t * sctp, uint16_t stream_id);

LOCAL sctp_stream_t *get_stream_by_stream_id(sctp_t *sctp, uint16_t stream_id)
{
    for (int i = 0; i < SCTP_STREAM_SLOTS; i++) {
        sctp_stream_t *stream = &sctp->streams[i];
        if (stream->used && stream->stream_id == stream_id) return stream;
    }
    return NULL;
}

LOCAL uint16_t get_default_stream_id(sctp_t *sctp)
{
    uint32_t stream_id = 0;
    int stream_num = 0;
    for (int i = 0; i < SCTP_STREAM_SLOTS; i++) {
        if (sctp->streams[i].used) {
            stream_id = sctp->streams[i].stream_id;
            stream_num++;
        }
    }
    return stream_num == 1 ? stream_id : 0;
}

LOCAL void remove_frame(sctp_t *sctp, sctp_stream_t *stream, sctp_frame_t *frame)
{
    sctp_frame_t *head = stream->frames;
    if (head == frame) {
        stream->frames = frame->next;
        if (stream->frames) stream->start_seq_num = stream->frames->seq_num;
        return;
    }
    while (head) {
        if (head->next == frame) {
            if (stream->tail == frame) stream->tail = head;
            head->next = frame->next;
            return;
        }
        head = head->next;
    }
}

LOCAL void return_frame(sctp_t *sctp, sctp_frame_t *frame)
{
    frame->next = sctp->free_frames;
    sctp->free_frames = frame;
}

LOCAL sctp_frame_t *insert_pre(sctp_frame_t *frame, sctp_data_chunk_t *data_chunk)
{
    frame->max_tsn = data_chunk->tsn;
    frame->frame_flag |= data_chunk->payload_type & 1;
    return frame;
}

LOCAL sctp_frame_t *insert_next(sctp_frame_t *frame, sctp_data_chunk_t *data_chunk)
{
    frame->min_tsn = data_chunk->tsn;
    frame->frame_flag |= data_chunk->payload_type & 2;
    return frame;
}

LOCAL sctp_frame_t *merge_next(sctp_t *sctp, sctp_frame_t *frame)
{
    sctp_frame_t *next = frame->next;
    sctp_frame_t *next_next = next->next;
    frame->max_tsn = next->max_tsn;
    frame->frame_flag |= next->frame_flag;
    frame->next = next_next;
    return_frame(sctp, next);
    return frame;
}

LOCAL void sctp_data_channel_opened(sctp_t *sctp, uint16_t stream_id)
{
    sctp_stream_t *stream = get_stream_by_stream_id(sctp, stream_id);
    if (stream == NULL || stream->opened) return;
    if (sctp->cfg.on_open) {
        esp_peer_data_channel_info_t info = {stream->label, stream_id};
        sctp->cfg.on_open(&info, sctp->cfg.ctx);
    }
    stream->opened = true;
}

LOCAL uint8_t *get_cookie_data(const uint8_t *buf, int *size)
{
    sctp_init_chunk_t *init_chunk = (sctp_init_chunk_t *)(buf + sizeof(sctp_header_t));
    uint16_t chunk_size = ntohs(init_chunk->common.length);
    if (chunk_size <= sizeof(sctp_init_chunk_t)) return NULL;
    int left_size = chunk_size - sizeof(sctp_init_chunk_t);
    uint8_t *param_data = (uint8_t *)init_chunk->param;
    while (left_size > 0) {
        sctp_chunk_param_t *param = (sctp_chunk_param_t *)param_data;
        uint16_t param_size = ntohs(param->length);
        param_size = (param_size + 3) & ~3;
        if (param->type == htons(7)) {
            *size = param_size - sizeof(sctp_chunk_param_t);
            return param->value;
        }
        left_size -= param_size;
        param_data += param_size;
    }
    return NULL;
}

LOCAL void parse_data_chunk(sctp_data_chunk_t *data_chunk)
{
    data_chunk->ppid = ntohl(data_chunk->ppid);
    data_chunk->tsn = ntohl(data_chunk->tsn);
    data_chunk->si = ntohs(data_chunk->si);
    data_chunk->seq_num = ntohs(data_chunk->seq_num);
    data_chunk->length = ntohs(data_chunk->length);
}

LOCAL void sctp_add_ref(sctp_t *sctp) { peer_atomic_inc(&sctp->ref_count); }
LOCAL void sctp_dec_ref(sctp_t *sctp) { peer_atomic_dec(&sctp->ref_count); }

int jetkvm_vendor_fill_header_and_write(sctp_t *sctp, uint8_t *buf, int len)
{
    sctp_header_t *header = (sctp_header_t *)buf;
    header->src_port = sctp->local_port;
    header->dst_port = sctp->remote_port;
    header->verification_tag = sctp->peer_tag;
    len = (len + 3) & ~3;
    header->checksum = 0;
    header->checksum = utils_crc32c_be(-1, buf, len);
    return sctp->cfg.write_packet(sctp->cfg.ctx, sctp->buf, len);
}

LOCAL void reset_stream_direct_data(sctp_stream_t *stream)
{
    if (stream->direct_data) {
        media_lib_free(stream->direct_data);
        stream->direct_data = NULL;
    }
    stream->direct_size = 0;
    stream->direct_capacity = 0;
    stream->direct_ppid = 0;
}

LOCAL void free_frames(sctp_frame_t *head)
{
    while (head) {
        sctp_frame_t *next = head->next;
        media_lib_free(head);
        head = next;
    }
}

LOCAL int sctp_direct_append(sctp_stream_t *stream, uint8_t *data, int size)
{
    if (size <= 0) return 0;
    uint32_t need_size = stream->direct_size + size;
    if (need_size > stream->direct_capacity) {
        uint8_t *new_data = media_lib_realloc(stream->direct_data, need_size);
        if (new_data == NULL) return -2;
        stream->direct_data = new_data;
        stream->direct_capacity = need_size;
    }
    memcpy(stream->direct_data + stream->direct_size, data, size);
    stream->direct_size += size;
    return 0;
}

int sctp_is_connected(sctp_handle_t h)
{
    if (h == NULL) return 0;
    sctp_t *sctp = h;
    return sctp->state == SCTP_STATE_CONNECTED;
}

void sctp_show_details(bool enable) { query_details = enable; }
bool sctp_details_enabled(void) { return query_details; }

LOCAL sctp_stream_t *get_stream_by_label(sctp_t *sctp, const char *label)
{
    for (int i = 0; i < SCTP_STREAM_SLOTS; i++) {
        sctp_stream_t *stream = &sctp->streams[i];
        if (stream->used && stream->label && strcmp(stream->label, label) == 0) return stream;
    }
    return NULL;
}

LOCAL sctp_frame_t *alloc_frame(sctp_t *sctp, sctp_stream_t *stream, sctp_data_chunk_t *data_chunk)
{
    if (sctp->free_frames == NULL) sctp->free_frames = media_lib_calloc(1, sizeof(sctp_frame_t));
    sctp_frame_t *frame = sctp->free_frames;
    if (frame == NULL) {
        LOGE("no memory for frame");
        return NULL;
    }
    frame->ppid = data_chunk->ppid;
    frame->seq_num = data_chunk->seq_num;
    frame->fill_num = 0;
    frame->frame_flag = data_chunk->payload_type;
    frame->stream_idx = stream->stream_idx;
    frame->min_tsn = frame->max_tsn = data_chunk->tsn;
    sctp->free_frames = frame->next;
    return frame;
}

LOCAL sctp_frame_t *insert_new(sctp_t *sctp, sctp_stream_t *stream, sctp_frame_t *frame, sctp_data_chunk_t *data_chunk)
{
    sctp_frame_t *new_frame = alloc_frame(sctp, stream, data_chunk);
    if (new_frame) {
        sctp_frame_t *next = frame->next;
        frame->next = new_frame;
        new_frame->next = next;
    }
    return new_frame;
}

/* SCTP calculates CRC with a zero checksum field, then restores the wire value.
 */
static inline uint32_t save_clear_crc(sctp_packet_t *packet)
{
    uint32_t value = packet->header.checksum;
    packet->header.checksum = 0;
    return value;
}

LOCAL bool sctp_verify_packet(sctp_t *sctp, const uint8_t *buf, int len)
{
    sctp_packet_t *packet = (sctp_packet_t *)buf;
    uint32_t src_crc = save_clear_crc(packet);
    uint32_t crc32c = utils_crc32c_be(-1, buf, len);
    packet->header.checksum = src_crc;
    if (crc32c != packet->header.checksum) {
        LOGE("CRC error 0x%x != 0x%x", crc32c, src_crc);
        return false;
    }
    if (sctp->state == SCTP_STATE_CONNECTED && packet->header.verification_tag != sctp->verification_tag) {
        LOGE("TAG error");
        return false;
    }
    return true;
}

LOCAL void sctp_check_need_forward(sctp_t *sctp)
{
    if (sctp->cfg.reliable_transport || sctp->recv_buf_mngr == NULL || sctp->need_forward) return;
    for (uint8_t i = 0; i < SCTP_STREAM_SLOTS; i++) {
        sctp_stream_t *stream = &sctp->streams[i];
        if (!stream->used) continue;
        if (!stream->reliable) {
            buf_mngr_set_forward_mode(sctp->recv_buf_mngr, sctp->acked_tsn);
            sctp->need_forward = true;
            break;
        }
    }
}

int sctp_create_channel(sctp_handle_t sctp, esp_peer_data_channel_cfg_t *cfg)
{
    if (sctp == NULL) return -1;
    return sctp_send_dcep(sctp, cfg);
}
int sctp_close_channel(sctp_handle_t sctp, const char *label)
{
    if (sctp == NULL) return -1;
    return jetkvm_sctp_send_recfg(sctp, label);
}
void sctp_do_sack(sctp_handle_t h)
{
    sctp_t *sctp = h;
    if (sctp) do_sack(sctp);
}

int jetkvm_vendor_check_sack(sctp_t *sctp)
{
    if (sctp->cfg.reliable_transport) return 0;
    uint32_t cur_time = esp_timer_get_time() / 1000;
    bool is_sack_timeout = false;
    if (cur_time < sctp->sack_send_time) {
        sctp->sack_send_time = cur_time;
        is_sack_timeout = true;
    } else if (cur_time > sctp->sack_send_time) {
        sctp->sack_send_time = cur_time;
        is_sack_timeout = true;
    }
    if (!is_sack_timeout) return 0;
    if (sctp->recv_buf_mngr) buf_mngr_check_overdue(sctp->recv_buf_mngr);
    do_sack(sctp);
    if (sctp->need_forward) do_forward_tsn(sctp);
    resend_not_sacked(sctp);
    return 0;
}

void sctp_main_loop(sctp_handle_t h)
{
    sctp_t *sctp = h;
    if (sctp == NULL) return;
    if (sctp->state != SCTP_STATE_CONNECTED) {
        sctp_do_connect(sctp);
        return;
    }
    if (sctp->send_buf_mngr) {
        if (!sctp->need_forward) buf_mngr_check_overdue(sctp->send_buf_mngr);
        else sctp_update_forward(sctp);
    }
    if (sctp->recv_buf_mngr) buf_mngr_check_overdue(sctp->recv_buf_mngr);
    jetkvm_sctp_check_sack(sctp);
}

#define TSN_GT(a,b) (((a) > (b) && (a) < (b) + UINT32_C(0x80000000)) || ((b) - (a) > UINT32_C(0x80000000)))
#define TSN_LT(a,b) (((a) < (b) && (b) < (a) + UINT32_C(0x80000000)) || ((a) - (b) > UINT32_C(0x80000000)))
LOCAL bool do_forward_tsn(sctp_t *sctp)
{
    if (!TSN_GT(sctp->last_forward_tsn, sctp->send_ack_tsn)) {
        sctp->last_forward_tsn = sctp->send_ack_tsn;
        return false;
    }
    media_lib_mutex_lock(sctp->send_lock, -1);
    sctp_packet_t *out_packet = (sctp_packet_t *)sctp->buf;
    sctp_forward_tsn_chunk_t *forward_chunk = (sctp_forward_tsn_chunk_t *)out_packet->chunks;
    forward_chunk->common.type = SCTP_FORWARD_TSN;
    forward_chunk->common.flags = 0;
    forward_chunk->cumulative_tsn = htonl(sctp->last_forward_tsn);
    int len = sizeof(sctp_forward_tsn_chunk_t) + 4 * sctp->forward_info.abandon_stream_num;
    uint16_t *v = forward_chunk->stream_info;
    for (int i = 0; i < sctp->forward_info.abandon_stream_num; i++) {
        *v++ = htons(sctp->forward_info.abandon_stream_id[i]);
        *v++ = htons(sctp->forward_info.abandon_packet_id[i]);
    }
    forward_chunk->common.length = htons(len);
    jetkvm_vendor_fill_header_and_write(sctp, sctp->buf, len + sizeof(sctp_header_t));
    media_lib_mutex_unlock(sctp->send_lock);
    return true;
}

LOCAL int sctp_outgoing_hdlr_ex(sctp_t *sctp, sctp_stream_t *stream, sctp_data_chunk_t *chunk,
    uint8_t *payload, int size, bool force_reliable)
{
    chunk->length = htons(size + sizeof(sctp_data_chunk_t));
    int write_len = (size + 3) & ~3;
    chunk->tsn = htonl(sctp->send_tsn);
    if (chunk->data != payload) memcpy(chunk->data, payload, size);
    if (size < write_len) memset(chunk->data + size, 0, write_len - size);
    write_len += sizeof(sctp_data_chunk_t) + sizeof(sctp_header_t);
    sctp_packet_t *packet = (sctp_packet_t *)sctp->buf;
    packet->header.checksum = 0;
    packet->header.checksum = utils_crc32c_be(-1, sctp->buf, write_len);
    bool reliable = force_reliable || stream->reliable;
    bool need_abandon = !force_reliable && stream->type != ESP_PEER_DATA_CHANNEL_RELIABLE && stream->unreliable_limit == 0;
    sctp->need_abandon |= need_abandon;
    int buf_size = need_abandon ? 0 : write_len;
    if (!sctp->cfg.reliable_transport) {
        if (buf_mngr_add_reliable(sctp->send_buf_mngr, reliable, chunk->payload_type & 1,
            sctp->send_tsn, stream->stream_id, stream->send_packet_num, sctp->buf, buf_size)) return -2;
    }
    sctp->cfg.write_packet(sctp->cfg.ctx, sctp->buf, write_len);
    sctp->send_tsn++;
    return 0;
}
LOCAL int sctp_outgoing_hdlr(sctp_t *sctp, sctp_stream_t *stream, sctp_data_chunk_t *chunk, uint8_t *payload, int size)
{
    return sctp_outgoing_hdlr_ex(sctp, stream, chunk, payload, size, false);
}
LOCAL int sctp_outgoing_reliable(sctp_t *sctp, sctp_stream_t *stream, sctp_data_chunk_t *chunk, uint8_t *payload, int size)
{
    return sctp_outgoing_hdlr_ex(sctp, stream, chunk, payload, size, true);
}

LOCAL char *chunk_type_to_name(uint8_t type)
{
    switch (type) {
    case SCTP_DATA: return "SCTP_DATA";
    case SCTP_INIT: return "SCTP_INIT";
    case SCTP_INIT_ACK: return "SCTP_INIT_ACK";
    case SCTP_SACK: return "SCTP_SACK";
    case SCTP_HEARTBEAT: return "SCTP_HEARTBEAT";
    case SCTP_HEARTBEAT_ACK: return "SCTP_HEARTBEAT_ACK";
    case SCTP_ABORT: return "SCTP_ABORT";
    case SCTP_SHUTDOWN: return "SCTP_SHUTDOWN";
    case SCTP_SHUTDOWN_ACK: return "SCTP_SHUTDOWN_ACK";
    case SCTP_ERROR: return "SCTP_ERROR";
    case SCTP_COOKIE_ACK: return "SCTP_COOKIE_ACK";
    case SCTP_COOKIE_ECHO: return "SCTP_COOKIE_ECHO";
    default: return "";
    }
}

void jetkvm_vendor_send_dcep_ack(sctp_t *sctp, uint16_t stream_id)
{
    sctp_stream_t *stream = jetkvm_sctp_get_stream(sctp, stream_id);
    if (stream == NULL) {
        LOGE("Not enough stream for receive channel");
        return;
    }
    media_lib_mutex_lock(sctp->send_lock, -1);
    sctp_packet_t *packet = (sctp_packet_t *)sctp->buf;
    sctp_data_chunk_t *chunk = (sctp_data_chunk_t *)packet->chunks;
    packet->header.src_port = sctp->local_port;
    packet->header.dst_port = sctp->remote_port;
    packet->header.verification_tag = sctp->peer_tag;
    chunk->type = SCTP_DATA;
    chunk->payload_type = 3;
    chunk->si = htons(stream_id);
    chunk->seq_num = htons(stream->send_packet_num);
    chunk->ppid = htonl(PPID_CONTROL);
    sctp_dcep_data_t *dcep = (sctp_dcep_data_t *)chunk->data;
    dcep->msg_type = 2;
    dcep->reliable = 0;
    int dcep_size = 1;
    sctp_outgoing_reliable(sctp, stream, chunk, (uint8_t *)dcep, dcep_size);
    stream->send_packet_num++;
    media_lib_mutex_unlock(sctp->send_lock);
}

LOCAL int sctp_client_send_echo_chunk(sctp_t *sctp, uint8_t *cookie, int cookie_size)
{
    uint8_t *buf = sctp->buf;
    sctp_init_chunk_t *init_ack = (sctp_init_chunk_t *)(buf + sizeof(sctp_header_t));
    if (cookie == NULL && init_ack->common.type != SCTP_COOKIE_ECHO) {
        LOGE("Last is not init ack");
        return -1;
    }
    media_lib_mutex_lock(sctp->send_lock, -1);
    int size;
    if (cookie) {
        memcpy((uint8_t *)init_ack + sizeof(sctp_chunk_t), cookie, cookie_size);
        init_ack->common.type = SCTP_COOKIE_ECHO;
        init_ack->common.length = htons(cookie_size + sizeof(sctp_chunk_t));
        size = sizeof(sctp_header_t) + sizeof(sctp_chunk_t) + ((cookie_size + 3) & ~3);
    } else {
        size = sizeof(sctp_header_t) + ntohs(init_ack->common.length);
    }
    LOGI("Send ECHO chunk");
    int ret = jetkvm_vendor_fill_header_and_write(sctp, sctp->buf, size);
    media_lib_mutex_unlock(sctp->send_lock);
    return ret;
}

LOCAL int sctp_client_send_init_ack_chunk(sctp_t *sctp)
{
    media_lib_mutex_lock(sctp->send_lock, -1);
    uint8_t *buf = sctp->buf;
    sctp_init_chunk_t *init_chunk = (sctp_init_chunk_t *)(buf + sizeof(sctp_header_t));
    init_chunk->common.type = SCTP_INIT_ACK;
    init_chunk->common.flags = 0;
    init_chunk->initiate_tag = sctp->verification_tag;
    init_chunk->wnd_size = htonl(sctp->cfg.send_cache_size);
    init_chunk->out_stream_num = htons(SCTP_NEGOTIATED_STREAMS);
    init_chunk->in_stream_num = htons(SCTP_NEGOTIATED_STREAMS);
    init_chunk->initial_tsn = htonl(sctp->send_tsn);
    int size = sizeof(sctp_header_t) + sizeof(sctp_init_chunk_t);
    sctp_chunk_param_t *param = (sctp_chunk_param_t *)(buf + size);
    int param_size = sizeof(sctp_chunk_param_t) + sizeof(uint32_t);
    param->type = htons(7);
    param->length = htons(param_size);
    uint32_t value = htonl(2);
    memcpy(param->value, &value, sizeof(value));
    size += param_size;
    sctp_chunk_param_t *ext_param = (sctp_chunk_param_t *)(buf + size);
    ext_param->type = htons(0x8008);
    uint8_t ext_num = 2;
    ext_param->value[0] = SCTP_FORWARD_TSN;
    ext_param->value[1] = SCTP_ASCONF;
    int ext_size = sizeof(sctp_chunk_param_t) + ext_num;
    ext_param->length = htons(ext_size);
    size += (ext_size + 3) & ~3;
    init_chunk->common.length = htons(size - sizeof(sctp_header_t));
    LOGI("Send INIT_ACK chunk");
    int ret = jetkvm_vendor_fill_header_and_write(sctp, sctp->buf, size);
    media_lib_mutex_unlock(sctp->send_lock);
    return ret;
}

LOCAL int sctp_client_send_echo_ack(sctp_t *sctp)
{
    media_lib_mutex_lock(sctp->send_lock, -1);
    uint8_t *buf = sctp->buf;
    sctp_chunk_t *common = (sctp_chunk_t *)(buf + sizeof(sctp_header_t));
    common->type = SCTP_COOKIE_ACK;
    common->length = htons(sizeof(sctp_chunk_t));
    LOGI("Send ECHO_ACK chunk");
    int size = sizeof(sctp_header_t) + sizeof(sctp_chunk_t);
    int ret = jetkvm_vendor_fill_header_and_write(sctp, sctp->buf, size);
    media_lib_mutex_unlock(sctp->send_lock);
    return ret;
}

LOCAL int sctp_client_send_init_chunk(sctp_t *sctp)
{
    media_lib_mutex_lock(sctp->send_lock, -1);
    uint8_t *buf = sctp->buf;
    sctp_init_chunk_t *init_chunk = (sctp_init_chunk_t *)(buf + sizeof(sctp_header_t));
    init_chunk->common.type = SCTP_INIT;
    init_chunk->common.flags = 0;
    init_chunk->common.length = htons(sizeof(sctp_init_chunk_t));
    init_chunk->initiate_tag = htonl(0x12345678);
    init_chunk->wnd_size = htonl(sctp->cfg.recv_cache_size);
    init_chunk->out_stream_num = htons(SCTP_NEGOTIATED_STREAMS);
    init_chunk->in_stream_num = htons(SCTP_NEGOTIATED_STREAMS);
    init_chunk->initial_tsn = htonl(sctp->send_tsn);
    int size = sizeof(sctp_header_t) + sizeof(sctp_init_chunk_t);
    sctp_chunk_param_t *ext_param = (sctp_chunk_param_t *)(buf + size);
    ext_param->type = htons(0x8008);
    uint8_t ext_num = 2;
    ext_param->value[0] = SCTP_FORWARD_TSN;
    ext_param->value[1] = SCTP_ASCONF;
    int ext_size = sizeof(sctp_chunk_param_t) + ext_num;
    ext_param->length = htons(ext_size);
    init_chunk->common.length = htons(sizeof(sctp_init_chunk_t) + ext_size);
    size += (ext_size + 3) & ~3;
    LOGI("Send INIT chunk");
    int ret = jetkvm_vendor_fill_header_and_write(sctp, sctp->buf, size);
    media_lib_mutex_unlock(sctp->send_lock);
    return ret;
}

LOCAL void release_stream(sctp_t *sctp, sctp_stream_t *stream)
{
    media_lib_mutex_lock(sctp->lock, -1);
    if (sctp->recv_buf_mngr) buf_mngr_remove_by_stream(sctp->recv_buf_mngr, stream->stream_id);
    if (sctp->send_buf_mngr) buf_mngr_remove_by_stream(sctp->send_buf_mngr, stream->stream_id);
    sctp_frame_t *frame = stream->frames;
    if (frame) {
        while (frame) {
            if (sctp->recv_buf_mngr) buf_mngr_remove_range_by_stream(sctp->recv_buf_mngr, frame->min_tsn, frame->max_tsn);
            frame = frame->next;
        }
        if (sctp->free_frames) {
            frame = sctp->free_frames;
            while (frame->next) frame = frame->next;
            frame->next = stream->frames;
        } else sctp->free_frames = stream->frames;
    }
    if (stream->label) {
        media_lib_free(stream->label);
        stream->label = NULL;
    }
    reset_stream_direct_data(stream);
    stream->used = false;
    stream->frames = NULL;
    stream->tail = NULL;
    stream->end_seq_num = 0;
    stream->start_seq_num = 0;
    stream->peer_closed = false;
    stream->local_closed = false;
    stream->opened = false;
    stream->close_notify = false;
    media_lib_mutex_unlock(sctp->lock);
}

int jetkvm_vendor_stream_closed(sctp_t *sctp, sctp_stream_t *stream)
{
    if (!stream->close_notify) {
        stream->close_notify = true;
        if (sctp->cfg.on_close) {
            esp_peer_data_channel_info_t info = {stream->label, stream->stream_id};
            sctp->cfg.on_close(&info, sctp->cfg.ctx);
        }
    }
    if (stream->local_closed && stream->peer_closed) release_stream(sctp, stream);
    return 0;
}

LOCAL void sctp_set_state(sctp_t *sctp, sctp_state_t state)
{
    if (sctp->state == state) return;
    sctp->state = state;
    LOGI("%d state %d", sctp->cfg.role, state);
    sctp->cmd_send_time = 0;
    sctp->cmd_resend_count = 0;
    if (state == SCTP_STATE_CONNECTED) {
        if (sctp->cfg.role == SCTP_ROLE_CLIENT && !sctp->cfg.manual_ch_create) {
            esp_peer_data_channel_cfg_t cfg = {
                .type = ESP_PEER_DATA_CHANNEL_PARTIAL_RELIABLE_TIMEOUT,
                .ordered = true,
                .label = NULL,
                .max_packet_lifetime = sctp->cfg.buf_time_out ? sctp->cfg.buf_time_out : 5000,
            };
            sctp_send_dcep(sctp, &cfg);
        }
        if (sctp->cfg.on_connected) sctp->cfg.on_connected(true, sctp->cfg.ctx);
    } else if (state == SCTP_STATE_DISCONNECTED) {
        if (sctp->cfg.on_close) {
            for (int i = 0; i < SCTP_STREAM_SLOTS; i++) {
                sctp_stream_t *stream = sctp->streams + i;
                if (!stream->used) continue;
                if (!stream->close_notify) {
                    stream->close_notify = true;
                    esp_peer_data_channel_info_t info = {stream->label, stream->stream_id};
                    sctp->cfg.on_close(&info, sctp->cfg.ctx);
                }
            }
        }
        if (sctp->cfg.on_connected) sctp->cfg.on_connected(false, sctp->cfg.ctx);
    }
}

LOCAL void sctp_do_connect(sctp_t *sctp)
{
    if (sctp->state == SCTP_STATE_CONNECTED || sctp->state == SCTP_STATE_DISCONNECTED) return;
    if (sctp->cfg.role != SCTP_ROLE_CLIENT) return;
    uint32_t cur_time = esp_timer_get_time() / 1000;
    if (sctp->cmd_send_time) {
        uint32_t interval = 20;
        if (sctp->cmd_resend_count) {
            uint32_t retry = sctp->cmd_resend_count - 1;
            interval = retry <= 5 ? 20 << retry : 640;
        }
        if (cur_time >= sctp->cmd_send_time && cur_time <= sctp->cmd_send_time + interval) return;
    }
    switch (sctp->state) {
    case SCTP_STATE_NONE:
    case SCTP_STATE_INIT_SENT:
        sctp_set_state(sctp, SCTP_STATE_INIT_SENT);
        sctp->cmd_send_time = cur_time;
        sctp->cmd_resend_count++;
        sctp_client_send_init_chunk(sctp);
        break;
    case SCTP_STATE_INIT_ACKED:
    case SCTP_STATE_COOKIE_ECHO_SENT:
        sctp_set_state(sctp, SCTP_STATE_COOKIE_ECHO_SENT);
        sctp->cmd_send_time = cur_time;
        sctp->cmd_resend_count++;
        sctp_client_send_echo_chunk(sctp, NULL, 0);
        break;
    default: break;
    }
}

int jetkvm_vendor_send_recfg(sctp_t *sctp, const char *label)
{
    sctp_stream_t *stream = get_stream_by_label(sctp, label);
    if (stream == NULL) return -1;
    stream->recfg_seq = sctp->recfg_seq;
    media_lib_mutex_lock(sctp->send_lock, -1);
    uint8_t *buf = sctp->buf;
    sctp_recfg_chunk_t *recfg_chunk = (sctp_recfg_chunk_t *)(buf + sizeof(sctp_header_t));
    recfg_chunk->common.type = SCTP_ASCONF;
    recfg_chunk->common.flags = 0;
    recfg_chunk->common.length = htons(sizeof(sctp_recfg_chunk_t));
    recfg_chunk->param.param_type = htons(13);
    recfg_chunk->param.param_len = htons(18);
    recfg_chunk->req_seq = htonl(stream->recfg_seq);
    recfg_chunk->resp_seq = 0;
    recfg_chunk->last_tsn = htonl(sctp->send_tsn - 1);
    recfg_chunk->stream_id = htons(stream->stream_id);
    *(uint16_t *)(buf + sizeof(sctp_header_t) + sizeof(sctp_recfg_chunk_t) - 2) = 0;
    LOGI("Send RECFG chunk for %s %d", label, stream->stream_id);
    int size = sizeof(sctp_header_t) + sizeof(sctp_recfg_chunk_t);
    int ret = jetkvm_vendor_fill_header_and_write(sctp, sctp->buf, size);
    media_lib_mutex_unlock(sctp->send_lock);
    stream->local_closed = true;
    sctp->recfg_seq++;
    jetkvm_vendor_stream_closed(sctp, stream);
    return ret;
}

void sctp_query(sctp_handle_t h)
{
    sctp_t *sctp = h;
    if (sctp == NULL) return;
    LOGI("Send: %lu Ack: %lu Fwd: %d:%lu Cached: %d Receive:%d", sctp->send_tsn, sctp->send_ack_tsn,
        sctp->need_forward, sctp->last_forward_tsn,
        sctp->send_buf_mngr ? buf_mngr_get_filled(sctp->send_buf_mngr) : 0, sctp->received_packets);
    LOGI("Recv: %lu ACK:%lu Cached: %d SackCount:%d", sctp->latest_recv_tsn, sctp->acked_tsn,
        sctp->recv_buf_mngr ? buf_mngr_get_filled(sctp->recv_buf_mngr) : 0, sctp->last_sack_count);
    if (query_details) {
        if (sctp->send_buf_mngr) {
            printf("%d send buffer status:\n", sctp->cfg.role);
            buf_mngr_print(sctp->send_buf_mngr);
        }
        if (sctp->recv_buf_mngr) {
            printf("%d receive buffer status:\n", sctp->cfg.role);
            buf_mngr_print(sctp->recv_buf_mngr);
        }
    }
}

void sctp_destroy(sctp_handle_t h)
{
    sctp_t *sctp = h;
    if (sctp == NULL) return;
    sctp_set_state(sctp, SCTP_STATE_DISCONNECTED);
    while (peer_atomic_load(&sctp->ref_count)) media_lib_thread_sleep(5);
    for (int i = 0; i < SCTP_STREAM_SLOTS; i++) {
        free_frames(sctp->streams[i].frames);
        reset_stream_direct_data(&sctp->streams[i]);
        if (sctp->streams[i].label) {
            media_lib_free(sctp->streams[i].label);
            sctp->streams[i].label = NULL;
        }
    }
    free_frames(sctp->free_frames);
    if (sctp->send_buf_mngr) {
        buf_mngr_destroy(sctp->send_buf_mngr);
        sctp->send_buf_mngr = NULL;
    }
    if (sctp->recv_buf_mngr) {
        buf_mngr_destroy(sctp->recv_buf_mngr);
        sctp->recv_buf_mngr = NULL;
    }
    if (sctp->frame_data) {
        media_lib_free(sctp->frame_data);
        sctp->frame_data = NULL;
    }
    if (sctp->lock) media_lib_mutex_destroy(sctp->lock);
    if (sctp->send_lock) media_lib_mutex_destroy(sctp->send_lock);
    media_lib_free(sctp);
}

sctp_handle_t sctp_create(sctp_cfg_t *cfg)
{
    sctp_t *sctp = media_lib_calloc(1, sizeof(sctp_t));
    if (sctp == NULL) return NULL;
    do {
        media_lib_mutex_create(&sctp->lock);
        if (sctp->lock == NULL) break;
        media_lib_mutex_create(&sctp->send_lock);
        if (sctp->send_lock == NULL) break;
        int timeout = cfg->buf_time_out ? cfg->buf_time_out : 5000;
        if (cfg->send_cache_size && !cfg->reliable_transport) {
            sctp->send_buf_mngr = buf_mngr_create(cfg->send_cache_size, timeout);
            buf_mngr_set_send_mode(sctp->send_buf_mngr, true);
        }
        if (cfg->recv_cache_size && !cfg->reliable_transport) {
            sctp->recv_buf_mngr = buf_mngr_create(cfg->recv_cache_size, timeout);
            buf_mngr_set_send_mode(sctp->recv_buf_mngr, false);
        }
        sctp->cfg = *cfg;
        if (cfg->role == SCTP_ROLE_SERVER) sctp->initial_stream_id = 1;
        else sctp->initial_stream_id = 2;
        sctp->recfg_seq = 1;
        sctp->local_port = htons(5000);
        sctp->remote_port = htons(5000);
        sctp->verification_tag = htonl(0x12345678);
        sctp->peer_stream_id = 256;
        sctp->send_tsn = cfg->init_tsn ? cfg->init_tsn : 1234;
        sctp->recfg_seq = sctp->send_tsn;
        sctp->send_ack_tsn = sctp->send_tsn - 1;
        return sctp;
    } while (0);
    sctp_destroy(sctp);
    return NULL;
}

LOCAL void sctp_update_forward(sctp_t *sctp)
{
    if (sctp->send_buf_mngr == NULL) return;
    if (sctp->need_abandon) {
        uint32_t cur_time = esp_timer_get_time() / 1000;
        uint32_t tsn = 0;
        bool found = buf_mngr_get_head_tsn(sctp->send_buf_mngr, &tsn, NULL);
        buf_info_t buf_info = {0};
        while (found) {
            found = buf_mngr_get_buffer(sctp->send_buf_mngr, tsn, &buf_info);
            if (!found || buf_info.reliable) break;
            if (cur_time < buf_info.add_time || cur_time > buf_info.add_time + 100) {
                if (buf_info.size == 0) buf_mngr_remove_for_abandon(sctp->send_buf_mngr, buf_info.buf_inst);
                tsn++;
            } else break;
        }
    }
    bool need_forward = false;
    buf_mngr_check_forward(sctp->send_buf_mngr, &need_forward, &sctp->forward_info);
    if (need_forward) sctp->last_forward_tsn = sctp->forward_info.abandon_tsn;
}

LOCAL int resend_packet(sctp_t *sctp, uint32_t start_time, buf_info_t *buf_info, char *reason)
{
    uint32_t cur_time = esp_timer_get_time() / 1000;
    if (buf_info->used && (cur_time < buf_info->last_send_time || cur_time > buf_info->last_send_time + 300)) {
        sctp_stream_t *stream = jetkvm_sctp_get_stream(sctp, buf_info->stream_id);
        if (stream == NULL) return 0;
        bool can_abandon = !buf_info->reliable;
        if (can_abandon && stream->type == ESP_PEER_DATA_CHANNEL_PARTIAL_RELIABLE_RETX) {
            if (buf_info->resend_count >= stream->unreliable_limit) {
                buf_mngr_remove_for_abandon(sctp->send_buf_mngr, buf_info->buf_inst);
                sctp_update_forward(sctp);
                return 1;
            }
        }
        if (can_abandon && stream->type == ESP_PEER_DATA_CHANNEL_PARTIAL_RELIABLE_TIMEOUT) {
            if (cur_time < buf_info->add_time || cur_time > buf_info->add_time + stream->unreliable_limit) {
                buf_mngr_remove_for_abandon(sctp->send_buf_mngr, buf_info->buf_inst);
                sctp_update_forward(sctp);
                return 1;
            }
        }
        cur_time = esp_timer_get_time() / 1000;
        if (cur_time <= start_time + 15) {
            int ret = sctp->cfg.write_packet(sctp->cfg.ctx, buf_info->buffer, buf_info->size);
            if (ret) return ret;
            buf_mngr_update_send_time(buf_info->buf_inst, cur_time);
            return 1;
        }
        return 0;
    }
    return 1;
}

LOCAL void resend_not_sacked(sctp_t *sctp)
{
    if (sctp->send_buf_mngr == NULL) return;
    uint32_t cur_time = esp_timer_get_time() / 1000;
    buf_info_t buf_info;
    bool found = buf_mngr_get_head_info(sctp->send_buf_mngr, &buf_info);
    while (found) {
        if (resend_packet(sctp, cur_time, &buf_info, "CHK") == 0) break;
        found = buf_mngr_get_next_nack_info(sctp->send_buf_mngr, &buf_info);
    }
}

LOCAL int sctp_direct_data_hdlr(sctp_t *sctp, sctp_data_chunk_t *data_chunk, bool add_only)
{
    sctp_data_chunk_t recv_chunk = *data_chunk;
    parse_data_chunk(&recv_chunk);
    sctp->acked_tsn = recv_chunk.tsn;
    sctp->latest_recv_tsn = recv_chunk.tsn;
    if (add_only) return 0;
    sctp_stream_t *stream = jetkvm_sctp_get_stream(sctp, recv_chunk.si);
    if (stream == NULL) {
        LOGE("Stream not found %d", recv_chunk.si);
        return -5;
    }
    int size = recv_chunk.length - sizeof(sctp_data_chunk_t);
    if (size < 0) return -8;
    if (recv_chunk.payload_type & 2) {
        stream->direct_size = 0;
        stream->direct_ppid = recv_chunk.ppid;
    }
    int ret = sctp_direct_append(stream, data_chunk->data, size);
    if (ret) {
        reset_stream_direct_data(stream);
        return ret;
    }
    if (recv_chunk.payload_type & 1) {
        if (stream->direct_size && sctp->cfg.on_message) {
            sctp->cfg.on_message(stream->direct_ppid, stream->stream_id, stream->direct_data,
                stream->direct_size, sctp->cfg.ctx);
        }
        stream->direct_size = 0;
    }
    return ret;
}

int sctp_outgoing_data(sctp_handle_t h, uint8_t *buf, int len, uint16_t stream_id, sctp_data_ppid_t ppid)
{
    sctp_t *sctp = h;
    if (sctp == NULL) return -3;
    sctp_add_ref(sctp);
    if (sctp->state != SCTP_STATE_CONNECTED) {
        sctp_dec_ref(sctp);
        return -3;
    }
    if (stream_id == 0) stream_id = get_default_stream_id(sctp);
    sctp_stream_t *stream = get_stream_by_stream_id(sctp, stream_id);
    if (stream == NULL) {
        sctp_dec_ref(sctp);
        return -5;
    }
    media_lib_mutex_lock(sctp->send_lock, -1);
    int head_size = sizeof(sctp_header_t) + sizeof(sctp_data_chunk_t);
    int payload_max = sizeof(sctp->buf) - head_size;
    /* Cache chunks are aligned to four bytes; include its per-fragment cost. */
    int need_size = len + ((len + payload_max - 1) / payload_max) * 32;
    if (sctp->send_buf_mngr && (int32_t)buf_mngr_get_avail(sctp->send_buf_mngr) < need_size) {
        media_lib_mutex_unlock(sctp->send_lock);
        sctp_dec_ref(sctp);
        return -9;
    }
    uint32_t block_tsn = sctp->send_ack_tsn + 10;
    if (sctp->send_buf_mngr && sctp->need_forward && TSN_LT(block_tsn, sctp->last_forward_tsn)) {
        media_lib_mutex_unlock(sctp->send_lock);
        sctp_dec_ref(sctp);
        return -9;
    }
    sctp_packet_t *packet = (sctp_packet_t *)sctp->buf;
    sctp_data_chunk_t *chunk = (sctp_data_chunk_t *)packet->chunks;
    packet->header.src_port = sctp->local_port;
    packet->header.dst_port = sctp->remote_port;
    packet->header.verification_tag = sctp->peer_tag;
    chunk->type = SCTP_DATA;
    chunk->payload_type = 2;
    chunk->si = htons(stream_id);
    chunk->seq_num = htons(stream->send_packet_num);
    chunk->ppid = htonl(ppid);
    uint32_t last_tsn = sctp->send_tsn;
    int pos = 0;
    int ret = 0;
    while (len > payload_max) {
        ret = sctp_outgoing_hdlr(sctp, stream, chunk, buf + pos, payload_max);
        if (ret) break;
        chunk->payload_type = 0;
        len -= payload_max;
        pos += payload_max;
    }
    if (ret == 0 && len > 0) {
        chunk->payload_type |= 1;
        ret = sctp_outgoing_hdlr(sctp, stream, chunk, buf + pos, len);
    }
    if (ret == 0) stream->send_packet_num++;
    else sctp->send_tsn = last_tsn;
    media_lib_mutex_unlock(sctp->send_lock);
    sctp_dec_ref(sctp);
    return ret;
}

LOCAL int sctp_send_dcep(sctp_t *sctp, esp_peer_data_channel_cfg_t *cfg)
{
    const char *label = cfg->label;
    sctp_stream_t *stream = jetkvm_sctp_get_stream(sctp, sctp->initial_stream_id);
    if (stream == NULL) {
        LOGE("Not enough stream for create channel");
        return -1;
    }
    media_lib_mutex_lock(sctp->send_lock, -1);
    sctp_packet_t *packet = (sctp_packet_t *)sctp->buf;
    sctp_data_chunk_t *chunk = (sctp_data_chunk_t *)packet->chunks;
    packet->header.src_port = sctp->local_port;
    packet->header.dst_port = sctp->remote_port;
    packet->header.verification_tag = sctp->peer_tag;
    chunk->type = SCTP_DATA;
    chunk->payload_type = 3;
    chunk->si = htons(sctp->initial_stream_id);
    chunk->seq_num = htons(stream->send_packet_num);
    chunk->ppid = htonl(PPID_CONTROL);
    sctp_dcep_data_t *dcep = (sctp_dcep_data_t *)chunk->data;
    dcep->msg_type = 3;
    bool reliable = cfg->type == ESP_PEER_DATA_CHANNEL_RELIABLE;
    /* The vendor has two independent if statements here. Preserve the overwrite. */
    if (cfg->type == ESP_PEER_DATA_CHANNEL_PARTIAL_RELIABLE_TIMEOUT) dcep->channel_type = 2;
    if (cfg->type == ESP_PEER_DATA_CHANNEL_PARTIAL_RELIABLE_RETX) dcep->channel_type = 1;
    else dcep->channel_type = 0;
    if (!cfg->ordered) dcep->channel_type |= 0x80;
    dcep->reliable = htonl(reliable ? 0 : cfg->max_packet_lifetime);
    if (label == NULL) label = "esp_channel";
    int label_len = strlen(label);
    if (stream->label) media_lib_free(stream->label);
    stream->type = cfg->type;
    stream->unreliable_limit = cfg->max_packet_lifetime;
    stream->label = media_lib_strdup(label);
    stream->reliable = cfg->type == ESP_PEER_DATA_CHANNEL_RELIABLE;
    stream->un_ordered = !cfg->ordered;
    dcep->label_len = htons(label_len);
    dcep->priority = 0;
    dcep->protocol_len = 0;
    memcpy(dcep->label, label, label_len);
    sctp_check_need_forward(sctp);
    int dcep_size = sizeof(sctp_dcep_data_t) + label_len;
    sctp_outgoing_reliable(sctp, stream, chunk, chunk->data, dcep_size);
    LOGI("Send DCEP %.*s event:%d type:%x si:%d", label_len, dcep->label, dcep->msg_type,
        dcep->channel_type, ntohs(chunk->si));
    stream->send_packet_num++;
    media_lib_mutex_unlock(sctp->send_lock);
    sctp->initial_stream_id += 2;
    return 0;
}

void sctp_handle_dcep(sctp_t *sctp, sctp_data_chunk_t *chunk, uint16_t chunk_size)
{
    sctp_dcep_data_t *dcep = (sctp_dcep_data_t *)chunk->data;
    if (dcep->msg_type == 2) {
        sctp_data_channel_opened(sctp, ntohs(chunk->si));
        return;
    }
    int dcep_len = chunk_size - sizeof(sctp_data_chunk_t);
    if (dcep_len < sizeof(sctp_dcep_data_t)) {
        LOGE("Bad DCEP len");
        return;
    }
    uint16_t stream_id = ntohs(chunk->si);
    uint8_t channel_type = dcep->channel_type;
    uint16_t label_len = ntohs(dcep->label_len);
    sctp_stream_t *stream = jetkvm_sctp_get_stream(sctp, stream_id);
    if (stream == NULL) return;
    if (label_len) {
        stream->label = media_lib_realloc(stream->label, label_len + 1);
        if (stream->label) {
            memcpy(stream->label, dcep->label, label_len);
            stream->label[label_len] = 0;
        }
        if ((channel_type & 15) == 0) stream->type = ESP_PEER_DATA_CHANNEL_RELIABLE;
        else if ((channel_type & 15) == 1) stream->type = ESP_PEER_DATA_CHANNEL_PARTIAL_RELIABLE_RETX;
        else if ((channel_type & 15) == 2) stream->type = ESP_PEER_DATA_CHANNEL_PARTIAL_RELIABLE_TIMEOUT;
        stream->unreliable_limit = ntohl(dcep->reliable);
        stream->reliable = stream->type == ESP_PEER_DATA_CHANNEL_RELIABLE;
        stream->un_ordered = channel_type >> 7;
        LOGI("Get DCEP %.*s event:%d type:%x si:%d param:%d", label_len, dcep->label, dcep->msg_type,
            dcep->channel_type, stream_id, stream->unreliable_limit);
        sctp_check_need_forward(sctp);
        jetkvm_sctp_send_dcep_ack(sctp, stream_id);
        sctp_data_channel_opened(sctp, stream_id);
    }
}

int sctp_check_data(sctp_handle_t sctp, const uint8_t *buf, int len)
{
    if (sctp_tracing) {
        sctp_packet_t *packet = (sctp_packet_t *)buf;
        if (packet->chunks[0].type == SCTP_DATA) {
            uint32_t checksum = packet->header.checksum;
            packet->header.checksum = 0;
            printf("Data tsn:%lu Sum:%x-%x\n", ntohl(*(uint32_t *)(buf + 16)),
                (unsigned)utils_crc32c_be(-1, buf, len), (unsigned)checksum);
            packet->header.checksum = checksum;
        } else if (packet->chunks[0].type == SCTP_SACK) {
            printf("SACK tsn:%lu\n", ntohl(*(uint32_t *)(buf + 16)));
        }
    }
    return 0;
}

LOCAL void remove_acked_packets(sctp_t *sctp, uint32_t acked_tsn)
{
    media_lib_mutex_lock(sctp->lock, -1);
    for (int i = 0; i < SCTP_STREAM_SLOTS; i++) {
        sctp_stream_t *stream = &sctp->streams[i];
        if (!stream->used || stream->reliable) continue;
        sctp_frame_t *head = stream->frames;
            uint32_t next_tsn = acked_tsn + 2;
            while (head) {
                if (TSN_LT(acked_tsn, head->min_tsn)) break;
                if (TSN_LT(acked_tsn, head->max_tsn)) break;
                bool has_end = head->frame_flag & 1;
                bool next_less = false;
                sctp_frame_t *next = head->next;
                if (next && TSN_LT(next->min_tsn, next_tsn)) next_less = true;
                if (next_less || has_end) {
                    remove_frame(sctp, stream, head);
                    return_frame(sctp, head);
                    head = stream->frames;
                    continue;
                }
                break;
            }
    }
    media_lib_mutex_unlock(sctp->lock);
}

LOCAL void do_sack(sctp_t *sctp)
{
    if (sctp->recv_buf_mngr == NULL) return;
    uint16_t limit_num = (sizeof(sctp->buf) - sizeof(sctp_header_t) - sizeof(sctp_sack_chunk_t)) / 4;
    sctp_packet_t *out_packet = (sctp_packet_t *)sctp->buf;
    sctp_sack_chunk_t *sack = (sctp_sack_chunk_t *)out_packet->chunks;
    uint16_t *ack_block = sack->blocks;
    media_lib_mutex_lock(sctp->send_lock, -1);
    media_lib_mutex_lock(sctp->lock, -1);
    if (sctp->last_sack_count > 20) {
        media_lib_mutex_unlock(sctp->lock);
        media_lib_mutex_unlock(sctp->send_lock);
        return;
    }
    sctp->last_sack_count++;
    int n = buf_mngr_get_ack_data(sctp->recv_buf_mngr, &sctp->acked_tsn, ack_block, limit_num);
    remove_acked_packets(sctp, sctp->acked_tsn);
    uint32_t recv_fifo_left = jetkvm_sctp_receive_window(sctp->recv_buf_mngr);
    sack->common.type = SCTP_SACK;
    sack->common.flags = 0;
    sack->wnd_size = htonl(recv_fifo_left);
    sack->ack_block_num = htons(n);
    sack->dup_tsn_num = 0;
    sack->cumulative_tsn_ack = htonl(sctp->acked_tsn);
    sctp->latest_recv_tsn = sctp->acked_tsn;
    if (n) sctp->latest_recv_tsn += ntohs(ack_block[(n - 1) * 2 + 1]);
    int len = sizeof(sctp_sack_chunk_t) + n * 4;
    sack->common.length = htons(len);
    len += sizeof(sctp_header_t);
    media_lib_mutex_unlock(sctp->lock);
    jetkvm_vendor_fill_header_and_write(sctp, sctp->buf, len);
    sctp->sack_send_time = esp_timer_get_time() / 1000;
    sctp->received_packets = 0;
    media_lib_mutex_unlock(sctp->send_lock);
}

LOCAL void handle_sack(sctp_t *sctp, sctp_sack_chunk_t *sack, uint16_t chunk_size)
{
    if (sctp->send_buf_mngr == NULL) return;
    uint32_t ack_tsn = ntohl(sack->cumulative_tsn_ack);
    uint16_t ack_num = ntohs(sack->ack_block_num);
    if (sizeof(sctp_sack_chunk_t) + ack_num * 4 > chunk_size) return;
    uint32_t last_sack = sctp->send_ack_tsn;
    if (TSN_LT(ack_tsn, last_sack)) return;
    sctp->send_ack_tsn = ack_tsn;
    if (query_details) printf("Receive SACK %lu num:%d\n", ack_tsn, ack_num);
    if (sctp->need_forward && do_forward_tsn(sctp)) return;
    buf_mngr_remove_range(sctp->send_buf_mngr, last_sack, ack_tsn);
    uint32_t nack_tsn = 1;
    uint32_t start_time = esp_timer_get_time() / 1000;
    buf_info_t buf_info;
    uint16_t *v = sack->blocks;
    for (int i = 0; i < ack_num; i++) {
        uint16_t start = ntohs(v[0]);
        uint16_t end = ntohs(v[1]);
        if (query_details) printf("ACK [%lu-%lu]\n", ack_tsn + start, ack_tsn + end);
        v += 2;
        if (end < start) {
            LOGE("Bad ack %d-%d", start, end);
            return;
        }
        /* Preserve the vendor's invariant-failure store for binary equivalence. */
        if (start > 10000) *(int *)0 = 0;
        buf_mngr_remove_range(sctp->send_buf_mngr, ack_tsn + start, ack_tsn + end);
        for (; nack_tsn < start; nack_tsn++) {
            bool found = buf_mngr_get_buffer(sctp->send_buf_mngr, ack_tsn + nack_tsn, &buf_info);
            if (found) {
                if (resend_packet(sctp, start_time, &buf_info, "SACK") == 0) break;
            }
        }
        nack_tsn = end + 1;
    }
}

#define SEQ_LT(a,b) (((a) < (b) && (b) < (a) + UINT32_C(0x8000)) || ((a) - (b) > UINT32_C(0x8000)))
#define IN_RANGE(v,low,high) (((high) >= (low) && (v) >= (low) && (v) <= (high)) || ((high) < (low) && ((v) >= (low) || (v) <= (high))))

LOCAL sctp_frame_t *insert_between(sctp_t *sctp, sctp_stream_t *stream, sctp_frame_t *frame, sctp_data_chunk_t *data_chunk)
{
    uint8_t payload_type = data_chunk->payload_type;
    bool next_started = false;
    sctp_frame_t *next = frame->next;
    if (next && (next->frame_flag & 2)) next_started = true;
    if (payload_type & 2) {
        if ((payload_type & 1) || next_started || next == NULL) return insert_new(sctp, stream, frame, data_chunk);
        return insert_next(next, data_chunk);
    }
    if (payload_type & 1) {
        if (frame->frame_flag & 1) return insert_new(sctp, stream, frame, data_chunk);
        return insert_pre(frame, data_chunk);
    }
    if (frame->frame_flag & 1) {
        if (next_started || next == NULL) return insert_new(sctp, stream, frame, data_chunk);
        return insert_next(next, data_chunk);
    }
    if (next && !next_started) return merge_next(sctp, frame);
    return insert_pre(frame, data_chunk);
}

LOCAL sctp_frame_t *split_frame(sctp_t *sctp, sctp_stream_t *stream, sctp_frame_t *frame, sctp_data_chunk_t *data_chunk)
{
    sctp_frame_t *new_frame = alloc_frame(sctp, stream, data_chunk);
    if (new_frame == NULL) return NULL;
    sctp_frame_t *next = frame->next;
    uint32_t max_tsn = frame->max_tsn;
    uint8_t frame_flag = frame->frame_flag;
    uint8_t payload_type = data_chunk->payload_type;
    if (payload_type & 2) {
        frame->frame_flag &= ~1;
        frame->max_tsn = buf_mngr_get_pre_tsn(sctp->recv_buf_mngr, data_chunk->tsn);
        if (!(payload_type & 1)) {
            new_frame->frame_flag |= frame_flag & 1;
            new_frame->frame_flag |= 2;
            new_frame->max_tsn = max_tsn;
            frame->next = new_frame;
            new_frame->next = next;
            return new_frame;
        }
        sctp_frame_t *tail = alloc_frame(sctp, stream, data_chunk);
        if (tail == NULL) return NULL;
        uint32_t next_tsn = buf_mngr_get_next_tsn(sctp->recv_buf_mngr, data_chunk->tsn);
        new_frame->frame_flag |= 3;
        tail->min_tsn = next_tsn;
        tail->max_tsn = max_tsn;
        tail->frame_flag &= ~2;
        frame->next = new_frame;
        new_frame->next = tail;
        tail->next = next;
        return new_frame;
    }
    frame->max_tsn = data_chunk->tsn;
    frame->frame_flag = frame_flag | 1;
    new_frame->min_tsn = buf_mngr_get_next_tsn(sctp->recv_buf_mngr, data_chunk->tsn);
    new_frame->max_tsn = max_tsn;
    new_frame->frame_flag = frame_flag & ~2;
    frame->next = new_frame;
    new_frame->next = next;
    return frame;
}

LOCAL sctp_frame_t *get_unordered_frames(sctp_t *sctp, sctp_stream_t *stream, sctp_data_chunk_t *data_chunk)
{
    media_lib_mutex_lock(sctp->lock, -1);
    sctp_frame_t *out_frame;
    do {
        if (stream->frames == NULL) {
            sctp_frame_t *frame = alloc_frame(sctp, stream, data_chunk);
            if (frame) {
                stream->frames = frame;
                frame->next = NULL;
            }
            out_frame = frame;
            break;
        }
        uint8_t payload_type = data_chunk->payload_type;
        /* The unsigned end flag preserves the vendor's GCC -Og normalization. */
        bool is_bound = (payload_type & 2) || (payload_type & 1U);
        sctp_frame_t *frame = stream->frames;
        if (TSN_GT(frame->min_tsn, data_chunk->tsn)) {
            if ((frame->frame_flag & 2) || (payload_type & 1)) {
                sctp_frame_t *new_frame = alloc_frame(sctp, stream, data_chunk);
                if (new_frame) {
                    new_frame->next = frame;
                    stream->frames = new_frame;
                }
                out_frame = new_frame;
            } else {
                frame->min_tsn = data_chunk->tsn;
                out_frame = frame;
            }
            break;
        }
        bool found = false;
        sctp_frame_t *pre = frame;
        while (frame) {
            if (TSN_GT(frame->min_tsn, data_chunk->tsn)) break;
            if (IN_RANGE(data_chunk->tsn, frame->min_tsn, frame->max_tsn)) {
                found = true;
                break;
            }
            pre = frame;
            frame = frame->next;
        }
        if (!found) out_frame = insert_between(sctp, stream, pre, data_chunk);
        else if (is_bound) out_frame = split_frame(sctp, stream, frame, data_chunk);
        else out_frame = frame;
    } while (0);
    media_lib_mutex_unlock(sctp->lock);
    return out_frame;
}

sctp_frame_t *jetkvm_vendor_get_frame(sctp_t *sctp, sctp_stream_t *stream, sctp_data_chunk_t *data_chunk)
{
    if (stream->un_ordered) return get_unordered_frames(sctp, stream, data_chunk);
    media_lib_mutex_lock(sctp->lock, -1);
    uint32_t seq_num = data_chunk->seq_num;
    sctp_frame_t *frame = stream->frames;
    if (frame == NULL || !IN_RANGE(seq_num, stream->start_seq_num, stream->end_seq_num)) {
        frame = alloc_frame(sctp, stream, data_chunk);
        if (frame == NULL) {
            media_lib_mutex_unlock(sctp->lock);
            return NULL;
        }
        if (stream->frames == NULL) {
            stream->frames = frame;
            stream->tail = frame;
            stream->end_seq_num = seq_num;
            stream->start_seq_num = seq_num;
            frame->next = NULL;
        } else if (SEQ_LT(seq_num, stream->start_seq_num)) {
            frame->next = stream->frames;
            stream->frames = frame;
            stream->start_seq_num = seq_num;
        } else {
            stream->tail->next = frame;
            frame->next = NULL;
            stream->end_seq_num = seq_num;
            stream->tail = frame;
        }
        media_lib_mutex_unlock(sctp->lock);
        return frame;
    }
    sctp_frame_t *pre = frame;
    while (frame) {
        if (frame->seq_num == seq_num) {
            media_lib_mutex_unlock(sctp->lock);
            return frame;
        }
        if (SEQ_LT(seq_num, frame->seq_num)) {
            sctp_frame_t *new_frame = alloc_frame(sctp, stream, data_chunk);
            if (new_frame == NULL) return NULL;
            pre->next = new_frame;
            new_frame->next = frame;
            media_lib_mutex_unlock(sctp->lock);
            return new_frame;
        }
        pre = frame;
        frame = frame->next;
    }
    LOGE("Should not happen not in frame list");
    /* Preserve the vendor's invariant-failure store for binary equivalence. */
    *(int *)0 = 0;
    media_lib_mutex_unlock(sctp->lock);
    return NULL;
}

LOCAL bool try_send_one_frame(sctp_t *sctp, sctp_stream_t *stream, sctp_frame_t *frame)
{
    uint32_t frame_size = 0;
    bool fill_done = false;
    if ((frame->frame_flag & 3) == 3) fill_done = buf_mngr_is_frame_complete(sctp->recv_buf_mngr, frame->min_tsn, frame->max_tsn, &frame_size);
    if (fill_done && frame_size) {
        if (frame_size > sctp->max_frame_size) {
            uint8_t *buffer = media_lib_realloc(sctp->frame_data, frame_size);
            if (buffer) {
                sctp->frame_data = buffer;
                sctp->max_frame_size = frame_size;
            } else LOGE("No buffer for frame size %d drop directly", frame_size);
        }
        if (sctp->max_frame_size >= frame_size) {
            buf_mngr_fill_frame(sctp->recv_buf_mngr, frame->min_tsn, frame->max_tsn, sctp->frame_data, frame_size);
            if (frame_size && sctp->cfg.on_message) sctp->cfg.on_message(frame->ppid, stream->stream_id, sctp->frame_data, frame_size, sctp->cfg.ctx);
        }
        if (stream->reliable) {
            stream->frame_sent = true;
            stream->last_sent_seq = frame->seq_num;
        }
    } else if (stream->reliable && frame->max_tsn > frame->min_tsn) {
        LOGW("Reliable drop done:%d size:%d [%lu - %lu]", fill_done, frame_size, frame->min_tsn, frame->max_tsn);
    }
    buf_mngr_remove_range_by_stream(sctp->recv_buf_mngr, frame->min_tsn, frame->max_tsn);
    return_frame(sctp, frame);
    return true;
}

LOCAL void send_all_frames(sctp_t *sctp, sctp_frame_t *frame)
{
    while (frame) {
        sctp_frame_t *head = frame->next;
        if (sctp_details_enabled()) printf("Try to remove %d from %lu to %lu\n", sctp->streams[frame->stream_idx].stream_id, frame->min_tsn, frame->max_tsn);
        try_send_one_frame(sctp, &sctp->streams[frame->stream_idx], frame);
        frame = head;
    }
}

sctp_frame_t *check_send_all_stream(sctp_t *sctp, bool send)
{
    media_lib_mutex_lock(sctp->lock, -1);
    sctp_frame_t *send_frames = NULL;
    sctp_frame_t *send_tail = NULL;
    for (int i = 0; i < SCTP_STREAM_SLOTS; i++) {
        sctp_stream_t *stream = &sctp->streams[i];
        if (!stream->used) continue;
        sctp_frame_t *frame = stream->frames;
        while (frame) {
            if (TSN_GT(frame->min_tsn, sctp->acked_tsn)) break;
            sctp_frame_t *next = frame->next;
            bool next_acked = false;
            if (next && !TSN_LT(sctp->acked_tsn, next->min_tsn)) next_acked = true;
            if (((frame->frame_flag & 1) && !TSN_LT(sctp->acked_tsn, frame->max_tsn)) || next_acked) {
                remove_frame(sctp, stream, frame);
                if (send_tail) send_tail->next = frame;
                else send_frames = frame;
                send_tail = frame;
                frame = next;
            } else break;
        }
    }
    if (send_tail) send_tail->next = NULL;
    media_lib_mutex_unlock(sctp->lock);
    if (send) {
        send_all_frames(sctp, send_frames);
        return NULL;
    }
    return send_frames;
}

void update_receive_ack_tsn(sctp_t *sctp, bool check_send, int line)
{
    media_lib_mutex_lock(sctp->lock, -1);
    uint32_t acked_tsn = buf_mngr_get_ack_tsn(sctp->recv_buf_mngr, sctp->acked_tsn);
    if (TSN_LT(sctp->acked_tsn, acked_tsn)) sctp->acked_tsn = acked_tsn;
    else check_send = false;
    media_lib_mutex_unlock(sctp->lock);
    if (check_send) check_send_all_stream(sctp, true);
}

LOCAL int remove_for_full(sctp_t *sctp, sctp_stream_t *stream)
{
    sctp_frame_t *head_frame = stream->frames;
    if (head_frame == NULL || stream->reliable) return 0;
    media_lib_mutex_lock(sctp->lock, -1);
    bool removed = buf_mngr_try_remove_range_by_stream(sctp->recv_buf_mngr, head_frame->min_tsn, head_frame->max_tsn);
    if (removed) {
        remove_frame(sctp, stream, head_frame);
        return_frame(sctp, head_frame);
    }
    media_lib_mutex_unlock(sctp->lock);
    if (removed) update_receive_ack_tsn(sctp, true, 491);
    return 0;
}

LOCAL int check_send_one_frame(sctp_t *sctp, sctp_stream_t *in_stream, sctp_frame_t *in_frame, uint32_t tsn)
{
    media_lib_mutex_lock(sctp->lock, -1);
    sctp_frame_t *send_frames = NULL;
    sctp_frame_t *send_tail = NULL;
    for (int i = 0; i < SCTP_STREAM_SLOTS; i++) {
        sctp_stream_t *stream = &sctp->streams[i];
        if (!stream->used) continue;
        if (!stream->un_ordered && (sctp->stream_num <= 1 || !stream->frame_sent)) continue;
        sctp_frame_t *frame = stream->frames;
        while (frame) {
            if (TSN_LT(tsn, frame->min_tsn)) break;
            if (TSN_LT(frame->max_tsn, tsn)) {
                frame = frame->next;
                continue;
            }
            if (frame->frame_flag != 3) break;
            if (!stream->un_ordered && frame->seq_num != stream->last_sent_seq + 1) break;
            uint32_t frame_size = 0;
            if (buf_mngr_is_frame_complete(sctp->recv_buf_mngr, frame->min_tsn, frame->max_tsn, &frame_size)) {
                remove_frame(sctp, stream, frame);
                if (send_tail) send_tail->next = frame;
                else send_frames = frame;
                send_tail = frame;
            }
            break;
        }
    }
    if (send_tail) send_tail->next = NULL;
    media_lib_mutex_unlock(sctp->lock);
    sctp_frame_t *all_frame = check_send_all_stream(sctp, false);
    if (send_tail) send_tail->next = all_frame;
    else send_frames = all_frame;
    if (send_frames) {
        send_all_frames(sctp, send_frames);
        measure_start("Ack");
        sctp_do_sack(sctp);
        measure_stop("Ack");
    }
    return 0;
}

int jetkvm_vendor_packet_hdlr(sctp_t *sctp, sctp_data_chunk_t *data_chunk, int add_only)
{
    sctp_data_chunk_t recv_chunk = *data_chunk;
    parse_data_chunk(&recv_chunk);
    sctp->received_packets++;
    sctp->last_sack_count = 0;
    if (!TSN_GT(recv_chunk.tsn, sctp->acked_tsn)) return 0;
    uint8_t *buffer = data_chunk->data;
    if (sctp_details_enabled()) printf("Receive tsn:%lu stream %d seq %d len %d flag:%d\n", recv_chunk.tsn, recv_chunk.si, recv_chunk.seq_num, recv_chunk.length, recv_chunk.payload_type);
    sctp_stream_t *stream = jetkvm_sctp_get_stream(sctp, recv_chunk.si);
    if (stream == NULL) {
        LOGE("Stream not found %d", recv_chunk.si);
        return -1;
    }
    int size = add_only ? 0 : recv_chunk.length - sizeof(sctp_data_chunk_t);
    int ret = buf_mngr_add_reliable(sctp->recv_buf_mngr, stream->reliable, recv_chunk.payload_type & 1,
        recv_chunk.tsn, recv_chunk.si, 0, buffer, size);
    if (ret > 0) return 0;
    sctp_frame_t *frame = jetkvm_sctp_get_frame(sctp, stream, &recv_chunk);
    if (frame == NULL) {
        LOGE("Fail to allocate frame");
        return -1;
    }
    if (ret < 0) {
        LOGE("%d No buffer for TSN %d", sctp->cfg.role, recv_chunk.tsn);
        remove_for_full(sctp, stream);
        return -1;
    }
    if (add_only) {
        update_receive_ack_tsn(sctp, true, 559);
        return 0;
    }
    if (TSN_GT(frame->min_tsn, recv_chunk.tsn)) frame->min_tsn = recv_chunk.tsn;
    if (TSN_LT(frame->max_tsn, recv_chunk.tsn)) frame->max_tsn = recv_chunk.tsn;
    if (recv_chunk.payload_type & 2) frame->frame_flag |= 2;
    if (recv_chunk.payload_type & 1) frame->frame_flag |= 1;
    frame->fill_num++;
    update_receive_ack_tsn(sctp, false, 575);
    check_send_one_frame(sctp, stream, frame, recv_chunk.tsn);
    return 0;
}

void sctp_incoming_data(sctp_handle_t h, const uint8_t *buf, int len)
{
    sctp_t *sctp = h;
    if (sctp == NULL) return;
    sctp_add_ref(sctp);
    if (!sctp_verify_packet(sctp, buf, len)) {
        sctp_dec_ref(sctp);
        return;
    }
    int pos = sizeof(sctp_header_t);
    while (((pos + 3) & ~3) < len) {
        sctp_chunk_t *chunk = (sctp_chunk_t *)(buf + pos);
        uint16_t chunk_size = ntohs(chunk->length);
        chunk_size = (chunk_size + 3) & ~3;
        if (chunk_size + pos > len || chunk_size < sizeof(sctp_chunk_t)) break;
        if (chunk->type != SCTP_DATA && chunk->type != SCTP_SACK && chunk->type != SCTP_FORWARD_TSN) {
            LOGI("%d Receive chunk %d %s", sctp->cfg.role, chunk->type, chunk_type_to_name(chunk->type));
        }
        switch (chunk->type) {
        case SCTP_HEARTBEAT:
            /* Echo the received heartbeat chunk. */
            media_lib_mutex_lock(sctp->send_lock, -1);
            memcpy(sctp->buf + sizeof(sctp_header_t), chunk, chunk_size);
            jetkvm_vendor_fill_header_and_write(sctp, sctp->buf, sizeof(sctp_header_t) + chunk_size);
            media_lib_mutex_unlock(sctp->send_lock);
            break;
        case SCTP_ABORT:
        case SCTP_SHUTDOWN:
            sctp_set_state(sctp, SCTP_STATE_DISCONNECTED);
            break;
        case SCTP_ASCONF:
            jetkvm_sctp_handle_recfg(sctp, chunk);
            break;
        case SCTP_DATA: {
            sctp_data_chunk_t *data_chunk = (sctp_data_chunk_t *)chunk;
            uint32_t ppid = ntohl(data_chunk->ppid);
            if (ppid != PPID_STRING && ppid != PPID_BINARY) {
                if (ppid == PPID_CONTROL) sctp_handle_dcep(sctp, data_chunk, chunk_size);
                else {
                    LOGE("Skip bad ppid %d", ppid);
                    break;
                }
            }
            if (sctp->cfg.reliable_transport) sctp_direct_data_hdlr(sctp, data_chunk, ppid == PPID_CONTROL);
            else jetkvm_sctp_receive_data(sctp, data_chunk, ppid == PPID_CONTROL);
            if (!sctp->cfg.reliable_transport) jetkvm_sctp_check_sack(sctp);
            break;
        }
        case SCTP_INIT_ACK: {
            sctp_init_chunk_t *init_ack = (sctp_init_chunk_t *)chunk;
            sctp->acked_tsn = ntohl(init_ack->initial_tsn) - 1;
            int cookie_size = 0;
            uint8_t *cookie = get_cookie_data(buf, &cookie_size);
            sctp->peer_tag = init_ack->initiate_tag;
            sctp->remote_port = ((sctp_header_t *)buf)->src_port;
            if (cookie) {
                sctp_set_state(sctp, SCTP_STATE_INIT_ACKED);
                sctp_client_send_echo_chunk(sctp, cookie, cookie_size);
            }
            break;
        }
        case SCTP_COOKIE_ACK:
            sctp_set_state(sctp, SCTP_STATE_CONNECTED);
            break;
        case SCTP_INIT: {
            if (sctp->cfg.role == SCTP_ROLE_CLIENT) {
                LOGW("Client need send init, skip init request");
                break;
            }
            sctp_set_state(sctp, SCTP_STATE_INIT_ACKED);
            sctp_init_chunk_t *init_chunk = (sctp_init_chunk_t *)chunk;
            sctp->acked_tsn = ntohl(init_chunk->initial_tsn) - 1;
            sctp->peer_tag = init_chunk->initiate_tag;
            sctp->remote_port = ((sctp_header_t *)buf)->src_port;
            uint8_t *param_data = (uint8_t *)init_chunk->param;
            for (int offset = sizeof(sctp_init_chunk_t); offset < chunk_size;) {
                sctp_chunk_param_t *param = (sctp_chunk_param_t *)param_data;
                if (param->type == htons(0x8008)) {
                    uint8_t ext_num = ntohs(param->length) - sizeof(sctp_chunk_param_t);
                    for (int i = 0; i < ext_num; i++) {
                        LOGI("Support ext %x", param->value[i]);
                        if (param->value[i] == SCTP_FORWARD_TSN) sctp->support_forward_tsn = true;
                    }
                }
                int param_len = (ntohs(param->length) + 3) & ~3;
                param_data += param_len;
                offset += param_len;
            }
            sctp_client_send_init_ack_chunk(sctp);
            break;
        }
        case SCTP_SACK:
            handle_sack(sctp, (sctp_sack_chunk_t *)chunk, chunk_size);
            break;
        case SCTP_FORWARD_TSN:
            jetkvm_sctp_handle_forward_tsn(sctp, (sctp_forward_tsn_chunk_t *)chunk);
            break;
        case SCTP_COOKIE_ECHO:
            sctp_client_send_echo_ack(sctp);
            sctp_set_state(sctp, SCTP_STATE_CONNECTED);
            pos = len;
            break;
        default: break;
        }
        pos += chunk_size;
    }
    sctp_dec_ref(sctp);
}

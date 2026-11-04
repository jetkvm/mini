#ifndef ESP_PEER_CORE_BUF_MNGR_H
#define ESP_PEER_CORE_BUF_MNGR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct buf_list_t {
    uint8_t *buffer;
    int size;
    int alloc_size;
    uint32_t add_time;
    uint32_t last_send_time;
    uint32_t tsn;
    uint32_t packet_id;
    uint16_t stream_id;
    uint8_t used:1;
    uint8_t reliable:1;
    uint8_t abandon:1;
    uint8_t is_end:1;
    uint16_t resend_count;
    struct buf_list_t *pre;
    struct buf_list_t *next;
} buf_list_t;

typedef struct {
    uint16_t abandon_stream_id[2];
    uint32_t abandon_packet_id[2];
    uint8_t abandon_stream_num;
    uint32_t abandon_tsn;
} buf_abandon_info_t;

typedef struct {
    void *lock;
    uint32_t min_tsn;
    uint32_t max_tsn;
    uint32_t remove_tsn;
    buf_abandon_info_t abandon_info;
    buf_list_t *free_list;
    buf_list_t *head;
    buf_list_t *tail;
    int fill_size;
    int limit_size;
    int timeout;
    uint8_t filled:1;
    uint8_t removed:1;
    uint8_t has_abandon:1;
    uint8_t need_forward:1;
    uint8_t send_mode:1;
    uint32_t last_ack;
} buf_mngr_t;

typedef struct {
    uint8_t *buffer;
    int size;
    uint16_t resend_count;
    uint32_t tsn;
    uint32_t last_send_time;
    uint32_t add_time;
    uint16_t stream_id;
    uint8_t used:1;
    uint8_t reliable:1;
    void *buf_inst;
} buf_info_t;

buf_mngr_t *buf_mngr_create(int limit_size, int timeout);
void buf_mngr_destroy(buf_mngr_t *mngr);
void buf_mngr_set_send_mode(buf_mngr_t *mngr, bool send_mode);
void buf_mngr_set_forward_mode(buf_mngr_t *mngr, uint32_t last_ack);
bool buf_mngr_get_head_info(buf_mngr_t *mngr, buf_info_t *buf_info);
bool buf_mngr_get_next_nack_info(buf_mngr_t *mngr, buf_info_t *buf_info);
bool buf_mngr_get_head_tsn(buf_mngr_t *mngr, uint32_t *tsn, uint32_t *packet_id);
int buf_mngr_update_send_time(void *buf_inst, uint32_t send_time);
bool buf_mngr_get_buffer(buf_mngr_t *mngr, uint32_t tsn, buf_info_t *buf_info);
void buf_mngr_remove(buf_mngr_t *mngr, void *ptr);
void buf_mngr_remove_for_abandon(buf_mngr_t *mngr, void *ptr);
void buf_mngr_remove_for_abandon_by_tsn(buf_mngr_t *mngr, uint32_t tsn);
void buf_mngr_remove_by_stream(buf_mngr_t *mngr, uint16_t stream_id);
void buf_mngr_remove_range(buf_mngr_t *mngr, uint32_t start_tsn, uint32_t end_tsn);
bool buf_mngr_check_remove_range_by_stream(buf_mngr_t *mngr, bool check_reliable, uint32_t start_tsn, uint32_t end_tsn);
bool buf_mngr_try_remove_range_by_stream(buf_mngr_t *mngr, uint32_t start_tsn, uint32_t end_tsn);
void buf_mngr_remove_range_by_stream(buf_mngr_t *mngr, uint32_t start_tsn, uint32_t end_tsn);
int buf_mngr_check_overdue(buf_mngr_t *mngr);
int buf_mngr_check_forward(buf_mngr_t *mngr, bool *need_forward, buf_abandon_info_t *info);
uint32_t buf_mngr_get_filled(buf_mngr_t *mngr);
uint32_t buf_mngr_get_avail(buf_mngr_t *mngr);
int buf_mngr_insert_abandon(buf_mngr_t *mngr, buf_list_t *pre, uint32_t start_tsn, uint32_t end_tsn);
int buf_mngr_abandon(buf_mngr_t *mngr, uint32_t acked_tsn, uint32_t forward_tsn);
bool buf_mngr_has_ongoing(buf_mngr_t *mngr, uint16_t stream_id);
int buf_mngr_add_reliable(buf_mngr_t *mngr, bool reliable, bool is_end, uint32_t tsn, uint16_t stream_id, uint32_t packet_id, uint8_t *buffer, int size);
int buf_mngr_add(buf_mngr_t *mngr, uint32_t tsn, uint16_t stream_id, uint32_t packet_id, uint8_t *buffer, int size);
uint32_t buf_mngr_get_ack_tsn(buf_mngr_t *mngr, uint32_t last_ack);
bool buf_mngr_is_frame_complete(buf_mngr_t *mngr, uint32_t start_tsn, uint32_t end_tsn, uint32_t *frame_size);
int buf_mngr_fill_frame(buf_mngr_t *mngr, uint32_t start_tsn, uint32_t end_tsn, uint8_t *buffer, int size);
int buf_mngr_get_ack_data(buf_mngr_t *mngr, uint32_t *ack_end, uint16_t *ack_block, int n);
uint32_t buf_mngr_get_next_tsn(buf_mngr_t *mngr, uint32_t tsn);
uint32_t buf_mngr_get_pre_tsn(buf_mngr_t *mngr, uint32_t tsn);
int buf_mngr_get_nack_data(buf_mngr_t *mngr, uint32_t *nack_entry, int n);
void buf_mngr_print(buf_mngr_t *mngr);
bool buf_mngr_is_expected(buf_mngr_t *mngr, uint32_t *arr, int num);

_Static_assert(sizeof(buf_list_t) == 44, "vendor buffer list size");
_Static_assert(sizeof(buf_abandon_info_t) == 20, "vendor abandon info size");
_Static_assert(sizeof(buf_mngr_t) == 68, "vendor buffer manager size");
_Static_assert(sizeof(buf_info_t) == 32, "vendor buffer info size");
_Static_assert(offsetof(buf_list_t, pre) == 36, "vendor list previous offset");
_Static_assert(offsetof(buf_mngr_t, head) == 40, "vendor manager head offset");
_Static_assert(offsetof(buf_mngr_t, last_ack) == 64, "vendor manager acknowledgment offset");

#endif

/* buf mngr implementation.
 * See LICENSE and LICENSES/ for copyright and licensing notices.
 */
#include "peer_platform.h"
#include "buf_mngr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOCK(m) media_lib_mutex_lock((m)->lock, UINT32_MAX)
#define UNLOCK(m) media_lib_mutex_unlock((m)->lock)
#define TSN_LT(a,b) (((a) < (b) && (b) < (a) + UINT32_C(0x80000000)) || ((a) - (b) > UINT32_C(0x80000000)))
#define LOG_ERROR(fmt, ...) esp_log((esp_log_config_t){.data=1}, "BUF_MNGR", "E (%lu) %s: " fmt "\n", esp_log_timestamp(), "BUF_MNGR", __VA_ARGS__)
#define LOG_INFO(fmt, ...) esp_log((esp_log_config_t){.data=3}, "BUF_MNGR", "I (%lu) %s: " fmt "\n", esp_log_timestamp(), "BUF_MNGR", __VA_ARGS__)

static buf_list_t *buf_mngr_get(buf_mngr_t *mngr, uint32_t tsn)
{
    if (!(mngr->max_tsn >= mngr->min_tsn && tsn >= mngr->min_tsn && tsn <= mngr->max_tsn)
        && !(mngr->max_tsn < mngr->min_tsn && (tsn >= mngr->min_tsn || tsn <= mngr->max_tsn))) {
        return NULL;
    }
    buf_list_t *head = mngr->head;
    while (head) {
        if (TSN_LT(tsn, head->tsn)) {
            return NULL;
        }
        if (head->tsn == tsn) {
            return head;
        }
        head = head->next;
    }
    return NULL;
}

static inline void set_buf_info(buf_info_t *buf_info, buf_list_t *item)
{
    buf_info->last_send_time = item->last_send_time;
    buf_info->buffer = item->buffer;
    buf_info->tsn = item->tsn;
    buf_info->size = item->size;
    buf_info->buf_inst = item;
    buf_info->used = item->used;
    buf_info->reliable = item->reliable;
    buf_info->add_time = item->add_time;
    buf_info->stream_id = item->stream_id;
    buf_info->resend_count = item->resend_count;
}

static void add_to_free_list(buf_mngr_t *mngr, buf_list_t *header, buf_list_t *tail)
{
    if (mngr->free_list) {
        tail->next = mngr->free_list;
        mngr->free_list->pre = tail;
        mngr->free_list = header;
    } else {
        mngr->free_list = header;
        tail->next = NULL;
    }
    mngr->min_tsn = tail->tsn;
}

static buf_list_t *allocate_buf(buf_mngr_t *mngr, int size)
{
    if (!mngr->free_list) {
        buf_list_t *item = media_lib_calloc(1, sizeof(buf_list_t));
        if (!item) {
            return NULL;
        }
        if (size) {
            item->buffer = media_lib_malloc(size);
            if (!item->buffer) {
                media_lib_free(item);
                return NULL;
            }
        }
        item->alloc_size = size;
        return item;
    }
    buf_list_t *item = mngr->free_list;
    if (item->alloc_size < size) {
        uint8_t *new_buffer = media_lib_realloc(item->buffer, size);
        if (!new_buffer) {
            return NULL;
        }
        item->buffer = new_buffer;
        item->alloc_size = size;
    }
    mngr->free_list = item->next;
    return item;
}

static void update_abandon_info(buf_mngr_t *mngr, buf_list_t *item)
{
    buf_abandon_info_t *info = &mngr->abandon_info;
    info->abandon_tsn = item->tsn;
    int sel = -1;
    for (int i = 0; i < info->abandon_stream_num; i++) {
        if (item->stream_id == info->abandon_stream_id[i]) {
            sel = i;
            break;
        }
    }
    if (sel == -1) {
        if (info->abandon_stream_num >= 2) {
            LOG_ERROR("Stream over limited for %d", item->stream_id);
            return;
        }
        sel = info->abandon_stream_num;
        info->abandon_stream_id[sel] = item->stream_id;
        info->abandon_stream_num++;
    }
    info->abandon_packet_id[sel] = item->packet_id;
}

static int remove_bufs(buf_mngr_t *mngr)
{
    int n = 0;
    buf_list_t *head = mngr->head;
    buf_list_t *new_head = head;
    uint32_t cur_time = esp_timer_get_time() / 1000;
    uint32_t remove_size = 0;
    buf_list_t *cur = head;
    while (cur) {
        new_head = cur;
        if (cur->used) {
            if (cur->reliable) {
                break;
            }
            if (mngr->send_mode) {
                break;
            }
            if (cur_time <= mngr->timeout + cur->add_time) {
                break;
            }
            cur->abandon = 1;
            cur->used = 0;
        }
        if (mngr->need_forward) {
            if (cur->tsn == mngr->last_ack + 1) {
                mngr->last_ack++;
            } else if (TSN_LT(mngr->last_ack, cur->tsn)) {
                break;
            }
        }
        remove_size += cur->alloc_size;
        mngr->remove_tsn = cur->tsn;
        if (cur->abandon) {
            update_abandon_info(mngr, cur);
            mngr->has_abandon = 1;
        }
        mngr->removed = 1;
        n++;
        new_head = cur->next;
        if (!new_head) {
            break;
        }
        cur = new_head;
    }
    if (mngr->head != new_head) {
        buf_list_t *tail = new_head ? new_head->pre : cur;
        add_to_free_list(mngr, head, tail);
        mngr->head = new_head;
        if (new_head) {
            new_head->pre = NULL;
            mngr->fill_size -= remove_size;
            mngr->min_tsn = new_head->tsn;
        } else {
            mngr->fill_size = 0;
            mngr->min_tsn = mngr->max_tsn + 1;
            mngr->tail = NULL;
            mngr->head = NULL;
        }
    }
    return n;
}

static int get_ack_idx(int start_tsn, int end_tsn)
{
    if (start_tsn < end_tsn) {
        return end_tsn - start_tsn;
    }
    return UINT32_MAX - start_tsn + end_tsn + 1;
}

buf_mngr_t *buf_mngr_create(int limit_size, int timeout)
{
    buf_mngr_t *mngr = media_lib_calloc(1, sizeof(buf_mngr_t));
    if (mngr) {
        media_lib_mutex_create(&mngr->lock);
        if (!mngr->lock) {
            media_lib_free(mngr);
            return NULL;
        }
        mngr->limit_size = limit_size;
        mngr->timeout = timeout;
    }
    return mngr;
}

void buf_mngr_set_send_mode(buf_mngr_t *mngr, bool send_mode) { mngr->send_mode = send_mode; }
void buf_mngr_set_forward_mode(buf_mngr_t *mngr, uint32_t last_ack) { mngr->need_forward = 1; mngr->last_ack = last_ack; }

bool buf_mngr_get_head_info(buf_mngr_t *mngr, buf_info_t *buf_info)
{
    LOCK(mngr);
    buf_list_t *head = mngr->head;
    bool found = false;
    if (head) {
        set_buf_info(buf_info, head);
        found = true;
    }
    UNLOCK(mngr);
    return found;
}

bool buf_mngr_get_next_nack_info(buf_mngr_t *mngr, buf_info_t *buf_info)
{
    LOCK(mngr);
    buf_list_t *head = buf_info->buf_inst;
    bool found = false;
    if (head) {
        head = head->next;
        while (head) {
            if (head->used && !head->abandon) {
                set_buf_info(buf_info, head);
                found = true;
            }
            head = head->next;
        }
    }
    UNLOCK(mngr);
    return found;
}

bool buf_mngr_get_head_tsn(buf_mngr_t *mngr, uint32_t *tsn, uint32_t *packet_id)
{
    LOCK(mngr);
    buf_list_t *head = mngr->head;
    bool found = false;
    if (head) {
        *tsn = head->tsn;
        if (packet_id) {
            *packet_id = head->packet_id;
        }
        found = true;
    }
    UNLOCK(mngr);
    return found;
}

int buf_mngr_update_send_time(void *buf_inst, uint32_t send_time)
{
    buf_list_t *item = buf_inst;
    item->last_send_time = send_time;
    item->resend_count++;
    return 0;
}

bool buf_mngr_get_buffer(buf_mngr_t *mngr, uint32_t tsn, buf_info_t *buf_info)
{
    LOCK(mngr);
    buf_list_t *item = buf_mngr_get(mngr, tsn);
    if (item) {
        set_buf_info(buf_info, item);
    }
    UNLOCK(mngr);
    return item != NULL;
}

void buf_mngr_remove(buf_mngr_t *mngr, void *ptr)
{
    LOCK(mngr);
    buf_list_t *item = ptr;
    item->used = 0;
    remove_bufs(mngr);
    UNLOCK(mngr);
}

static void mark_all_abandon(buf_mngr_t *mngr, buf_list_t *item)
{
    (void)mngr;
    if (!item->used) {
        if (item->is_end) {
            item->abandon = 1;
        }
        return;
    }
    uint16_t stream_id = item->stream_id;
    while (item) {
        if (item->stream_id == stream_id) {
            item->used = 0;
            if (item->is_end) {
                item->abandon = 1;
                break;
            }
        }
        item = item->next;
    }
}

void buf_mngr_remove_for_abandon(buf_mngr_t *mngr, void *ptr)
{
    LOCK(mngr);
    buf_list_t *item = ptr;
    mark_all_abandon(mngr, item);
    remove_bufs(mngr);
    UNLOCK(mngr);
}

void buf_mngr_remove_for_abandon_by_tsn(buf_mngr_t *mngr, uint32_t tsn)
{
    LOCK(mngr);
    buf_list_t *head = mngr->head;
    while (head) {
        if (head->tsn == tsn) {
            mark_all_abandon(mngr, head);
            break;
        }
        head = head->next;
    }
    remove_bufs(mngr);
    UNLOCK(mngr);
}

void buf_mngr_remove_by_stream(buf_mngr_t *mngr, uint16_t stream_id)
{
    LOCK(mngr);
    buf_list_t *head = mngr->head;
    while (head) {
        if (head->stream_id == stream_id) {
            head->used = 0;
        }
        head = head->next;
    }
    remove_bufs(mngr);
    UNLOCK(mngr);
}

void buf_mngr_remove_range(buf_mngr_t *mngr, uint32_t start_tsn, uint32_t end_tsn)
{
    LOCK(mngr);
    buf_list_t *head = mngr->head;
    while (head) {
        if (TSN_LT(head->tsn, start_tsn)) {
        } else if (TSN_LT(end_tsn, head->tsn)) {
            break;
        } else {
            head->used = 0;
        }
        head = head->next;
    }
    remove_bufs(mngr);
    UNLOCK(mngr);
}

bool buf_mngr_check_remove_range_by_stream(buf_mngr_t *mngr, bool check_reliable, uint32_t start_tsn, uint32_t end_tsn)
{
    (void)check_reliable;
    LOCK(mngr);
    buf_list_t *head = mngr->head;
    uint16_t stream_id = 0;
    while (head) {
        if (TSN_LT(head->tsn, start_tsn)) {
        } else if (TSN_LT(end_tsn, head->tsn)) {
            break;
        } else {
            if (head->tsn == start_tsn) {
                stream_id = head->stream_id;
            }
            if (head->stream_id == stream_id) {
                head->used = 0;
            }
        }
        head = head->next;
    }
    remove_bufs(mngr);
    UNLOCK(mngr);
    return true;
}

bool buf_mngr_try_remove_range_by_stream(buf_mngr_t *mngr, uint32_t start_tsn, uint32_t end_tsn)
{ return buf_mngr_check_remove_range_by_stream(mngr, true, start_tsn, end_tsn); }
void buf_mngr_remove_range_by_stream(buf_mngr_t *mngr, uint32_t start_tsn, uint32_t end_tsn)
{ buf_mngr_check_remove_range_by_stream(mngr, false, start_tsn, end_tsn); }

int buf_mngr_check_overdue(buf_mngr_t *mngr)
{
    LOCK(mngr);
    int n = remove_bufs(mngr);
    UNLOCK(mngr);
    return n;
}

int buf_mngr_check_forward(buf_mngr_t *mngr, bool *need_forward, buf_abandon_info_t *info)
{
    LOCK(mngr);
    int n = remove_bufs(mngr);
    if (mngr->has_abandon) {
        *need_forward = true;
        *info = mngr->abandon_info;
        LOG_INFO("Set forward tsn %lu", info->abandon_tsn);
    }
    mngr->has_abandon = 0;
    UNLOCK(mngr);
    return n;
}

uint32_t buf_mngr_get_filled(buf_mngr_t *mngr)
{
    LOCK(mngr);
    uint32_t filled = mngr->fill_size;
    UNLOCK(mngr);
    return filled;
}

uint32_t buf_mngr_get_avail(buf_mngr_t *mngr)
{
    LOCK(mngr);
    uint32_t avail = mngr->limit_size - mngr->fill_size;
    UNLOCK(mngr);
    return avail;
}

int buf_mngr_insert_abandon(buf_mngr_t *mngr, buf_list_t *pre, uint32_t start_tsn, uint32_t end_tsn)
{
    end_tsn++;
    buf_list_t *next = pre ? pre->next : mngr->head;
    buf_list_t *cur = NULL;
    for (; TSN_LT(start_tsn, end_tsn); start_tsn++) {
        cur = allocate_buf(mngr, 0);
        if (!cur) {
            return -2;
        }
        cur->tsn = start_tsn;
        cur->used = 0;
        cur->abandon = 1;
        cur->next = NULL;
        if (pre) {
            pre->next = cur;
            cur->pre = pre;
        } else {
            cur->pre = NULL;
            mngr->head = cur;
            if (!next) {
                mngr->tail = cur;
            }
        }
        pre = cur;
    }
    if (cur) {
        cur->next = next;
        if (next) {
            next->pre = cur;
        } else {
            mngr->tail = cur;
        }
    }
    return 0;
}

int buf_mngr_abandon(buf_mngr_t *mngr, uint32_t acked_tsn, uint32_t forward_tsn)
{
    LOCK(mngr);
    buf_list_t *head = mngr->head;
    if (!head) {
        mngr->last_ack = forward_tsn;
    } else {
        if (TSN_LT(acked_tsn, head->tsn)) {
            mngr->last_ack = head->tsn - 1;
        }
        uint32_t end_tsn = forward_tsn + 1;
        uint32_t pre_tsn = head->tsn - 1;
        while (1) {
            if (TSN_LT(head->tsn, end_tsn)) {
                if (!head->reliable) {
                    head->used = 0;
                    head->abandon = 1;
                }
                if (head->tsn != pre_tsn + 1) {
                    int ret = buf_mngr_insert_abandon(mngr, head->pre, pre_tsn + 1, head->tsn - 1);
                    if (ret) {
                        return ret;
                    }
                }
            } else {
                head = head->pre;
                break;
            }
            if (!head->next) {
                break;
            }
            pre_tsn = head->tsn;
            head = head->next;
        }
        if (head && TSN_LT(head->tsn, end_tsn)) {
            int ret = buf_mngr_insert_abandon(mngr, head, head->tsn + 1, forward_tsn);
            if (ret) {
                return ret;
            }
        }
        if (TSN_LT(mngr->max_tsn, forward_tsn)) {
            mngr->max_tsn = forward_tsn;
        }
    }
    remove_bufs(mngr);
    UNLOCK(mngr);
    return 0;
}

bool buf_mngr_has_ongoing(buf_mngr_t *mngr, uint16_t stream_id)
{
    LOCK(mngr);
    buf_list_t *head = mngr->head;
    bool found = false;
    while (head) {
        if (head->stream_id == stream_id) {
            found = true;
            break;
        }
        head = head->next;
    }
    UNLOCK(mngr);
    return found;
}

int buf_mngr_add_reliable(buf_mngr_t *mngr, bool reliable, bool is_end, uint32_t tsn,
                          uint16_t stream_id, uint32_t packet_id, uint8_t *buffer, int size)
{
    LOCK(mngr);
    if (buf_mngr_get(mngr, tsn)) {
        UNLOCK(mngr);
        return 1;
    }
    if (mngr->fill_size + size >= mngr->limit_size) {
        UNLOCK(mngr);
        return -2;
    }
    buf_list_t *item = allocate_buf(mngr, size);
    if (!item) {
        UNLOCK(mngr);
        return -2;
    }
    if (size) {
        memcpy(item->buffer, buffer, size);
    }
    item->reliable = reliable;
    item->is_end = is_end;
    item->abandon = 0;
    item->add_time = esp_timer_get_time() / 1000;
    item->resend_count = 0;
    item->last_send_time = item->add_time;
    item->size = size;
    item->stream_id = stream_id;
    item->tsn = tsn;
    item->packet_id = packet_id;
    item->used = 1;
    mngr->filled = 1;
    mngr->fill_size += item->alloc_size;
    if (!mngr->tail) {
        mngr->tail = item;
        mngr->head = item;
        mngr->max_tsn = tsn;
        mngr->min_tsn = tsn;
        item->next = NULL;
        item->pre = NULL;
        mngr->fill_size = item->alloc_size;
        UNLOCK(mngr);
        return 0;
    }
    if (TSN_LT(mngr->max_tsn, tsn)) {
        mngr->max_tsn = tsn;
        mngr->tail->next = item;
        item->pre = mngr->tail;
        item->next = NULL;
        mngr->tail = item;
    } else if (TSN_LT(tsn, mngr->min_tsn)) {
        mngr->min_tsn = tsn;
        mngr->head->pre = item;
        item->add_time = mngr->head->add_time;
        item->next = mngr->head;
        item->pre = NULL;
        mngr->head = item;
    } else {
        buf_list_t *head = mngr->head;
        while (head) {
            if (TSN_LT(head->tsn, tsn)) {
                head = head->next;
                continue;
            }
            item->add_time = head->add_time;
            item->next = head;
            if (head->pre) {
                head->pre->next = item;
                item->pre = head->pre;
            } else {
                mngr->head = item;
                item->pre = NULL;
            }
            head->pre = item;
            break;
        }
    }
    UNLOCK(mngr);
    return 0;
}

int buf_mngr_add(buf_mngr_t *mngr, uint32_t tsn, uint16_t stream_id, uint32_t packet_id, uint8_t *buffer, int size)
{ return buf_mngr_add_reliable(mngr, false, false, tsn, stream_id, packet_id, buffer, size); }

uint32_t buf_mngr_get_ack_tsn(buf_mngr_t *mngr, uint32_t last_ack)
{
    LOCK(mngr);
    buf_list_t *head = mngr->head;
    uint32_t ack_tsn = last_ack;
    if (mngr->removed && TSN_LT(ack_tsn, mngr->remove_tsn)) {
        ack_tsn = mngr->remove_tsn;
    }
    while (head) {
        if (TSN_LT(ack_tsn, head->tsn)) {
            if (head->tsn != ack_tsn + 1) {
                break;
            }
            ack_tsn++;
            head = head->next;
        } else {
            head = head->next;
        }
    }
    UNLOCK(mngr);
    return ack_tsn;
}

bool buf_mngr_is_frame_complete(buf_mngr_t *mngr, uint32_t start_tsn, uint32_t end_tsn, uint32_t *frame_size)
{
    LOCK(mngr);
    buf_list_t *head = mngr->head;
    uint32_t size = 0;
    uint16_t stream_id = 0;
    uint32_t pre_tsn = 0;
    bool valid = false;
    bool head_found = false;
    bool abandon = false;
    while (head) {
        if (TSN_LT(head->tsn, start_tsn)) {
        } else if (TSN_LT(end_tsn, head->tsn)) {
            break;
        } else {
            if (head->tsn == start_tsn) {
                if (!head->used) {
                    break;
                }
                stream_id = head->stream_id;
                head_found = true;
            } else if (head->tsn != pre_tsn + 1) {
                break;
            }
            if (head->stream_id == stream_id) {
                size += head->size;
                if (head->abandon) {
                    abandon = true;
                }
            }
            if (head->tsn == end_tsn && head_found) {
                valid = true;
                break;
            }
            pre_tsn = head->tsn;
        }
        head = head->next;
    }
    UNLOCK(mngr);
    *frame_size = abandon ? 0 : size;
    return valid;
}

int buf_mngr_fill_frame(buf_mngr_t *mngr, uint32_t start_tsn, uint32_t end_tsn, uint8_t *buffer, int size)
{
    LOCK(mngr);
    uint32_t fill_size = 0;
    buf_list_t *head = mngr->head;
    uint16_t stream_id = 0;
    while (head) {
        if (TSN_LT(head->tsn, start_tsn)) {
        } else if (TSN_LT(end_tsn, head->tsn)) {
            break;
        } else {
            if (head->tsn == start_tsn) {
                stream_id = head->stream_id;
            }
            if (head->stream_id == stream_id) {
                if (!head->used || head->abandon) {
                    fill_size = -1;
                    break;
                }
                if (head->size) {
                    if (fill_size + head->size > size) {
                        fill_size = -1;
                        break;
                    }
                    memcpy(buffer + fill_size, head->buffer, head->size);
                }
                fill_size += head->size;
            }
        }
        head = head->next;
    }
    UNLOCK(mngr);
    return fill_size;
}

/* lwIP's conversion keeps the left shift at the original argument width. */
#define net16(x) __builtin_bswap16(x)

int buf_mngr_get_ack_data(buf_mngr_t *mngr, uint32_t *ack_end, uint16_t *ack_block, int n)
{
    LOCK(mngr);
    buf_list_t *head = mngr->head;
    if (!head) {
        if (mngr->filled) {
            if (!mngr->need_forward) {
                *ack_end = mngr->max_tsn;
            }
        }
        UNLOCK(mngr);
        return 0;
    }
    uint32_t ack_tsn = *ack_end;
    if (mngr->removed && TSN_LT(ack_tsn, mngr->remove_tsn)) {
        ack_tsn = mngr->remove_tsn;
    }
    while (head) {
        if (TSN_LT(ack_tsn, head->tsn)) {
            if (head->tsn != ack_tsn + 1) {
                break;
            }
            ack_tsn++;
            head = head->next;
        } else {
            head = head->next;
        }
    }
    *ack_end = ack_tsn;
    if (!head) {
        UNLOCK(mngr);
        return 0;
    }
    int fill = 0;
    uint32_t expect_tsn = head->tsn;
    uint32_t start_tsn = expect_tsn;
    while (head) {
        while (head && head->tsn == expect_tsn) {
            expect_tsn++;
            head = head->next;
        }
        *ack_block++ = net16(get_ack_idx(ack_tsn, start_tsn));
        *ack_block++ = net16(get_ack_idx(ack_tsn, expect_tsn - 1));
        fill++;
        if (fill >= n) {
            break;
        }
        if (head) {
            expect_tsn = head->tsn;
            start_tsn = head->tsn;
        } else {
            break;
        }
    }
    UNLOCK(mngr);
    return fill;
}

uint32_t buf_mngr_get_next_tsn(buf_mngr_t *mngr, uint32_t tsn)
{
    uint32_t next_tsn = tsn;
    LOCK(mngr);
    buf_list_t *head = mngr->head;
    uint16_t stream_id = 0;
    bool found = false;
    while (head) {
        if (head->tsn == tsn) {
            stream_id = head->stream_id;
            found = true;
        }
        if (TSN_LT(tsn, head->tsn) && found && head->stream_id == stream_id && head->used) {
            next_tsn = head->tsn;
            break;
        }
        head = head->next;
    }
    UNLOCK(mngr);
    return next_tsn;
}

uint32_t buf_mngr_get_pre_tsn(buf_mngr_t *mngr, uint32_t tsn)
{
    uint32_t pre_tsn = tsn;
    LOCK(mngr);
    buf_list_t *head = mngr->head;
    bool found = false;
    uint16_t stream_id = 0;
    while (head) {
        if (found && head->stream_id == stream_id && head->used) {
            pre_tsn = head->tsn;
            break;
        }
        if (head->tsn == tsn) {
            stream_id = head->stream_id;
            found = true;
        }
        head = found ? head->pre : head->next;
    }
    UNLOCK(mngr);
    return pre_tsn;
}

int buf_mngr_get_nack_data(buf_mngr_t *mngr, uint32_t *nack_entry, int n)
{
    LOCK(mngr);
    buf_list_t *head = mngr->head;
    if (!head) {
        UNLOCK(mngr);
        return 0;
    }
    int fill = 0;
    uint32_t ack_tsn = head->tsn - 1;
    while (head) {
        if (head->tsn == ++ack_tsn) {
            head = head->next;
        } else {
            while (TSN_LT(ack_tsn, head->tsn) && fill < n) {
                *nack_entry = 0;
                *(uint16_t *)nack_entry = net16(ack_tsn);
                fill++;
                LOG_INFO("L: %lu", (uint32_t)(uint16_t)ack_tsn);
                ack_tsn++;
            }
            break;
        }
    }
    UNLOCK(mngr);
    return fill;
}

void buf_mngr_print(buf_mngr_t *mngr)
{
    LOCK(mngr);
    buf_list_t *cur = mngr->head;
    int n = 0;
    while (cur) {
        printf("%lu[%d:%d-%d]->", cur->tsn, cur->size, cur->used, cur->stream_id);
        cur = cur->next;
        if (n > 1000) {
            *(volatile int *)0 = 0;
        }
        n++;
    }
    printf("\n");
    UNLOCK(mngr);
}

bool buf_mngr_is_expected(buf_mngr_t *mngr, uint32_t *arr, int num)
{
    LOCK(mngr);
    int n = 0;
    buf_list_t *cur = mngr->head;
    while (cur) {
        if (n == num) {
            return false;
        }
        if (cur->tsn != arr[n]) {
            return false;
        }
        cur = cur->next;
        n++;
    }
    UNLOCK(mngr);
    return n == num;
}

void buf_mngr_destroy(buf_mngr_t *mngr)
{
    if (!mngr) {
        return;
    }
    if (mngr->free_list) {
        if (mngr->tail) {
            mngr->tail->next = mngr->free_list;
        } else {
            mngr->head = mngr->free_list;
        }
    }
    buf_list_t *cur = mngr->head;
    while (cur) {
        buf_list_t *next = cur->next;
        if (cur->buffer) {
            media_lib_free(cur->buffer);
        }
        media_lib_free(cur);
        cur = next;
    }
    if (mngr->lock) {
        media_lib_mutex_destroy(mngr->lock);
        mngr->lock = NULL;
    }
    media_lib_free(mngr);
}

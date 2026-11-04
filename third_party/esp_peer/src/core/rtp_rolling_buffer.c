/* rtp rolling buffer implementation.
 * See LICENSE and LICENSES/ for copyright and licensing notices.
 */
#include "peer_platform.h"
#include "rtp_rolling_buffer.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#define WAIT_FOREVER portMAX_DELAY
#define take(sem) xSemaphoreTake((sem), WAIT_FOREVER)
#define give(sem) xSemaphoreGive(sem)

typedef struct {
    uint32_t uuid;
    uint8_t *data;
    int size;
    bool using;
    uint16_t resend_count;
    uint8_t meta_data[];
} rolling_packet_info_t;

typedef struct {
    RingbufHandle_t fifo;
    rolling_packet_info_t *packet_info;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t release_sem;
    uint16_t packet_count;
    uint16_t meta_size;
    uint32_t pool_size;
    uint16_t max_resend_limit;
    uint16_t packet_start;
    uint16_t packet_filled;
    bool freed;
} rolling_buff_t;

_Static_assert(sizeof(rolling_packet_info_t) == 16, "vendor packet info size");
_Static_assert(offsetof(rolling_packet_info_t, meta_data) == 16, "vendor metadata offset");
_Static_assert(sizeof(rolling_buff_t) == 32, "vendor rolling state size");
_Static_assert(offsetof(rolling_buff_t, packet_count) == 16, "vendor packet count offset");
_Static_assert(offsetof(rolling_buff_t, packet_filled) == 28, "vendor packet fill offset");
_Static_assert(offsetof(rolling_buff_t, freed) == 30, "vendor freed offset");

static rolling_packet_info_t *get_packet_info(rolling_buff_t *rolling, uint16_t idx)
{
    uint8_t *packet_info = (uint8_t *)rolling->packet_info;
    return (rolling_packet_info_t *)(packet_info + idx * (sizeof(rolling_packet_info_t) + rolling->meta_size));
}

static rolling_packet_info_t *search_packet(rolling_buff_t *rolling, uint32_t uuid, uint16_t *packet_idx)
{
    uint16_t idx = rolling->packet_start;
    for (uint16_t i = 0; i < rolling->packet_filled; i++) {
        rolling_packet_info_t *packet_info = get_packet_info(rolling, idx);
        if (packet_info->uuid == uuid) {
            *packet_idx = idx;
            return packet_info;
        }
        idx++;
        if (idx >= rolling->packet_count) {
            idx -= rolling->packet_count;
        }
    }
    return NULL;
}

rtp_rolling_buf_handle_t rtp_rolling_buffer_create(rtp_rolling_buf_cfg_t *cfg)
{
    rolling_buff_t *rolling = media_lib_calloc(1, sizeof(rolling_buff_t));
    if (!rolling) {
        return NULL;
    }
    do {
        uint16_t packet_count = cfg->packet_count ? cfg->packet_count : 256;
        rolling->packet_info = media_lib_calloc(1, packet_count * (sizeof(rolling_packet_info_t) + cfg->meta_size));
        if (!rolling->packet_info) {
            break;
        }
        uint32_t pool_size = cfg->pool_size ? cfg->pool_size : 400 * 1024;
        rolling->fifo = xRingbufferCreateWithCaps(pool_size, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
        if (!rolling->fifo) {
            rolling->fifo = xRingbufferCreateWithCaps(pool_size, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_INTERNAL);
        }
        if (!rolling->fifo) {
            break;
        }
        rolling->release_sem = xSemaphoreCreateBinary();
        if (!rolling->release_sem) {
            break;
        }
        rolling->lock = xSemaphoreCreateMutex();
        if (!rolling->lock) {
            break;
        }
        rolling->meta_size = cfg->meta_size;
        rolling->packet_count = packet_count;
        rolling->pool_size = pool_size;
        rolling->max_resend_limit = cfg->max_resend_limit ? cfg->max_resend_limit : 3;
        return rolling;
    } while (0);
    rtp_rolling_buffer_destroy(rolling);
    return NULL;
}

int rtp_rolling_buffer_insert(rtp_rolling_buf_handle_t handle, uint32_t uuid,
                              uint8_t *data, size_t size, uint8_t *meta_data)
{
    rolling_buff_t *rolling = handle;
    if (size > rolling->pool_size) {
        esp_log((esp_log_config_t){ .data = 1 }, "ROLLING_BUF",
                "E (%lu) %s: Size over pool %d > %d\n", esp_log_timestamp(),
                "ROLLING_BUF", size, rolling->pool_size);
        return -1;
    }
    take(rolling->lock);
    bool queue_enough = rolling->packet_filled < rolling->packet_count;
    bool fifo_enough = xRingbufferGetCurFreeSize(rolling->fifo) >= size;
    int ret = -1;
    while (rolling->packet_filled && (!queue_enough || !fifo_enough)) {
        uint16_t rp = rolling->packet_start;
        rolling_packet_info_t *packet_info = get_packet_info(rolling, rp);
        if (packet_info->using) {
            give(rolling->lock);
            take(rolling->release_sem);
            if (rolling->freed) {
                return -1;
            }
            take(rolling->lock);
        }
        size_t item_size;
        void *item = xRingbufferReceive(rolling->fifo, &item_size, WAIT_FOREVER);
        if (item) {
            vRingbufferReturnItem(rolling->fifo, item);
        }
        rolling->packet_start = (rp + 1) % rolling->packet_count;
        rolling->packet_filled--;
        fifo_enough = xRingbufferGetCurFreeSize(rolling->fifo) >= size;
        queue_enough = true;
    }
    if (queue_enough && fifo_enough) {
        void *item = NULL;
        xRingbufferSendAcquire(rolling->fifo, &item, size, WAIT_FOREVER);
        if (item) {
            memcpy(item, data, size);
            uint16_t wp = (rolling->packet_start + rolling->packet_filled) % rolling->packet_count;
            rolling_packet_info_t *packet_info = get_packet_info(rolling, wp);
            packet_info->uuid = uuid;
            if (meta_data && rolling->meta_size) {
                memcpy(packet_info->meta_data, meta_data, rolling->meta_size);
            }
            rolling->packet_filled++;
            xRingbufferSendComplete(rolling->fifo, item);
            packet_info->data = item;
            packet_info->size = size;
            packet_info->using = false;
            packet_info->resend_count = 0;
            ret = 0;
        }
    }
    give(rolling->lock);
    return ret;
}

int rtp_rolling_buffer_acquire(rtp_rolling_buf_handle_t handle, uint32_t uuid,
                               rtp_rolling_buf_pkt_t *pkt)
{
    rolling_buff_t *rolling = handle;
    take(rolling->lock);
    int ret = -1;
    do {
        if (rolling->packet_filled == 0) {
            break;
        }
        rolling_packet_info_t *packet_info = search_packet(rolling, uuid, &pkt->packet_idx);
        if (!packet_info) {
            break;
        }
        if (packet_info->resend_count >= rolling->max_resend_limit) {
            break;
        }
        packet_info->using = true;
        packet_info->resend_count++;
        pkt->data = packet_info->data;
        pkt->size = packet_info->size;
        pkt->meta_data = packet_info->meta_data;
        ret = 0;
    } while (0);
    give(rolling->lock);
    return ret;
}

int rtp_rolling_buffer_release(rtp_rolling_buf_handle_t handle, rtp_rolling_buf_pkt_t *pkt)
{
    rolling_buff_t *rolling = handle;
    take(rolling->lock);
    if (pkt->packet_idx >= rolling->packet_count) {
        give(rolling->lock);
        return -1;
    }
    rolling_packet_info_t *packet_info = get_packet_info(rolling, pkt->packet_idx);
    if (packet_info->using) {
        packet_info->using = false;
        give(rolling->release_sem);
    }
    give(rolling->lock);
    return 0;
}

void rtp_rolling_buffer_destroy(rtp_rolling_buf_handle_t handle)
{
    rolling_buff_t *rolling = handle;
    rolling->freed = true;
    if (rolling->release_sem) {
        give(rolling->release_sem);
    }
    if (rolling->lock) {
        take(rolling->lock);
        give(rolling->lock);
        vSemaphoreDelete(rolling->lock);
    }
    if (rolling->release_sem) {
        vSemaphoreDelete(rolling->release_sem);
    }
    if (rolling->packet_info) {
        media_lib_free(rolling->packet_info);
        rolling->packet_info = NULL;
    }
    if (rolling->fifo) {
        vRingbufferDeleteWithCaps(rolling->fifo);
        rolling->fifo = NULL;
    }
    media_lib_free(rolling);
}

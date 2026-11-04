#include "jetkvm_console.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_serial_output.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"

/* The UART drains 11.5 KB/s; 16 KiB absorbs a one-second log burst. */
#define CONSOLE_QUEUE_BYTES       (16U * 1024U)
#define CONSOLE_TASK_STACK        3072U
#define CONSOLE_TASK_PRIORITY     (tskIDLE_PRIORITY + 1U)
#define CONSOLE_DROP_REPORT_US    1000000
#define CONSOLE_FLUSH_DEADLINE_US 500000

static RingbufHandle_t s_queue;
static vprintf_like_t s_direct;
static size_t s_max_item;
static uint32_t s_dropped;
/* Messages reserved in the queue and messages written; equal when idle. */
static uint32_t s_reserved;
static uint32_t s_written;
static int64_t s_last_drop_report_us;

static int queue_vprintf(const char *format, va_list args)
{
    va_list measure;
    va_copy(measure, args);
    int length = vsnprintf(NULL, 0, format, measure);
    va_end(measure);
    if (length <= 0) {
        return length;
    }
    /* A message longer than an item is cut, never printed the blocking way. */
    size_t size = (size_t)length + 1U;
    if (size > s_max_item) {
        size = s_max_item;
    }
    void *item = NULL;
    __atomic_fetch_add(&s_reserved, 1U, __ATOMIC_ACQ_REL);
    if (xRingbufferSendAcquire(s_queue, &item, size, 0) != pdTRUE) {
        __atomic_fetch_sub(&s_reserved, 1U, __ATOMIC_ACQ_REL);
        __atomic_fetch_add(&s_dropped, 1U, __ATOMIC_RELAXED);
        return length;
    }
    char *text = item;
    vsnprintf(text, size, format, args);
    if ((size_t)length + 1U > size && size >= 2U) {
        text[size - 2U] = '\n';
    }
    xRingbufferSendComplete(s_queue, item);
    return length;
}

static void report_drops(void)
{
    int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_drop_report_us < CONSOLE_DROP_REPORT_US) {
        return;
    }
    uint32_t dropped = __atomic_exchange_n(&s_dropped, 0U, __ATOMIC_RELAXED);
    if (dropped != 0U) {
        s_last_drop_report_us = now_us;
        printf("W (%" PRIu32 ") console: %" PRIu32 " log messages dropped; the console was busy\n",
               esp_log_timestamp(), dropped);
    }
}

/* Writes one queued message; the console task is the only reader. */
static void write_next(TickType_t wait)
{
    size_t size = 0;
    char *item = xRingbufferReceive(s_queue, &size, wait);
    if (item == NULL) {
        return;
    }
    fwrite(item, 1, strnlen(item, size), stdout);
    vRingbufferReturnItem(s_queue, item);
    report_drops();
    fflush(stdout);
    __atomic_fetch_add(&s_written, 1U, __ATOMIC_ACQ_REL);
}

static void console_task(void *arg)
{
    (void)arg;
    for (;;) {
        write_next(portMAX_DELAY);
    }
}

esp_err_t jetkvm_console_start(void)
{
    if (s_queue != NULL) {
        return ESP_OK;
    }
    s_queue = xRingbufferCreateWithCaps(CONSOLE_QUEUE_BYTES, RINGBUF_TYPE_NOSPLIT,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_max_item = xRingbufferGetMaxItemSize(s_queue);
    if (xTaskCreatePinnedToCoreWithCaps(console_task, "console", CONSOLE_TASK_STACK, NULL,
                                        CONSOLE_TASK_PRIORITY, NULL, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vRingbufferDeleteWithCaps(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_direct = esp_log_set_vprintf(queue_vprintf);
    return ESP_OK;
}

void jetkvm_console_flush(void)
{
    if (s_queue == NULL || s_direct == NULL) {
        return;
    }
    /* Later messages go straight to the console; the console task writes the
     * queued ones in order. Wait for it within a deadline: a writer stopped
     * between reserving and completing its message would block the queue. */
    esp_log_set_vprintf(s_direct);
    int64_t deadline_us = esp_timer_get_time() + CONSOLE_FLUSH_DEADLINE_US;
    while (__atomic_load_n(&s_written, __ATOMIC_ACQUIRE) !=
               __atomic_load_n(&s_reserved, __ATOMIC_ACQUIRE) &&
           esp_timer_get_time() < deadline_us) {
        vTaskDelay(1);
    }
    s_last_drop_report_us = 0;
    report_drops();
    fflush(stdout);
    /* The restart resets the chip directly (main.c), so let the UART finish. */
    esp_rom_output_tx_wait_idle(CONFIG_ESP_CONSOLE_ROM_SERIAL_PORT_NUM);
}

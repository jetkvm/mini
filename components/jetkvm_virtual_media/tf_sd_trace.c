/* SPDX-License-Identifier: MIT */

#include "tf_sd_trace.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/rmt_rx.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef JETKVM_TF_RMT_TRACE
#define JETKVM_TF_RMT_TRACE 0
#endif

#if JETKVM_TF_RMT_TRACE

#define TRACE_RESOLUTION_HZ 1000000U
#define TRACE_CLK_GPIO GPIO_NUM_43
#define TRACE_CMD_GPIO GPIO_NUM_44
#define TRACE_CLK_SYMBOLS 4096U
#define TRACE_CMD_SYMBOLS 1024U

typedef struct {
    volatile size_t symbols;
    volatile bool done;
} trace_result_t;

typedef struct {
    rmt_channel_handle_t clk_channel;
    rmt_channel_handle_t cmd_channel;
    rmt_symbol_word_t *clk_symbols;
    rmt_symbol_word_t *cmd_symbols;
    trace_result_t clk_result;
    trace_result_t cmd_result;
    bool started;
} trace_context_t;

static const char *TAG = "tf_sd_trace";
static trace_context_t s_trace;

static bool IRAM_ATTR trace_done(rmt_channel_handle_t channel,
                                 const rmt_rx_done_event_data_t *event,
                                 void *user_data)
{
    (void)channel;
    trace_result_t *result = user_data;
    result->symbols = event->num_symbols;
    result->done = event->flags.is_last;
    return false;
}

static void destroy_channel(rmt_channel_handle_t *channel)
{
    if (*channel != NULL) {
        (void)rmt_disable(*channel);
        (void)rmt_del_channel(*channel);
        *channel = NULL;
    }
}

static void cleanup(void)
{
    destroy_channel(&s_trace.clk_channel);
    destroy_channel(&s_trace.cmd_channel);
    heap_caps_free(s_trace.clk_symbols);
    heap_caps_free(s_trace.cmd_symbols);
    memset(&s_trace, 0, sizeof(s_trace));
}

static esp_err_t create_channel(gpio_num_t gpio, size_t memory_symbols,
                                bool dma, trace_result_t *result,
                                rmt_channel_handle_t *channel)
{
    rmt_rx_channel_config_t config = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = TRACE_RESOLUTION_HZ,
        .mem_block_symbols = memory_symbols,
        .flags.with_dma = dma,
    };
    esp_err_t error = rmt_new_rx_channel(&config, channel);
    if (error != ESP_OK) {
        return error;
    }
    const rmt_rx_event_callbacks_t callbacks = {
        .on_recv_done = trace_done,
    };
    error = rmt_rx_register_event_callbacks(*channel, &callbacks, result);
    if (error == ESP_OK) {
        error = rmt_enable(*channel);
    }
    return error;
}

esp_err_t tf_sd_trace_start(void)
{
    cleanup();
    s_trace.clk_symbols = heap_caps_aligned_alloc(
        64, sizeof(rmt_symbol_word_t) * TRACE_CLK_SYMBOLS,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_trace.cmd_symbols = heap_caps_aligned_alloc(
        64, sizeof(rmt_symbol_word_t) * TRACE_CMD_SYMBOLS,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_trace.clk_symbols == NULL || s_trace.cmd_symbols == NULL) {
        cleanup();
        return ESP_ERR_NO_MEM;
    }
    memset(s_trace.clk_symbols, 0, sizeof(rmt_symbol_word_t) * TRACE_CLK_SYMBOLS);
    memset(s_trace.cmd_symbols, 0, sizeof(rmt_symbol_word_t) * TRACE_CMD_SYMBOLS);

    /* Use the driver's non-DMA ping-pong path for both inputs. P4 GDMA does
     * not reliably write this capture directly into external PSRAM. */
    esp_err_t error = create_channel(TRACE_CMD_GPIO, 96, false,
                                     &s_trace.cmd_result,
                                     &s_trace.cmd_channel);
    if (error == ESP_OK) {
        error = create_channel(TRACE_CLK_GPIO, 96, false,
                               &s_trace.clk_result,
                               &s_trace.clk_channel);
    }
    const rmt_receive_config_t receive = {
        .signal_range_min_ns = 500,
        /* Slot setup takes a little over 10 ms before the first command. Keep
         * the receiver armed across that quiet setup interval. At 1 MHz this
         * remains below the RMT symbol duration limit. */
        .signal_range_max_ns = 30000000,
    };
    if (error == ESP_OK) {
        error = rmt_receive(s_trace.cmd_channel, s_trace.cmd_symbols,
                            sizeof(rmt_symbol_word_t) * TRACE_CMD_SYMBOLS,
                            &receive);
    }
    if (error == ESP_OK) {
        error = rmt_receive(s_trace.clk_channel, s_trace.clk_symbols,
                            sizeof(rmt_symbol_word_t) * TRACE_CLK_SYMBOLS,
                            &receive);
    }
    if (error != ESP_OK) {
        cleanup();
        return error;
    }
    s_trace.started = true;
    ESP_LOGI(TAG, "passive RMT RX armed CLK43/CMD44 resolution=%u Hz",
             TRACE_RESOLUTION_HZ);
    return ESP_OK;
}

static void dump_signal(const char *name, const rmt_symbol_word_t *symbols,
                        size_t count, bool done)
{
    printf("TF_TRACE_META signal=%s resolution_hz=%u symbols=%u done=%u\n",
           name, TRACE_RESOLUTION_HZ, (unsigned)count, done ? 1U : 0U);
    for (size_t i = 0; i < count; i += 8U) {
        printf("TF_TRACE_RAW %s %04u", name, (unsigned)i);
        size_t end = i + 8U < count ? i + 8U : count;
        for (size_t j = i; j < end; ++j) {
            printf(" %08" PRIx32, symbols[j].val);
        }
        putchar('\n');
    }
}

void tf_sd_trace_dump_and_stop(void)
{
    if (!s_trace.started) {
        return;
    }
    /* ACMD41 failure leaves CMD high, allowing its 30 ms idle threshold to
     * terminate the capture and publish the final symbol count. */
    vTaskDelay(pdMS_TO_TICKS(40));
    dump_signal("CLK", s_trace.clk_symbols, s_trace.clk_result.symbols,
                s_trace.clk_result.done);
    dump_signal("CMD", s_trace.cmd_symbols, s_trace.cmd_result.symbols,
                s_trace.cmd_result.done);
    cleanup();
}

#else

esp_err_t tf_sd_trace_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void tf_sd_trace_dump_and_stop(void)
{
}

#endif

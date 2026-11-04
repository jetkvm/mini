#!/usr/bin/env python3
"""Exercise the vendor frame timeout and actual owner-side DMA abort sequence.

Extract the production completion-wait branch, abort helper and DMA quiesce
functions unchanged. Timer/semaphore/register providers model no FRAME_DONE,
bitstream exhaustion and a late semaphore signal at several RTOS tick rates.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
VENDOR = ROOT / "managed_components/espressif__esp_h264"
encoder = (VENDOR / "hw/src/esp_h264_enc_single_hw.c").read_text()
header = (VENDOR / "hw/src/esp_h264_enc_hw_param.h").read_text()
dma = (VENDOR / "hw/hal/esp32p4/h264_dma_hal.c").read_text()


def block(source, marker):
    start = source.index(marker)
    pos = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[start:pos]


timeout_define = re.search(r"^#define H264_TIME_OUT\s+[^\n]+", header, re.M)[0]
frame_wait = block(encoder, "if (esp_h264_mutex_lock(hw_hd->frame_done,")
abort_frame = block(encoder, "static void h264_hw_abort_frame(")
stop_dma = block(dma, "static bool h264_dma_stop_and_reset(")
deinit_dma = block(dma, "void h264_dma_hal_deinit(")
# The converted timeout must remain exclusive to frame completion.
assert encoder.count("H264_TIME_OUT") == 1
assert "h264_hw_abort_frame(hw_hd);" in frame_wait

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#define ESP_H264_ERR_OK 0
#define ESP_H264_ERR_TIMEOUT (-1)
#define ESP_H264_ERR_MEM (-2)
#define ESP_H264_LOGE(...) ((void)0)
#define ESP_ERROR_CHECK(call) assert((call) == 0)
#define pdTRUE 1
#define HAL_CONFIG(name) 300
#define H264_DMA_IN_OUT_CH_NUM 5
#define TAG "test"
typedef uint32_t TickType_t;
typedef int esp_h264_err_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)((uint64_t)(ms) * TEST_TICK_HZ / 1000U))
typedef struct {
    struct { int inlink_stop, outlink_stop; } link_conf;
    struct { int in_cmd_disable, out_cmd_disable, in_rst, out_rst; } conf0;
    struct { int in_reset_avail, out_reset_avail; } state;
} channel_t;
typedef struct {
    channel_t dma_in_ch[5], dma_out_ch[5], dma_in_ch5;
} h264_dma_dev_t;
typedef struct { h264_dma_dev_t *dev; } h264_dma_hal_context_t;
typedef struct { struct { uint32_t val; } int_ena; } h264_dev_t;
typedef struct { h264_dev_t *dev; } h264_hal_context_t;
typedef struct {
    h264_hal_context_t h264_hal;
    h264_dma_hal_context_t dma2d_hal;
    void *intr_hd, *frame_done;
    bool bs_bit_overflow;
} esp_h264_hw_handle_t;
static h264_dev_t core;
static h264_dma_dev_t dma_regs;
static esp_h264_hw_handle_t hardware;
static struct {
    int64_t now_us;
    bool completion, bitstream_full, stuck_dma, irq_enabled, core_reset;
    bool dma_owns_buffers, caller_owns_buffers;
    unsigned waits, pending_signals, reset_counters, dma_irq_clears, core_irq_clears;
    unsigned dma_delays, irq_disables, irq_enables;
    TickType_t timeout_ticks;
} state;
static jmp_buf fatal_jump;

static void assert_dma_stopped(void)
{
    for (unsigned i=0; i<5; ++i) {
        assert(dma_regs.dma_in_ch[i].link_conf.inlink_stop);
        assert(dma_regs.dma_out_ch[i].link_conf.outlink_stop);
        assert(dma_regs.dma_in_ch[i].conf0.in_cmd_disable);
        assert(dma_regs.dma_out_ch[i].conf0.out_cmd_disable == (i<3));
    }
    assert(dma_regs.dma_in_ch5.conf0.in_cmd_disable);
}

static int64_t esp_timer_get_time(void) { return state.now_us; }
static void esp_rom_delay_us(unsigned us)
{
    assert(us == 1 && state.core_reset && !state.irq_enabled);
    assert_dma_stopped();
    /* Advance the fake timer faster than real polling to keep the test small. */
    state.now_us += 1000;
    ++state.dma_delays;
    if (state.dma_delays >= 2 && !state.stuck_dma) {
        for (unsigned i=0; i<5; ++i) {
            dma_regs.dma_in_ch[i].state.in_reset_avail = 1;
            /* TX3/TX4 bits are reserved, not independent reset groups. */
            dma_regs.dma_out_ch[i].state.out_reset_avail = i<3;
        }
        dma_regs.dma_in_ch5.state.in_reset_avail = 1;
        state.dma_owns_buffers = false;
    }
}
static void test_fatal(void)
{
    assert(state.dma_owns_buffers && !state.caller_owns_buffers);
    assert_dma_stopped();
    longjmp(fatal_jump, 1);
}
#define abort test_fatal
'''

PROVIDERS = r'''
static int esp_intr_disable(void *handle)
{
    assert(handle == &hardware && state.irq_enabled);
    state.irq_enabled = false;
    ++state.irq_disables;
    return 0;
}
static int esp_intr_enable(void *handle)
{
    assert(handle == &hardware && !state.irq_enabled);
    assert(!state.dma_owns_buffers && state.pending_signals == 0);
    assert(state.reset_counters == 3 && state.dma_irq_clears == 1);
    assert(state.core_irq_clears == 1 && core.int_ena.val == 0x55);
    state.irq_enabled = true;
    ++state.irq_enables;
    return 0;
}
static void h264_hal_ena_intr(h264_hal_context_t *hal, uint32_t enabled)
{
    assert(hal == &hardware.h264_hal && !state.irq_enabled);
    hal->dev->int_ena.val = enabled;
}
static void h264_hal_reset(h264_hal_context_t *hal)
{
    assert(hal == &hardware.h264_hal && !state.irq_enabled && !core.int_ena.val);
    state.core_reset = true;
}
static void reset_counter(h264_dma_hal_context_t *hal)
{
    assert(hal == &hardware.dma2d_hal && !state.dma_owns_buffers);
    ++state.reset_counters;
}
#define h264_dma_hal_reset_counter_dbtmp reset_counter
#define h264_dma_hal_reset_counter_db reset_counter
#define h264_dma_hal_reset_counter_ref reset_counter
static void h264_dma_hal_clear_intr(h264_dma_hal_context_t *hal)
{
    assert(hal == &hardware.dma2d_hal && !state.dma_owns_buffers);
    ++state.dma_irq_clears;
}
static void h264_hal_clear_intr_status(h264_hal_context_t *hal, uint32_t mask)
{
    assert(hal == &hardware.h264_hal && mask == UINT32_MAX && !state.dma_owns_buffers);
    ++state.core_irq_clears;
}
static uint32_t h264_dma_hal_get_bs_intr(h264_dma_hal_context_t *hal)
{
    assert(hal == &hardware.dma2d_hal && state.waits == 1 && state.dma_owns_buffers);
    return state.bitstream_full ? 1U : 0U;
}
static int esp_h264_mutex_lock(void *semaphore, TickType_t ticks)
{
    assert(semaphore == &state);
    if (ticks == 0) {
        assert(!state.irq_enabled && !state.dma_owns_buffers);
        if (!state.pending_signals) return 0;
        --state.pending_signals;
        return pdTRUE;
    }
    ++state.waits;
    state.timeout_ticks = ticks;
    assert(state.irq_enabled && state.dma_owns_buffers && !state.caller_owns_buffers);
    if (state.completion) {
        state.now_us += 15000;
        state.dma_owns_buffers = false;
        return pdTRUE;
    }
    state.now_us += (int64_t)ticks * 1000000 / TEST_TICK_HZ;
    /* FRAME_DONE races with the expired wait. Abort must retire it after
     * masking the IRQ; it cannot falsely complete the following frame. */
    state.pending_signals = 1;
    return 0;
}
'''

CHECKS = r'''
static void setup(bool completion, bool bitstream_full, bool stuck)
{
    memset(&state, 0, sizeof(state));
    memset(&dma_regs, 0, sizeof(dma_regs));
    core.int_ena.val = 0x55;
    hardware = (esp_h264_hw_handle_t) {
        .h264_hal={.dev=&core}, .dma2d_hal={.dev=&dma_regs},
        .intr_hd=&hardware, .frame_done=&state, .bs_bit_overflow=true,
    };
    state.completion = completion;
    state.bitstream_full = bitstream_full;
    state.stuck_dma = stuck;
    state.irq_enabled = state.dma_owns_buffers = true;
}

static esp_h264_err_t caller_wait(uint32_t *length)
{
    esp_h264_err_t result = production_wait(&hardware, length);
    /* Model the application's ownership transfer only after SDK return. */
    assert(!state.dma_owns_buffers);
    state.caller_owns_buffers = true;
    return result;
}

int main(void)
{
    uint32_t length = 99;
    setup(true, false, false);
    assert(caller_wait(&length) == ESP_H264_ERR_OK);
    assert(state.now_us == 15000 && !state.irq_disables && !state.core_reset);
    assert(state.timeout_ticks == TEST_TICK_HZ);

    for (unsigned full=0; full<2; ++full) {
        setup(false, full, false);
        length = 99;
        assert(caller_wait(&length) == (full ? ESP_H264_ERR_MEM : ESP_H264_ERR_TIMEOUT));
        assert(length == 0 && state.caller_owns_buffers);
        assert(state.now_us == 1002000); /* 1 s frame wait + 2 ms DMA drain. */
        assert(state.now_us < 3000000); /* Application mode-change deadline. */
        assert(state.irq_disables == 1 && state.irq_enables == 1);
        assert(state.pending_signals == 0 && !hardware.bs_bit_overflow);
    }

    setup(false, false, true);
    if (setjmp(fatal_jump) == 0) {
        (void)caller_wait(&length);
        assert(!"live DMA ownership was released after a failed reset");
    }
    assert(state.now_us >= 1020000 && state.now_us < 1030000);
    assert(state.irq_disables == 1 && state.irq_enables == 0);
    assert(!state.caller_owns_buffers && state.dma_owns_buffers);
    printf("H264 frame timeout at %u Hz: 1 s budget, owner-side DMA reset, late signal and retained ownership: PASS\n", TEST_TICK_HZ);
}
'''

with tempfile.TemporaryDirectory(prefix="h264-frame-timeout-") as directory:
    path = Path(directory)
    wait_wrapper = (
        "static esp_h264_err_t production_wait(esp_h264_hw_handle_t *hw_hd, "
        "uint32_t *out_len)\n{\n" + frame_wait + "\nreturn ESP_H264_ERR_OK;\n}\n"
    )
    (path / "test.c").write_text(
        PREFIX + "\n" + timeout_define + "\n" + stop_dma + "\n" + deinit_dma
        + "\n" + PROVIDERS + abort_frame + "\n" + wait_wrapper + CHECKS
    )
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    for tick_hz in (100, 250, 1000):
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
            "-DTEST_TICK_HZ=" + str(tick_hz), str(path / "test.c"),
            "-o", str(path / "test"),
        ], check=True)
        subprocess.run([str(path / "test")], env=env, check=True)

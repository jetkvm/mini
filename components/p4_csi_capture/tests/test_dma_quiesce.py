#!/usr/bin/env python3
"""Run actual CSI stop/delete helpers against delayed DMA and ISR completion."""

from pathlib import Path
import os
import re
import subprocess
import tempfile


source = (Path(__file__).resolve().parents[1] / "p4_cam_ctlr_csi.c").read_text()


def function(signature):
    match = re.search(re.escape(signature) + r"[^;{]*\{", source)
    assert match, signature
    pos = match.end()
    depth = 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[match.start():pos]


production = "\n".join(function(signature) for signature in (
    "static void s_save_first_error(",
    "static int csi_dma_control_core(",
    "static bool IRAM_ATTR csi_dma_callback_enter(",
    "static void IRAM_ATTR csi_dma_callback_exit(",
    "static esp_err_t csi_dma_quiesce(",
    "static esp_err_t csi_dma_resume_callbacks(",
    "esp_err_t s_ctlr_csi_stop(",
    "esp_err_t s_del_csi_ctlr(",
))

HARNESS = r"""
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define IRAM_ATTR
#define CSI_DMA_STOP_TIMEOUT_MS 20
#define CONFIG_FREERTOS_UNICORE 0
#define CONFIG_FREERTOS_SMP 0
#define CONFIG_PM_ENABLE 0
#define SOC_ISP_SHARE_CSI_BRG 0
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_NOT_ALLOWED 4
#define DW_GDMA_LL_CHANNELS_PER_GROUP 4
#define DW_GDMA_LL_CHANNEL_EVENT_DISABLED (1u << 30)
#define MIPI_CSI_LL_HOST_CTLR_NUMS 1
#define TAG "host"
#define ESP_LOGE(tag, ...) do { (void)(tag); if (0) printf(__VA_ARGS__); } while (0)
#define ESP_RETURN_ON_FALSE(condition, error, ...) do { if (!(condition)) return (error); } while (0)
#define ESP_RETURN_ON_ERROR(expression, ...) do { int error_ = (expression); if (error_ != ESP_OK) return error_; } while (0)
#define PERIPH_RCC_ATOMIC() for (int once_ = 1; once_; --once_)
typedef int esp_err_t;
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms) / 10u)
typedef struct { volatile uint32_t val; } fake_reg_t;
typedef struct {
    fake_reg_t chen0;
    struct { fake_reg_t int_sig_ena0, int_st0; } ch[4];
} dw_gdma_dev_t;
static dw_gdma_dev_t device;
#define DW_GDMA_LL_GET_HW(group) (&device)
typedef struct { int channel_id; } fake_channel_t;
typedef fake_channel_t *dw_gdma_channel_handle_t;
typedef struct { int unused; } esp_cam_ctlr_t;
typedef esp_cam_ctlr_t *esp_cam_ctlr_handle_t;
typedef enum { CSI_FSM_INIT = 1, CSI_FSM_ENABLED, CSI_FSM_STARTED } csi_fsm_t;
typedef struct csi_controller_t {
    struct { void *bridge_dev; } hal;
    int spinlock, csi_id;
    csi_fsm_t csi_fsm;
    dw_gdma_channel_handle_t dma_chan;
    void *dma_link_list, *backup_buffer, *trans_que;
    int8_t dma_owner_core;
    uint32_t dma_saved_irq_mask, dma_callbacks_active;
    bool dma_irq_mask_saved, dma_stopping;
    esp_cam_ctlr_t base;
} csi_controller_t;
#define __containerof(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
static struct {
    uint64_t now_us, hardware_done_at, callback_done_at;
    unsigned delays, disables, frees, channel_deletes, ring_deletes;
    unsigned clears, premature_clears;
    int core, disable_error, channel_delete_error;
    bool bridge_on, stuck;
    csi_controller_t *ctx;
} state;
static fake_channel_t channel = {.channel_id = 1};
static int lock_depth;
static void lock(int *mux) { (void)mux; assert(lock_depth == 0); ++lock_depth; }
static void unlock(int *mux) { (void)mux; assert(lock_depth == 1); --lock_depth; }
#define portENTER_CRITICAL(mux) lock(mux)
#define portEXIT_CRITICAL(mux) unlock(mux)
#define portENTER_CRITICAL_ISR(mux) lock(mux)
#define portEXIT_CRITICAL_ISR(mux) unlock(mux)
static int xTaskGetCoreID(void *task) { (void)task; return state.core; }
static TickType_t xTaskGetTickCount(void) { return (TickType_t)(state.now_us / 10000); }
static const char *esp_err_to_name(esp_err_t error) { (void)error; return "fake"; }
static esp_err_t dw_gdma_channel_get_id(dw_gdma_channel_handle_t chan, int *id)
{
    assert(chan == &channel);
    *id = chan->channel_id;
    return ESP_OK;
}
static void dw_gdma_ll_channel_enable_intr_propagation(
    dw_gdma_dev_t *dev, unsigned id, uint32_t mask, bool enable)
{
    assert(lock_depth == 1 && dev == &device && id == 1 && !enable);
    dev->ch[id].int_sig_ena0.val &= ~mask;
}
static void mipi_csi_brg_ll_enable(void *bridge, bool enable)
{
    assert(bridge == &device && !enable);
    state.bridge_on = false;
}
static esp_err_t dw_gdma_channel_enable_ctrl(dw_gdma_channel_handle_t chan, bool enable)
{
    assert(!lock_depth && chan == &channel && !enable);
    assert(device.ch[1].int_sig_ena0.val == 0 && !state.bridge_on);
    assert(state.ctx->dma_stopping);
    ++state.disables;
    /* EN stays high until the final AXI transaction has completed. */
    if (!state.stuck && !state.disable_error) state.hardware_done_at = state.now_us + 75;
    return state.disable_error;
}
static void dw_gdma_ll_channel_clear_intr(dw_gdma_dev_t *dev, unsigned id, uint32_t mask)
{
    assert(dev == &device && id == 1);
    ++state.clears;
    if ((dev->chen0.val & (1u << id)) || state.ctx->dma_callbacks_active)
        ++state.premature_clears;
    dev->ch[id].int_st0.val &= ~mask;
}
static void csi_dma_callback_exit(csi_controller_t *ctlr);
static void esp_rom_delay_us(unsigned delay)
{
    assert(!lock_depth && delay == 1);
    ++state.now_us;
    ++state.delays;
    if (state.hardware_done_at && state.now_us >= state.hardware_done_at) {
        device.chen0.val &= ~(1u << 1);
        device.ch[1].int_st0.val |= DW_GDMA_LL_CHANNEL_EVENT_DISABLED;
        state.hardware_done_at = 0;
    }
    if (state.callback_done_at && state.now_us >= state.callback_done_at) {
        csi_dma_callback_exit(state.ctx);
        state.callback_done_at = 0;
    }
}
static void assert_quiet(void)
{
    assert(!(device.chen0.val & (1u << 1)) && !state.bridge_on);
    assert(state.ctx->dma_callbacks_active == 0 && state.ctx->dma_stopping);
    assert(device.ch[1].int_sig_ena0.val == 0);
}
static esp_err_t dw_gdma_del_channel(dw_gdma_channel_handle_t chan)
{
    assert(chan == &channel);
    assert_quiet();
    ++state.channel_deletes;
    return state.channel_delete_error;
}
static esp_err_t dw_gdma_del_link_list(void *ring)
{
    assert(ring == &channel);
    assert_quiet();
    ++state.ring_deletes;
    return ESP_OK;
}
static void mipi_csi_ll_enable_phy_config_clock(int id, int enable)
{
    (void)id; (void)enable; assert_quiet();
}
static esp_err_t s_csi_disable_phy_clock_source(csi_controller_t *ctlr)
{
    assert(ctlr == state.ctx); assert_quiet(); return ESP_OK;
}
static esp_err_t s_csi_declaim_controller(csi_controller_t *ctlr)
{
    assert(ctlr == state.ctx); assert_quiet(); return ESP_OK;
}
static void checked_free(void *allocation)
{
    assert_quiet();
    ++state.frees;
    free(allocation);
}
static void vQueueDeleteWithCaps(void *queue) { checked_free(queue); }
#define free checked_free

/* PRODUCTION */

static csi_controller_t *reset(bool running)
{
    assert(!lock_depth);
    memset(&state, 0, sizeof(state));
    memset(&device, 0, sizeof(device));
    csi_controller_t *ctx = calloc(1, sizeof(*ctx));
    assert(ctx);
    *ctx = (csi_controller_t) {
        .hal.bridge_dev = &device, .csi_id = -1,
        .csi_fsm = running ? CSI_FSM_STARTED : CSI_FSM_ENABLED,
        .dma_chan = &channel, .dma_link_list = &channel,
        .dma_owner_core = 0, .dma_stopping = !running,
        .backup_buffer = malloc(8), .trans_que = malloc(8),
    };
    assert(ctx->backup_buffer && ctx->trans_que);
    state.ctx = ctx;
    state.bridge_on = running;
    device.ch[1].int_sig_ena0.val = 0x1234;
    device.ch[1].int_st0.val = 0xff; /* Pending EOF from the old stream. */
    /* Another channel stays enabled and keeps its own interrupt state. */
    device.chen0.val = (1u << 3) | (running ? 1u << 1 : 0);
    device.ch[3].int_sig_ena0.val = 0x7654;
    device.ch[3].int_st0.val = 0xabc;
    return ctx;
}
static void assert_neighbor_unchanged(void)
{
    assert(device.chen0.val & (1u << 3));
    assert(device.ch[3].int_sig_ena0.val == 0x7654);
    assert(device.ch[3].int_st0.val == 0xabc);
}
static void delete_ok(csi_controller_t *ctx)
{
    assert(s_del_csi_ctlr(ctx) == ESP_OK);
    assert(state.frees == 3 && state.channel_deletes == 1 && state.ring_deletes == 1);
    assert_neighbor_unchanged();
    assert(state.premature_clears == 0);
}
int main(void)
{
    /* The first start/partial-construction cleanup cannot wait for a
     * DISABLED edge that a never-enabled channel will never produce. */
    csi_controller_t *ctx = reset(false);
    assert(csi_dma_quiesce(ctx) == ESP_OK && state.delays == 0 && state.disables == 0);
    assert(ctx->dma_saved_irq_mask == 0x1234 && ctx->dma_irq_mask_saved);
    assert(device.ch[1].int_sig_ena0.val == 0 && device.ch[1].int_st0.val == 0);
    assert(csi_dma_resume_callbacks(ctx) == ESP_OK);
    assert(!ctx->dma_stopping && !ctx->dma_irq_mask_saved);
    assert(device.ch[1].int_sig_ena0.val == 0x1234);
    delete_ok(ctx);

    /* Returning from the API disable request is not completion. A stale
     * DISABLED flag must not allow a still-enabled channel to escape early. */
    ctx = reset(true);
    device.ch[1].int_st0.val |= DW_GDMA_LL_CHANNEL_EVENT_DISABLED;
    assert(s_ctlr_csi_stop(&ctx->base) == ESP_OK);
    assert(state.delays == 75 && state.disables == 1);
    assert(ctx->csi_fsm == CSI_FSM_ENABLED && ctx->dma_stopping);
    assert(!csi_dma_callback_enter(ctx) && ctx->dma_callbacks_active == 0);
    assert(s_ctlr_csi_stop(&ctx->base) == ESP_ERR_INVALID_STATE);
    assert(csi_dma_quiesce(ctx) == ESP_OK && state.disables == 1);
    assert(ctx->dma_saved_irq_mask == 0x1234); /* Repeated stop did not save zero. */
    assert(csi_dma_resume_callbacks(ctx) == ESP_OK);
    assert(device.ch[1].int_sig_ena0.val == 0x1234);
    assert(csi_dma_callback_enter(ctx) && ctx->dma_callbacks_active == 1);
    csi_dma_callback_exit(ctx);
    delete_ok(ctx);

    /* An INVALID-LLI halt does not clear EN; soft rearm still performs a
     * real disable and clears the invalid status before restoring callbacks. */
    ctx = reset(true);
    device.ch[1].int_st0.val = 1u << 13;
    assert(csi_dma_quiesce(ctx) == ESP_OK && state.disables == 1);
    assert(device.ch[1].int_st0.val == 0);
    delete_ok(ctx);

    /* Defensive callback drain: DMA may be idle before a callback exits. */
    ctx = reset(true);
    assert(csi_dma_callback_enter(ctx));
    state.callback_done_at = 125;
    assert(csi_dma_quiesce(ctx) == ESP_OK);
    assert(state.delays == 125 && ctx->dma_callbacks_active == 0);
    delete_ok(ctx);

    /* No IRQ restore, ownership return or teardown on a hung DMA. A later
     * retry retains the original interrupt mask and can finish safely. */
    ctx = reset(true);
    state.stuck = true;
    assert(s_ctlr_csi_stop(&ctx->base) == ESP_ERR_TIMEOUT);
    assert(state.delays == 20000 && ctx->csi_fsm == CSI_FSM_STARTED);
    assert(ctx->dma_irq_mask_saved && ctx->dma_saved_irq_mask == 0x1234);
    assert(csi_dma_resume_callbacks(ctx) == ESP_ERR_INVALID_STATE);
    assert(s_del_csi_ctlr(ctx) == ESP_ERR_TIMEOUT);
    assert(state.frees == 0 && state.channel_deletes == 0 && state.ring_deletes == 0);
    assert(ctx->dma_chan == &channel && ctx->dma_link_list == &channel);
    assert(state.premature_clears == 0 && device.ch[1].int_sig_ena0.val == 0);
    state.stuck = false;
    assert(s_ctlr_csi_stop(&ctx->base) == ESP_OK);
    assert(ctx->csi_fsm == CSI_FSM_ENABLED && ctx->dma_saved_irq_mask == 0x1234);
    assert(csi_dma_resume_callbacks(ctx) == ESP_OK);
    assert(device.ch[1].int_sig_ena0.val == 0x1234);
    delete_ok(ctx);

    /* Disable API failure is reported with the same retaining contract. */
    ctx = reset(true);
    state.disable_error = ESP_FAIL;
    assert(s_del_csi_ctlr(ctx) == ESP_ERR_TIMEOUT && state.frees == 0);
    assert(ctx->csi_fsm == CSI_FSM_STARTED && ctx->dma_stopping);
    state.disable_error = ESP_OK;
    delete_ok(ctx);

    /* A hung callback also prevents freeing an already idle channel. */
    ctx = reset(false);
    ctx->dma_callbacks_active = 1;
    assert(s_del_csi_ctlr(ctx) == ESP_ERR_TIMEOUT && state.frees == 0);
    assert(state.disables == 0 && state.delays == 20000);
    csi_dma_callback_exit(ctx);
    delete_ok(ctx);

    /* Tick wrap must not turn the timeout into an unbounded wait. */
    ctx = reset(true);
    state.now_us = (uint64_t)UINT32_MAX * 10000;
    state.stuck = true;
    assert(csi_dma_quiesce(ctx) == ESP_ERR_TIMEOUT && state.delays == 20000);
    state.stuck = false;
    delete_ok(ctx);

    /* Other-core and unpinned lifecycle callers cannot outlive an old IDF
     * ISR. Rejection preserves hardware, the handle and its allocations. */
    for (unsigned i = 0; i < 2; ++i) {
        ctx = reset(true);
        state.core = i == 0 ? 1 : -1;
        assert(s_ctlr_csi_stop(&ctx->base) == ESP_ERR_NOT_ALLOWED);
        assert(s_del_csi_ctlr(ctx) == ESP_ERR_NOT_ALLOWED && state.frees == 0);
        assert(state.bridge_on && state.disables == 0 && ctx->csi_fsm == CSI_FSM_STARTED);
        assert(device.ch[1].int_sig_ena0.val == 0x1234 && !ctx->dma_irq_mask_saved);
        state.core = 0;
        delete_ok(ctx);
    }

    /* Missing DMA during failed construction still disables the bridge and
     * cleans up safely; no DMA register polling is necessary. */
    ctx = reset(false);
    ctx->dma_chan = NULL;
    ctx->dma_link_list = NULL;
    device.ch[1].int_sig_ena0.val = 0;
    assert(s_del_csi_ctlr(ctx) == ESP_OK);
    assert(state.frees == 3 && state.channel_deletes == 0 && state.ring_deletes == 0);

    /* Secondary cleanup failures retain the existing consumed-handle
     * contract once quiescence has succeeded. */
    ctx = reset(true);
    state.channel_delete_error = ESP_FAIL;
    assert(s_del_csi_ctlr(ctx) == ESP_FAIL);
    assert(state.frees == 3 && state.channel_deletes == 1 && state.ring_deletes == 1);
    assert(state.premature_clears == 0);
    puts("CSI DMA quiescence: delayed AXI/ISR, timeout ownership, IRQ restore, owner core, delete: PASS");
}
"""

with tempfile.TemporaryDirectory(prefix="csi-dma-quiesce-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(HARNESS.replace("/* PRODUCTION */", production))
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
        str(path / "test.c"), "-o", str(path / "test"),
    ], check=True)
    environment = os.environ.copy()
    # LeakSanitizer does not work when the process runs under ptrace
    # (debuggers, ptrace-based sandboxes), so leak detection is off.
    environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
    subprocess.run([str(path / "test")], check=True, env=environment)

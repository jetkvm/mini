#!/usr/bin/env python3
"""Run ESP-IDF's SDMMC transaction code against a controller that stalls.

The stock timeout path only stops the DMA, so the controller stays inside the
abandoned transfer and every later command waits for it; the stock read path
also reports a timed-out read as ESP_OK.

Compiles `sd_trans_sdmmc.c` and the command/reset functions of
`sd_host_sdmmc.c` with and without the patch against a fake controller and
card. The stock code must stay wedged and report success; the patched code
must recover and report every timeout. With
`esp-idf-6.0.2-sdmmc-data-error-recovery.patch` too, a TF transfer that fails
with a CRC error resets the controller, so the next CMD53 works at once.
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
PATCHES = ROOT / "patches"
PATCH_NAME = "esp-idf-6.0.2-sdmmc-timeout-recovery.patch"
DATA_ERROR_PATCH = "esp-idf-6.0.2-sdmmc-data-error-recovery.patch"
# Earlier patches that change the same files, in CMakeLists.txt order.
PREREQUISITES = ["esp-idf-6.0.2-sdmmc-isr-slot-dispatch.patch",
                 "esp-idf-6.0.2-sdmmc-tx-cache-sync.patch"]
DRIVER = "components/esp_driver_sdmmc"
FILES = ["Kconfig", "include/esp_private/sd_host_private.h",
         "src/sd_host_sdmmc.c", "src/sd_trans_sdmmc.c"]
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

registration = f"apply_idf_patch(patches/{PATCH_NAME})"
assert registration in CMAKE
for prerequisite in PREREQUISITES:
    assert CMAKE.index(f"apply_idf_patch(patches/{prerequisite})") < CMAKE.index(registration)
assert CMAKE.index(registration) < CMAKE.index("include($ENV{IDF_PATH}/tools/cmake/project.cmake)")
assert CMAKE.index(registration) < CMAKE.index(f"apply_idf_patch(patches/{DATA_ERROR_PATCH})") < \
    CMAKE.index("include($ENV{IDF_PATH}/tools/cmake/project.cmake)")

idf_path = os.environ.get("IDF_PATH")
if not idf_path or not (Path(idf_path) / DRIVER / "src/sd_trans_sdmmc.c").is_file():
    print(f"SKIP {Path(__file__).name}: needs IDF_PATH with {DRIVER}")
    sys.exit(0)
compiler = shutil.which("cc") or shutil.which("gcc")
assert compiler, "no host C compiler"

FAKE_HEADER = r"""
#pragma once
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC 0x109

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W %s: " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOG_OFF(tag, fmt, ...) do { if (0) fprintf(stderr, fmt, ##__VA_ARGS__); } while (0)
#define ESP_LOGI ESP_LOG_OFF
#define ESP_LOGD ESP_LOG_OFF
#define ESP_LOGV ESP_LOG_OFF
#define ESP_RETURN_ON_FALSE(c, err, tag, ...) do { if (!(c)) return (err); } while (0)
#define ESP_RETURN_ON_ERROR(x, tag, ...) do { esp_err_t err_rc_ = (x); if (err_rc_ != ESP_OK) return err_rc_; } while (0)
#define ESP_GOTO_ON_ERROR(x, label, tag, ...) do { esp_err_t err_rc_ = (x); if (err_rc_ != ESP_OK) { ret = err_rc_; goto label; } } while (0)
#define __containerof(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

#define SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE 1
#define SOC_SDMMC_NUM_SLOTS 2
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C 2
esp_err_t esp_cache_msync(void *addr, size_t size, int flags);
int64_t esp_timer_get_time(void);

#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
#define portTICK_PERIOD_MS 1
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
int xQueueReceive(QueueHandle_t q, void *item, uint32_t ticks);
int xQueueReset(QueueHandle_t q);
int xSemaphoreTake(SemaphoreHandle_t s, uint32_t ticks);
int xSemaphoreGive(SemaphoreHandle_t s);
void vTaskDelay(uint32_t ticks);

#define SCF_CMD_READ 0x0008
#define SCF_RSP_PRESENT 0x0100
#define SCF_RSP_136 0x0200
#define SCF_RSP_CRC 0x0400
#define SCF_WAIT_BUSY 0x2000
#define MMC_GO_IDLE_STATE 0
#define MMC_STOP_TRANSMISSION 12
#define MMC_READ_DAT_UNTIL_STOP 11
#define MMC_READ_BLOCK_MULTIPLE 18
#define MMC_WRITE_DAT_UNTIL_STOP 20
#define MMC_WRITE_BLOCK_MULTIPLE 25
#define MMC_APP_CMD 55
#define MMC_SWITCH 6
#define SD_SWITCH_VOLTAGE 11
#define SD_IO_RW_DIRECT 52
#define SD_IO_RW_EXTENDED 53
#define SD_ARG_CMD52_WRITE (1u << 31)
#define SD_ARG_CMD52_REG_SHIFT 9
#define SD_ARG_CMD53_FUNC_SHIFT 28
#define SD_ARG_CMD53_FUNC_MASK 0x7
#define SD_IO_CCCR_CTL 0x06

#define SDMMC_INTMASK_CD (1u << 0)
#define SDMMC_INTMASK_RESP_ERR (1u << 1)
#define SDMMC_INTMASK_CMD_DONE (1u << 2)
#define SDMMC_INTMASK_DATA_OVER (1u << 3)
#define SDMMC_INTMASK_RCRC (1u << 6)
#define SDMMC_INTMASK_DCRC (1u << 7)
#define SDMMC_INTMASK_RTO (1u << 8)
#define SDMMC_INTMASK_DTO (1u << 9)
#define SDMMC_INTMASK_HTO (1u << 10)
#define SDMMC_INTMASK_HLE (1u << 12)
#define SDMMC_INTMASK_SBE (1u << 13)
#define SDMMC_INTMASK_EBE (1u << 15)
#define SDMMC_INTMASK_VOLT_SW SDMMC_INTMASK_HTO
#define SDMMC_LL_SD_EVENT_MASK 0x0000ffffu
#define SDMMC_IDMAC_INTMASK_TI (1u << 0)
#define SDMMC_IDMAC_INTMASK_RI (1u << 1)
#define SDMMC_IDMAC_INTMASK_NI (1u << 8)
#define SDMMC_DMA_MAX_BUF_LEN 4096
#define SD_HOST_SDMMC_START_CMD_TIMEOUT_US (1000 * 1000)
#define SD_HOST_SDMMC_RESET_TIMEOUT_US (5000 * 1000)

typedef struct {
    uint32_t cmd_index, response_expect, response_long, check_response_crc, data_expected, rw,
             send_auto_stop, wait_complete, stop_abort_cmd, send_init, update_clk_reg, card_num,
             volt_switch, use_hold_reg, start_command;
} sdmmc_hw_cmd_t;
typedef struct sdmmc_desc_s {
    uint32_t first_descriptor, last_descriptor, second_address_chained, owned_by_idmac, buffer1_size;
    void *buffer1_ptr;
    struct sdmmc_desc_s *next_desc_ptr;
} sdmmc_desc_t;
typedef struct {
    struct { uint32_t val; } status, rintsts, idsts;
    struct { uint32_t fifo_reset; } ctrl;
    uint32_t resp[4];
} sdmmc_dev_t;
extern sdmmc_dev_t SDMMC;

typedef struct {
    uint32_t opcode, arg, response[4];
    void *data;
    size_t datalen, blklen, buflen;
    int flags;
    esp_err_t error;
    uint32_t timeout_ms;
    esp_err_t (*volt_switch_cb)(void *, int);
    void *volt_switch_cb_arg;
} sdmmc_command_t;

typedef struct { int unused; } sd_slot_driver_t;
typedef sd_slot_driver_t *sd_host_slot_handle_t;
typedef enum { SD_HOST_SLOT_STATE_INIT, SD_HOST_SLOT_STATE_READY } sd_host_sdmmc_slot_state_t;
typedef struct { uint32_t sdmmc_status, dma_status; } sd_host_sdmmc_event_t;
typedef struct { uint8_t *ptr; size_t size_remaining, next_desc, desc_remaining; } sd_host_sdmmc_trans_state_t;
typedef struct sd_host_sdmmc_ctlr_t sd_host_sdmmc_ctlr_t;
typedef struct {
    sd_slot_driver_t drv;
    int slot_id;
    struct { int freq_hz; sd_host_sdmmc_slot_state_t freq_state; } freq;
    sd_host_sdmmc_ctlr_t *ctlr;
} sd_host_sdmmc_slot_t;
struct sd_host_sdmmc_ctlr_t {
    struct { sdmmc_dev_t *dev; } hal;
    sdmmc_desc_t *dma_desc;
    int dma_desc_num;
    sd_host_sdmmc_trans_state_t cur_transfer;
    int cur_slot_id;
    bool is_app_cmd;
    SemaphoreHandle_t mutex;
    QueueHandle_t event_queue;
    uint32_t lost_events;
};

esp_err_t sd_host_slot_start_command(sd_host_sdmmc_slot_t *slot, sdmmc_hw_cmd_t cmd, uint32_t arg);
esp_err_t sd_host_recover(sd_host_sdmmc_slot_t *slot);
esp_err_t sd_host_slot_set_card_clk(sd_host_sdmmc_slot_t *slot);
esp_err_t sd_host_slot_get_real_freq(sd_host_sdmmc_slot_t *slot, int *real_freq_khz);
esp_err_t sd_host_slot_set_bus_width(sd_host_sdmmc_slot_t *slot);
esp_err_t sd_host_slot_set_bus_sampling_mode(sd_host_sdmmc_slot_t *slot);
esp_err_t sd_host_set_delay_phase(sd_host_sdmmc_slot_t *slot);
esp_err_t sd_host_set_delay_line(sd_host_sdmmc_slot_t *slot);
esp_err_t sd_host_slot_set_cclk_always_on_internal(sd_host_sdmmc_slot_t *slot, bool always_on);
void sd_host_slot_enable_clk_cmd11(sd_host_sdmmc_slot_t *slot, bool enable);
bool sd_host_check_buffer_alignment(sd_host_sdmmc_slot_t *slot, const void *buf, size_t size);

bool sdmmc_ll_is_card_detected(sdmmc_dev_t *hw, int slot);
bool sdmmc_ll_is_card_write_protected(sdmmc_dev_t *hw, int slot);
bool sdmmc_ll_is_card_data_busy(sdmmc_dev_t *hw);
bool sdmmc_ll_is_command_taken(sdmmc_dev_t *hw);
void sdmmc_ll_set_command_arg(sdmmc_dev_t *hw, uint32_t arg);
void sdmmc_ll_set_command(sdmmc_dev_t *hw, sdmmc_hw_cmd_t cmd);
void sdmmc_ll_set_data_transfer_len(sdmmc_dev_t *hw, uint32_t len);
void sdmmc_ll_set_block_size(sdmmc_dev_t *hw, uint32_t size);
void sdmmc_ll_set_desc_addr(sdmmc_dev_t *hw, uint32_t addr);
void sdmmc_ll_enable_dma(sdmmc_dev_t *hw, bool en);
void sdmmc_ll_stop_dma(sdmmc_dev_t *hw);
void sdmmc_ll_poll_demand(sdmmc_dev_t *hw);
void sdmmc_ll_init_dma(sdmmc_dev_t *hw);
void sdmmc_ll_reset_controller(sdmmc_dev_t *hw);
void sdmmc_ll_reset_dma(sdmmc_dev_t *hw);
void sdmmc_ll_reset_fifo(sdmmc_dev_t *hw);
bool sdmmc_ll_is_controller_reset_done(sdmmc_dev_t *hw);
bool sdmmc_ll_is_dma_reset_done(sdmmc_dev_t *hw);
bool sdmmc_ll_is_fifo_reset_done(sdmmc_dev_t *hw);
void sdmmc_ll_clear_interrupt(sdmmc_dev_t *hw, uint32_t mask);
uint32_t sdmmc_ll_get_idsts_interrupt_raw(sdmmc_dev_t *hw);
void sdmmc_ll_clear_idsts_interrupt(sdmmc_dev_t *hw, uint32_t mask);
"""

HOST_PREFIX = r"""
#include "esp_private/sd_host_private.h"
static const char *TAG = "SD_HOST";
static esp_err_t sd_host_slot_clock_update_command(sd_host_sdmmc_slot_t *slot, bool is_cmd11);
"""

# The fake controller and card. A command is taken only while no data
# transfer is in flight; writing a command while one is pending is a hardware
# locked error. A data transfer completes (DATA_OVER) only with the DMA
# running, unless the scenario stalls it; a stalled transfer stays in flight
# until a controller reset. A card that accepted a stalled CMD53 stays in its
# data phase and does not answer the next CMD53 (response timeout) until a
# CMD52 write to the I/O abort register.
HARNESS = r"""
#include "esp_private/sd_host_private.h"
#include <stdlib.h>

sdmmc_dev_t SDMMC;
static struct {
    bool stall_next, crc_next, in_flight, pending, dma_on, card_in_cmd53;
    int resets, clock_updates, aborts;
    sd_host_sdmmc_event_t queue[32];
    int queued;
} hw;

static void push(uint32_t status)
{
    assert(hw.queued < 32);
    hw.queue[hw.queued++] = (sd_host_sdmmc_event_t){.sdmmc_status = status};
}

esp_err_t esp_cache_msync(void *addr, size_t size, int flags) { (void)addr; (void)size; (void)flags; return ESP_OK; }
int64_t esp_timer_get_time(void) { static int64_t now; return now += 1000; }
int xQueueReceive(QueueHandle_t q, void *item, uint32_t ticks)
{
    (void)q; (void)ticks;
    if (hw.queued == 0) {
        return pdFALSE;  /* the wait times out */
    }
    memcpy(item, &hw.queue[0], sizeof(hw.queue[0]));
    memmove(&hw.queue[0], &hw.queue[1], sizeof(hw.queue[0]) * --hw.queued);
    return pdTRUE;
}
int xQueueReset(QueueHandle_t q) { (void)q; hw.queued = 0; return pdTRUE; }
int xSemaphoreTake(SemaphoreHandle_t s, uint32_t t) { (void)s; (void)t; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
void vTaskDelay(uint32_t t) { (void)t; }

esp_err_t sd_host_slot_set_card_clk(sd_host_sdmmc_slot_t *s) { (void)s; return ESP_OK; }
esp_err_t sd_host_slot_get_real_freq(sd_host_sdmmc_slot_t *s, int *khz) { *khz = s->freq.freq_hz / 1000; return ESP_OK; }
esp_err_t sd_host_slot_set_bus_width(sd_host_sdmmc_slot_t *s) { (void)s; return ESP_OK; }
esp_err_t sd_host_slot_set_bus_sampling_mode(sd_host_sdmmc_slot_t *s) { (void)s; return ESP_OK; }
esp_err_t sd_host_set_delay_phase(sd_host_sdmmc_slot_t *s) { (void)s; return ESP_OK; }
esp_err_t sd_host_set_delay_line(sd_host_sdmmc_slot_t *s) { (void)s; return ESP_OK; }
esp_err_t sd_host_slot_set_cclk_always_on_internal(sd_host_sdmmc_slot_t *s, bool on) { (void)s; (void)on; return ESP_OK; }
void sd_host_slot_enable_clk_cmd11(sd_host_sdmmc_slot_t *s, bool en) { (void)s; (void)en; }
bool sd_host_check_buffer_alignment(sd_host_sdmmc_slot_t *s, const void *b, size_t n) { (void)s; (void)b; (void)n; return true; }

bool sdmmc_ll_is_card_detected(sdmmc_dev_t *d, int slot) { (void)d; (void)slot; return true; }
bool sdmmc_ll_is_card_write_protected(sdmmc_dev_t *d, int slot) { (void)d; (void)slot; return false; }
bool sdmmc_ll_is_card_data_busy(sdmmc_dev_t *d) { (void)d; return false; }
bool sdmmc_ll_is_command_taken(sdmmc_dev_t *d) { (void)d; return !hw.pending; }
static uint32_t s_arg;
void sdmmc_ll_set_command_arg(sdmmc_dev_t *d, uint32_t arg) { (void)d; s_arg = arg; }
void sdmmc_ll_set_command(sdmmc_dev_t *d, sdmmc_hw_cmd_t cmd)
{
    (void)d;
    if (hw.pending) {
        push(SDMMC_INTMASK_HLE);
        return;
    }
    if (hw.in_flight && cmd.wait_complete) {
        hw.pending = true;  /* waits for the transfer in flight */
        return;
    }
    if (cmd.update_clk_reg) {
        hw.clock_updates++;
        return;
    }
    if (cmd.cmd_index == SD_IO_RW_DIRECT && s_arg == (SD_ARG_CMD52_WRITE | (SD_IO_CCCR_CTL << SD_ARG_CMD52_REG_SHIFT) | 1)) {
        hw.aborts++;
        hw.card_in_cmd53 = false;
    }
    if (cmd.cmd_index == SD_IO_RW_EXTENDED && hw.card_in_cmd53) {
        push(SDMMC_INTMASK_CMD_DONE | SDMMC_INTMASK_RTO);
        return;
    }
    push(SDMMC_INTMASK_CMD_DONE);
    if (!cmd.data_expected) {
        return;
    }
    if (hw.crc_next) {
        /* The card answers with bad CRC: the transfer ends with the error,
         * and the data path stays busy until a reset. */
        hw.crc_next = false;
        hw.in_flight = true;
        push(SDMMC_INTMASK_DCRC | SDMMC_INTMASK_DATA_OVER);
        return;
    }
    if (hw.stall_next || !hw.dma_on) {
        hw.stall_next = false;
        hw.in_flight = true;
        hw.card_in_cmd53 = cmd.cmd_index == SD_IO_RW_EXTENDED;
        return;
    }
    push(SDMMC_INTMASK_DATA_OVER);
}
void sdmmc_ll_set_data_transfer_len(sdmmc_dev_t *d, uint32_t n) { (void)d; (void)n; }
void sdmmc_ll_set_block_size(sdmmc_dev_t *d, uint32_t n) { (void)d; (void)n; }
void sdmmc_ll_set_desc_addr(sdmmc_dev_t *d, uint32_t a) { (void)d; (void)a; }
void sdmmc_ll_enable_dma(sdmmc_dev_t *d, bool en) { (void)d; hw.dma_on = en; }
void sdmmc_ll_stop_dma(sdmmc_dev_t *d) { (void)d; hw.dma_on = false; }
void sdmmc_ll_poll_demand(sdmmc_dev_t *d) { (void)d; }
void sdmmc_ll_init_dma(sdmmc_dev_t *d) { (void)d; }
void sdmmc_ll_reset_controller(sdmmc_dev_t *d) { (void)d; hw.resets++; hw.in_flight = hw.pending = false; }
void sdmmc_ll_reset_dma(sdmmc_dev_t *d) { (void)d; }
void sdmmc_ll_reset_fifo(sdmmc_dev_t *d) { (void)d; }
bool sdmmc_ll_is_controller_reset_done(sdmmc_dev_t *d) { (void)d; return true; }
bool sdmmc_ll_is_dma_reset_done(sdmmc_dev_t *d) { (void)d; return true; }
bool sdmmc_ll_is_fifo_reset_done(sdmmc_dev_t *d) { (void)d; return true; }
void sdmmc_ll_clear_interrupt(sdmmc_dev_t *d, uint32_t m) { (void)d; (void)m; }
uint32_t sdmmc_ll_get_idsts_interrupt_raw(sdmmc_dev_t *d) { (void)d; return 0; }
void sdmmc_ll_clear_idsts_interrupt(sdmmc_dev_t *d, uint32_t m) { (void)d; (void)m; }

esp_err_t sd_host_slot_sdmmc_do_transaction(sd_host_slot_handle_t slot, sdmmc_command_t *cmdinfo);

static sdmmc_desc_t s_desc[4];
static sd_host_sdmmc_ctlr_t s_ctlr = {.hal = {&SDMMC}, .dma_desc = s_desc, .dma_desc_num = 4,
                                      .event_queue = &hw.queue};
static sd_host_sdmmc_slot_t s_slot[2] = {
    {.slot_id = 0, .freq = {20000000, SD_HOST_SLOT_STATE_READY}, .ctlr = &s_ctlr},
    {.slot_id = 1, .freq = {40000000, SD_HOST_SLOT_STATE_READY}, .ctlr = &s_ctlr},
};
static uint32_t s_buf[384];

/* One CMD53 to function 1 (or, on slot 0, a block read) of `len` bytes. */
static esp_err_t transfer(int slot, bool read, size_t len)
{
    sdmmc_command_t cmd = {
        .opcode = slot == 1 ? 53 : 17, .arg = slot == 1 ? 1u << SD_ARG_CMD53_FUNC_SHIFT : 0, .flags = SCF_RSP_PRESENT | (read ? SCF_CMD_READ : 0),
        .data = s_buf, .datalen = len, .blklen = len < 512 ? len : 512, .buflen = len,
        .timeout_ms = 1000,
    };
    /* As sdmmc_send_cmd(): the transaction's result, then the command's. */
    esp_err_t ret = sd_host_slot_sdmmc_do_transaction(&s_slot[slot].drv, &cmd);
    return ret != ESP_OK ? ret : cmd.error;
}

static int expect(esp_err_t got, esp_err_t want, const char *what)
{
    if (got != want) {
        printf("FAIL %s: got 0x%x, want 0x%x\n", what, got, want);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *scenario = argc > 1 ? argv[1] : "";
    int fails = 0;
    if (strcmp(scenario, "write_stall") == 0) {
        /* A CMD53 write stalls; the next writes and a TF read must work. */
        hw.stall_next = true;
        fails += expect(transfer(1, false, 512), ESP_ERR_TIMEOUT, "stalled write");
        fails += expect(transfer(1, false, 512), ESP_OK, "next write");
        fails += expect(transfer(0, true, 512), ESP_OK, "TF read on slot 0");
        fails += expect(transfer(1, false, 64), ESP_OK, "later write");
    } else if (strcmp(scenario, "read_stall") == 0) {
        /* A CMD53 read stalls: the caller must see the timeout. */
        hw.stall_next = true;
        fails += expect(transfer(1, true, 1536), ESP_ERR_TIMEOUT, "stalled read");
        fails += expect(transfer(1, true, 1536), ESP_OK, "next read");
    } else if (strcmp(scenario, "start_timeout") == 0) {
        /* The controller is already inside an abandoned transfer, as after
         * the stock timeout: the next command is never taken. */
        hw.in_flight = true;
        fails += expect(transfer(1, false, 512), ESP_ERR_TIMEOUT, "command not taken");
        fails += expect(transfer(1, false, 512), ESP_OK, "next write");
    } else if (strcmp(scenario, "data_error") == 0) {
        /* A TF read fails with a CRC error; the next CMD53 to the C5 must
         * work at once, without a timeout. */
        hw.crc_next = true;
        fails += expect(transfer(0, true, 512), ESP_ERR_INVALID_CRC, "TF read with CRC error");
        fails += expect(transfer(1, false, 512), ESP_OK, "next write to the C5");
        fails += expect(transfer(0, true, 512), ESP_OK, "next TF read");
    } else if (strcmp(scenario, "no_stall") == 0) {
        /* Transfers that complete must not reset the controller. */
        fails += expect(transfer(1, false, 512), ESP_OK, "write");
        fails += expect(transfer(1, true, 1536), ESP_OK, "read");
        fails += expect(transfer(0, true, 512), ESP_OK, "TF read");
        if (hw.resets != 0) {
            printf("FAIL %d controller resets without a timeout\n", hw.resets);
            fails++;
        }
    } else {
        printf("unknown scenario %s\n", scenario);
        return 2;
    }
    printf("resets=%d clock_updates=%d aborts=%d\n", hw.resets, hw.clock_updates, hw.aborts);
    return fails ? 1 : 0;
}
"""

SCENARIOS = ["write_stall", "read_stall", "start_timeout", "data_error", "no_stall"]


def git_apply(tree: Path, patch: Path, *flags: str) -> bool:
    return subprocess.run(["git", "apply", "--check", *flags, "--whitespace=nowarn", str(patch)],
                          cwd=tree, capture_output=True).returncode == 0


def variants(work: Path) -> dict:
    """Return the driver sources before and after the patch, each with the prerequisites."""
    tree = work / "idf"
    for name in FILES:
        target = tree / DRIVER / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(Path(idf_path) / DRIVER / name, target)
    patch = PATCHES / PATCH_NAME
    data_error_patch = PATCHES / DATA_ERROR_PATCH
    if git_apply(tree, data_error_patch, "--reverse"):
        subprocess.run(["git", "apply", "--reverse", "--whitespace=nowarn", str(data_error_patch)], cwd=tree, check=True)
    if git_apply(tree, patch, "--reverse"):
        subprocess.run(["git", "apply", "--reverse", "--whitespace=nowarn", str(patch)], cwd=tree, check=True)
    for prerequisite in PREREQUISITES:
        if git_apply(tree, PATCHES / prerequisite):
            subprocess.run(["git", "apply", "--whitespace=nowarn", str(PATCHES / prerequisite)],
                           cwd=tree, check=True)
    read = lambda: {name: (tree / DRIVER / name).read_text(encoding="utf-8") for name in FILES[2:]}
    stock = read()
    subprocess.run(["git", "apply", "--whitespace=nowarn", str(patch)], cwd=tree, check=True)
    timeout_only = read()
    subprocess.run(["git", "apply", "--whitespace=nowarn", str(data_error_patch)], cwd=tree, check=True)
    patched = read()
    assert git_apply(tree, patch, "--reverse"), f"{PATCH_NAME} no longer reverses under {DATA_ERROR_PATCH}"
    # A second configure finds every patch applied only if this one keeps the
    # earlier patches' lines intact (cmake/apply_idf_patch.cmake).
    for prerequisite in PREREQUISITES:
        assert git_apply(tree, PATCHES / prerequisite, "--reverse"), f"{prerequisite} no longer reverses"
    assert "sd_host_recover" not in stock["src/sd_trans_sdmmc.c"]
    assert "sd_host_recover" in patched["src/sd_trans_sdmmc.c"]
    return {"stock": stock, "timeout_only": timeout_only, "patched": patched}


def host_functions(source: str) -> str:
    """From sd_host_slot_start_command to the clock section: the command, clock
    update, reset and (patched) recovery functions."""
    start = source.index("esp_err_t sd_host_slot_start_command(")
    end = source.index("                    Static: Clock")
    return HOST_PREFIX + source[start:source.rindex("/*", start, end)]


def run(work: Path, name: str, sources: dict) -> dict:
    build = work / name
    (build / "esp_private").mkdir(parents=True)
    (build / "esp_private" / "sd_host_private.h").write_text(FAKE_HEADER, encoding="utf-8")
    for header in ["esp_log.h", "esp_intr_alloc.h", "esp_check.h", "esp_timer.h",
                   "soc/soc_caps.h", "driver/sd_host.h"]:
        (build / header).parent.mkdir(parents=True, exist_ok=True)
        (build / header).write_text('#include "esp_private/sd_host_private.h"\n', encoding="utf-8")
    units = {"trans.c": sources["src/sd_trans_sdmmc.c"],
             "host.c": host_functions(sources["src/sd_host_sdmmc.c"]),
             "harness.c": HARNESS}
    for unit, text in units.items():
        (build / unit).write_text(text, encoding="utf-8")
    exe = build / "run"
    subprocess.run([compiler, "-std=gnu11", "-O0", "-g", "-w", "-I", str(build), "-o", str(exe),
                    *(str(build / unit) for unit in units)], check=True)
    results = {}
    for scenario in SCENARIOS:
        proc = subprocess.run([str(exe), scenario], capture_output=True, text=True, timeout=60)
        results[scenario] = (proc.returncode, proc.stdout.strip())
    return results


with tempfile.TemporaryDirectory() as tmp:
    work = Path(tmp)
    texts = variants(work)
    stock = run(work, "stock", texts["stock"])
    timeout_only = run(work, "timeout_only", texts["timeout_only"])
    patched = run(work, "patched", texts["patched"])

# The stock driver reproduces the device failure: after the stall every
# command times out, and the stalled read is reported as a success. Transfers
# that complete work in both versions.
assert stock["write_stall"][0] == 1 and "FAIL next write" in stock["write_stall"][1], stock["write_stall"]
assert stock["start_timeout"][0] == 1 and "FAIL next write" in stock["start_timeout"][1], stock["start_timeout"]
assert stock["read_stall"][0] == 1 and "FAIL stalled read: got 0x0" in stock["read_stall"][1], stock["read_stall"]
assert stock["no_stall"][0] == 0, stock["no_stall"]

# The timeout recovery alone recovers only after the next command has timed
# out: ESP-Hosted counts that failed CMD53, and restarts the P4 after two.
assert timeout_only["data_error"][0] == 1 and "FAIL next write to the C5: got 0x107" in timeout_only["data_error"][1], \
    timeout_only["data_error"]
for scenario, (code, output) in patched.items():
    assert code == 0, f"patched {scenario}: rc={code} {output}"
# One controller reset and one clock update for the failed TF read; no SDIO
# abort, because the failed transfer was no CMD53.
assert "resets=1 clock_updates=1 aborts=0" in patched["data_error"][1], patched["data_error"]
# One controller reset, one clock update and one SDIO abort per CMD53 timeout.
for scenario in ["write_stall", "read_stall", "start_timeout"]:
    assert "resets=1 clock_updates=1 aborts=1" in patched[scenario][1], patched[scenario]

print(f"PASS {Path(__file__).name}: stock stays wedged, patched recovers in all {len(SCENARIOS)} scenarios")

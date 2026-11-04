#!/usr/bin/env python3
"""Run ESP-IDF's SDMMC interrupt handler against a shared-controller state.

On the Wi-Fi Mini the ESP32-C5 (SDIO, slot 1) and the TF card (slot 0) share
one SDMMC controller. When the TF card fails to mount, slot 0 is removed while
`cur_slot_id` still names it, and the stock `sd_host_isr` dereferences the
removed slot on the next SDIO interrupt.

Compiles `sd_host_isr` with and without the patch against a fake controller
and runs eight scenarios in separate processes: the stock handler must crash
when slot 0 is gone; the patched one must pass every scenario.
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]
PATCH_NAME = "esp-idf-6.0.2-sdmmc-isr-slot-dispatch.patch"
PATCH_PATH = ROOT / "patches" / PATCH_NAME
CMAKE = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
SOURCE_REL = "components/esp_driver_sdmmc/src/sd_host_sdmmc.c"
PATCH_MARKER = "An SDIO interrupt belongs to the slot that raised it"

registration = f"apply_idf_patch(patches/{PATCH_NAME})"
shared = "apply_idf_patch(patches/esp-idf-6.0.2-sdmmc-shared-controller.patch)"
assert registration in CMAKE
assert CMAKE.index(shared) < CMAKE.index(registration)

idf_path = os.environ.get("IDF_PATH")
if not idf_path or not (Path(idf_path) / SOURCE_REL).is_file():
    print(f"SKIP {Path(__file__).name}: needs IDF_PATH with {SOURCE_REL}")
    sys.exit(0)
compiler = shutil.which("cc") or shutil.which("gcc")
assert compiler, "no host C compiler"

HARNESS_HEAD = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Distinct bits in the positions the handler expects: SD events in the low
 * half, SDIO interrupts of slot 0/1 above it, DMA events in their own word. */
#define SOC_SDMMC_NUM_SLOTS 2
#define SDMMC_LL_SD_EVENT_MASK 0x0000ffffu
#define SDMMC_INTMASK_IO_SLOT0 (1u << 16)
#define SDMMC_INTMASK_IO_SLOT1 (1u << 17)
#define SDMMC_LL_EVENT_DMA_TI (1u << 0)
#define SDMMC_LL_EVENT_DMA_RI (1u << 1)
#define SDMMC_LL_EVENT_DMA_NI (1u << 8)
#define SDMMC_LL_EVENT_DMA_MASK 0x0000001fu
#define pdFALSE 0
#define pdTRUE 1

typedef struct { uint32_t status, mask, idsts; } fake_dev_t;
typedef struct { int unused; } sd_host_evt_data_t;
typedef struct { int unused; } sd_host_slot_t;
typedef bool (*evt_cb_t)(sd_host_slot_t *drv, const sd_host_evt_data_t *edata, void *user_data);
typedef struct { evt_cb_t on_trans_done; evt_cb_t on_io_interrupt; } sd_host_evt_cbs_t;
typedef struct {
    sd_host_slot_t drv;
    int slot_id;
    sd_host_evt_cbs_t cbs;
    void *user_data;
} sd_host_sdmmc_slot_t;
typedef struct { uint32_t sdmmc_status, dma_status; } sd_host_sdmmc_event_t;
typedef struct {
    struct { fake_dev_t *dev; } hal;
    sd_host_sdmmc_slot_t *slot[SOC_SDMMC_NUM_SLOTS];
    int cur_slot_id;
    void *event_queue;
    void *io_intr_sem;
    uint32_t lost_events;  /* esp-idf-6.0.2-sdmmc-timeout-recovery.patch */
} sd_host_sdmmc_ctlr_t;

static int s_queue_sends, s_sem_gives, s_yields;
static int s_io_calls[2], s_done_calls[2], s_refills[2], s_resumes[2];

#define portYIELD_FROM_ISR() (s_yields++)
/* Like the hardware's masked status register (mintsts). */
static uint32_t sdmmc_ll_get_intr_status(fake_dev_t *d) { return d->status & d->mask; }
static void sdmmc_ll_clear_interrupt(fake_dev_t *d, uint32_t m) { d->status &= ~m; }
static uint32_t sdmmc_ll_get_idsts_interrupt_raw(fake_dev_t *d) { return d->idsts; }
static void sdmmc_ll_clear_idsts_interrupt(fake_dev_t *d, uint32_t m) { d->idsts &= ~m; }
static void sdmmc_ll_enable_interrupt(fake_dev_t *d, uint32_t m, bool on)
{
    d->mask = on ? (d->mask | m) : (d->mask & ~m);
}
static int xQueueIsQueueFullFromISR(void *q) { (void)q; return 0; }
static int xQueueSendFromISR(void *q, const void *item, int *woken)
{
    (void)q; (void)item; (void)woken;
    s_queue_sends++;
    return 1;
}
static int xSemaphoreGiveFromISR(void *sem, int *woken)
{
    (void)sem;
    *woken = pdTRUE;
    s_sem_gives++;
    return 1;
}
/* The real helpers dereference the slot: a NULL slot must never get here. */
static size_t sd_host_get_free_descriptors_count(sd_host_sdmmc_slot_t *s)
{
    if (!s) {
        abort();
    }
    return 2;
}
static void sd_host_fill_dma_descriptors(sd_host_sdmmc_slot_t *s, size_t n)
{
    (void)n;
    s_refills[s->slot_id]++;
}
static void sd_host_dma_resume(sd_host_sdmmc_slot_t *s) { s_resumes[s->slot_id]++; }

static bool io_cb(sd_host_slot_t *drv, const sd_host_evt_data_t *e, void *user)
{
    (void)drv; (void)e;
    s_io_calls[*(int *)user]++;
    return false;
}
static bool done_cb(sd_host_slot_t *drv, const sd_host_evt_data_t *e, void *user)
{
    (void)drv; (void)e;
    s_done_calls[*(int *)user]++;
    return false;
}
"""

HARNESS_MAIN = r"""
static int s_ids[2] = {0, 1};
static sd_host_sdmmc_slot_t s_tf = {.slot_id = 0, .cbs = {done_cb, io_cb}, .user_data = &s_ids[0]};
static sd_host_sdmmc_slot_t s_c5 = {.slot_id = 1, .cbs = {done_cb, io_cb}, .user_data = &s_ids[1]};

static int check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s: queue=%d sem=%d io=[%d,%d] done=[%d,%d] refill=[%d,%d]\n", what,
               s_queue_sends, s_sem_gives, s_io_calls[0], s_io_calls[1],
               s_done_calls[0], s_done_calls[1], s_refills[0], s_refills[1]);
    }
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    fake_dev_t dev = {.mask = 0xffffffffu};
    sd_host_sdmmc_ctlr_t ctlr = {.hal = {&dev}};
    const char *scenario = argc > 1 ? argv[1] : "";

    if (strcmp(scenario, "removed_slot_c5_io") == 0) {
        /* TF slot 0 removed after a failed mount; the C5 raises SDIO. */
        ctlr.slot[1] = &s_c5;
        ctlr.cur_slot_id = 0;
        dev.status = SDMMC_INTMASK_IO_SLOT1;
        sd_host_isr(&ctlr);
        return check(s_sem_gives == 1 && s_io_calls[1] == 1 && s_io_calls[0] == 0,
                     "C5 SDIO interrupt after slot 0 removal");
    }
    if (strcmp(scenario, "both_slots_c5_io") == 0) {
        /* Both slots registered, the TF card did the last transaction. */
        ctlr.slot[0] = &s_tf;
        ctlr.slot[1] = &s_c5;
        ctlr.cur_slot_id = 0;
        dev.status = SDMMC_INTMASK_IO_SLOT1;
        sd_host_isr(&ctlr);
        return check(s_sem_gives == 1 && s_io_calls[1] == 1 && s_io_calls[0] == 0,
                     "C5 SDIO interrupt goes to the C5 slot");
    }
    if (strcmp(scenario, "tf_transfer_done") == 0) {
        /* A TF transfer completes: the event belongs to the current slot. */
        ctlr.slot[0] = &s_tf;
        ctlr.slot[1] = &s_c5;
        ctlr.cur_slot_id = 0;
        dev.status = 1u << 2;
        sd_host_isr(&ctlr);
        return check(s_queue_sends == 1 && s_done_calls[0] == 1 && s_done_calls[1] == 0,
                     "transfer event goes to the current slot");
    }
    if (strcmp(scenario, "removed_slot_transfer_event") == 0) {
        /* A late transfer status after slot 0 removal still reaches the queue. */
        ctlr.slot[1] = &s_c5;
        ctlr.cur_slot_id = 0;
        dev.status = 1u << 2;
        sd_host_isr(&ctlr);
        return check(s_queue_sends == 1 && s_done_calls[1] == 0,
                     "transfer status after slot 0 removal");
    }
    if (strcmp(scenario, "tf_io_slot0") == 0) {
        /* An SDIO interrupt of slot 0 while slot 1 did the last transaction. */
        ctlr.slot[0] = &s_tf;
        ctlr.slot[1] = &s_c5;
        ctlr.cur_slot_id = 1;
        dev.status = SDMMC_INTMASK_IO_SLOT0;
        sd_host_isr(&ctlr);
        return check(s_sem_gives == 1 && s_io_calls[0] == 1 && s_io_calls[1] == 0,
                     "slot 0 SDIO interrupt goes to slot 0");
    }
    if (strcmp(scenario, "both_io_bits") == 0) {
        ctlr.slot[0] = &s_tf;
        ctlr.slot[1] = &s_c5;
        ctlr.cur_slot_id = 0;
        dev.status = SDMMC_INTMASK_IO_SLOT0 | SDMMC_INTMASK_IO_SLOT1;
        sd_host_isr(&ctlr);
        return check(s_sem_gives == 1 && s_io_calls[0] == 1 && s_io_calls[1] == 1,
                     "both SDIO interrupts are delivered");
    }
    if (strcmp(scenario, "dma_refill_live_slot") == 0) {
        /* A DMA descriptor refill for the slot of the running transfer. */
        ctlr.slot[0] = &s_tf;
        ctlr.slot[1] = &s_c5;
        ctlr.cur_slot_id = 0;
        dev.idsts = SDMMC_LL_EVENT_DMA_NI | SDMMC_LL_EVENT_DMA_TI;
        sd_host_isr(&ctlr);
        return check(s_refills[0] == 1 && s_resumes[0] == 1 && s_refills[1] == 0,
                     "DMA refill for the current slot");
    }
    if (strcmp(scenario, "dma_ni_removed_slot") == 0) {
        /* A DMA status after slot 0 removal must not refill through it. */
        ctlr.slot[1] = &s_c5;
        ctlr.cur_slot_id = 0;
        dev.idsts = SDMMC_LL_EVENT_DMA_NI | SDMMC_LL_EVENT_DMA_TI;
        sd_host_isr(&ctlr);
        return check(s_refills[0] == 0 && s_refills[1] == 0,
                     "no DMA refill after slot 0 removal");
    }
    printf("unknown scenario %s\n", scenario);
    return 2;
}
"""

SCENARIOS = ["removed_slot_c5_io", "both_slots_c5_io", "tf_transfer_done",
             "removed_slot_transfer_event", "tf_io_slot0", "both_io_bits",
             "dma_refill_live_slot", "dma_ni_removed_slot"]


def extract_isr(source: str) -> str:
    start = source.index("static void sd_host_isr(void *arg)\n{")
    depth = 0
    for i in range(source.index("{", start), len(source)):
        depth += {"{": 1, "}": -1}.get(source[i], 0)
        if depth == 0:
            return source[start:i + 1]
    raise AssertionError("unterminated sd_host_isr")


def variants(work: Path) -> dict:
    """Return the stock and patched sd_host_sdmmc.c texts."""
    tree = work / "idf"
    target = tree / SOURCE_REL
    target.parent.mkdir(parents=True)
    shutil.copy(Path(idf_path) / SOURCE_REL, target)
    current = target.read_text(encoding="utf-8")
    direction = ["--reverse"] if PATCH_MARKER in current else []
    subprocess.run(["git", "apply", *direction, "--whitespace=nowarn", str(PATCH_PATH)],
                   cwd=tree, check=True)
    other = target.read_text(encoding="utf-8")
    patched, stock = (current, other) if direction else (other, current)
    assert PATCH_MARKER in patched and PATCH_MARKER not in stock
    return {"stock": stock, "patched": patched}


def run(work: Path, name: str, source: str) -> dict:
    c_file = work / f"{name}.c"
    exe = work / name
    c_file.write_text(HARNESS_HEAD + extract_isr(source) + HARNESS_MAIN, encoding="utf-8")
    subprocess.run([compiler, "-std=gnu11", "-O0", "-g", "-Wall", "-Werror",
                    "-Wno-unused-function", "-o", str(exe), str(c_file)], check=True)
    results = {}
    for scenario in SCENARIOS:
        proc = subprocess.run([str(exe), scenario], capture_output=True, text=True)
        results[scenario] = (proc.returncode, proc.stdout.strip())
    return results


with tempfile.TemporaryDirectory() as tmp:
    work = Path(tmp)
    texts = variants(work)
    stock = run(work, "stock", texts["stock"])
    patched = run(work, "patched", texts["patched"])

# The harness reproduces the device failure with the stock handler: a crash
# (signal, negative return code) when slot 0 is gone, and the C5 interrupt
# delivered to the other slot's callback while both slots exist; the
# transfer and DMA paths of a live slot work in both versions.
assert stock["removed_slot_c5_io"][0] < 0, stock["removed_slot_c5_io"]
assert stock["dma_ni_removed_slot"][0] < 0, stock["dma_ni_removed_slot"]
assert stock["both_slots_c5_io"][0] == 1, stock["both_slots_c5_io"]
assert stock["tf_io_slot0"][0] == 1, stock["tf_io_slot0"]
assert stock["tf_transfer_done"][0] == 0, stock["tf_transfer_done"]
assert stock["dma_refill_live_slot"][0] == 0, stock["dma_refill_live_slot"]

for scenario, (code, output) in patched.items():
    assert code == 0, f"patched {scenario}: rc={code} {output}"

print(f"PASS {Path(__file__).name}: stock crashes, patched handles all {len(SCENARIOS)} scenarios")

#!/usr/bin/env python3
"""Exercise actual capture synchronization when completion races its deadline."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = Path(os.environ.get("CSI_SYNC_TEST_SOURCE", root / "components/jetkvm_video/jetkvm_video.c")).read_text()
marker = "static esp_err_t csi_sync_rx_to_next_frame(app_ctx_t *ctx)\n{"
start = source.index(marker)
pos = source.index("{", start) + 1
depth = 1
while depth:
    depth += (source[pos] == "{") - (source[pos] == "}")
    pos += 1
function = source[start:pos]
timeout_us = int(re.search(r"#define CSI_FRAME_SYNC_TIMEOUT_US\s+(\d+)", source).group(1))
prefix = f"#define CSI_FRAME_SYNC_TIMEOUT_US {timeout_us}\n" + r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define BIT(n) (1u << (n))
#define ESP_OK 0
#define ESP_ERR_TIMEOUT -1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;
typedef struct { int marker; } app_ctx_t;
static struct { struct { volatile uint32_t val; } int_clr, int_raw; } ISP;
static int64_t now;
static unsigned clock_reads, restarts, delays;
static bool finish_during_clock_read, finish_during_delay;
static esp_err_t restart_result;
static int64_t esp_timer_get_time(void) {
    if (++clock_reads == 2 && finish_during_clock_read) {
        now = CSI_FRAME_SYNC_TIMEOUT_US + 25000; /* The task resumes after its deadline. */
        ISP.int_raw.val |= BIT(28);
    }
    return now;
}
static void esp_rom_delay_us(unsigned us) {
    assert(us == 20);
    now += 1000;
    ++delays;
    if (finish_during_delay && delays == 2) ISP.int_raw.val |= BIT(28);
}
static esp_err_t csi_restart_rx(app_ctx_t *ctx) {
    assert(ctx->marker == 42 && (ISP.int_raw.val & BIT(28)));
    ++restarts;
    return restart_result;
}
static void reset(void) {
    ISP.int_raw.val = ISP.int_clr.val = 0;
    now = clock_reads = restarts = delays = 0;
    finish_during_clock_read = finish_during_delay = false;
    restart_result = ESP_OK;
}
'''
suffix = r'''int main(void) {
    app_ctx_t ctx = {42};
    reset(); finish_during_clock_read = true;
    assert(csi_sync_rx_to_next_frame(&ctx) == ESP_OK);
    assert(restarts == 1 && delays == 0 && ISP.int_clr.val == BIT(28));
    reset(); finish_during_delay = true;
    assert(csi_sync_rx_to_next_frame(&ctx) == ESP_OK);
    assert(restarts == 1 && delays == 2);
    reset();
    assert(csi_sync_rx_to_next_frame(&ctx) == ESP_ERR_TIMEOUT);
    assert(restarts == 0 && delays == CSI_FRAME_SYNC_TIMEOUT_US / 1000);
    reset(); finish_during_clock_read = true; restart_result = -2;
    assert(csi_sync_rx_to_next_frame(&ctx) == -2);
    assert(restarts == 1);
    puts("CSI sync: deadline-racing completion accepted; absent frames time out; restart errors preserved: PASS");
}
'''
with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    c.write_text(prefix + function + suffix)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

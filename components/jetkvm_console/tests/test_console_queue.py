#!/usr/bin/env python3
"""Build the production jetkvm_console.c on the host against a fake FreeRTOS
ring buffer and ESP log hook, and check that a log call never waits for the
console: it queues, or drops and counts when the queue is full, and cuts a
message too long for the queue. The flush sends later messages direct and
waits until the console task has written the queued ones."""
from pathlib import Path
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]

stubs = {
    "esp_err.h": "#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NO_MEM 0x101\n",
    "esp_heap_caps.h": "#pragma once\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n",
    "esp_log.h": r'''
#pragma once
#include <stdarg.h>
#include <stdint.h>
typedef int (*vprintf_like_t)(const char *, va_list);
vprintf_like_t esp_log_set_vprintf(vprintf_like_t func);
uint32_t esp_log_timestamp(void);
''',
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
    "esp_rom_serial_output.h": "#pragma once\n#include <stdint.h>\nvoid esp_rom_output_tx_wait_idle(uint8_t n);\n#define CONFIG_ESP_CONSOLE_ROM_SERIAL_PORT_NUM 0\n",
    "freertos/FreeRTOS.h": r'''
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t UBaseType_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 0xffffffffU
#define tskIDLE_PRIORITY 0U
#define tskNO_AFFINITY 0x7fffffff
''',
    "freertos/task.h": r'''
#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name,
    uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *handle,
    BaseType_t core, uint32_t caps);
void vTaskDelay(TickType_t ticks);
''',
    "freertos/ringbuf.h": r'''
#pragma once
#include "freertos/FreeRTOS.h"
typedef struct fake_ring *RingbufHandle_t;
#define RINGBUF_TYPE_NOSPLIT 0
RingbufHandle_t xRingbufferCreateWithCaps(size_t size, int type, uint32_t caps);
void vRingbufferDeleteWithCaps(RingbufHandle_t ring);
size_t xRingbufferGetMaxItemSize(RingbufHandle_t ring);
BaseType_t xRingbufferSendAcquire(RingbufHandle_t ring, void **item, size_t size, TickType_t wait);
BaseType_t xRingbufferSendComplete(RingbufHandle_t ring, void *item);
void *xRingbufferReceive(RingbufHandle_t ring, size_t *size, TickType_t wait);
void vRingbufferReturnItem(RingbufHandle_t ring, void *item);
''',
}

test = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../jetkvm_console.c"   /* static functions are part of the test */

/* A ring of whole items with an 8-byte header each, like NOSPLIT. */
#define MAX_ITEMS 64
struct fake_ring { size_t capacity, used; int head, count; char *items[MAX_ITEMS]; size_t sizes[MAX_ITEMS]; };
static struct fake_ring ring;
static int ring_waits, direct_calls, task_fail, created, delays, idle_waits;
static int64_t now_us = 5000000;
static vprintf_like_t current;

RingbufHandle_t xRingbufferCreateWithCaps(size_t size, int type, uint32_t caps)
{ (void)type; (void)caps; memset(&ring, 0, sizeof(ring)); ring.capacity = size; return &ring; }
void vRingbufferDeleteWithCaps(RingbufHandle_t r) { (void)r; }
size_t xRingbufferGetMaxItemSize(RingbufHandle_t r) { return r->capacity / 2 - 8; }
BaseType_t xRingbufferSendAcquire(RingbufHandle_t r, void **item, size_t size, TickType_t wait)
{
    if (wait != 0) ring_waits++;
    assert(size <= xRingbufferGetMaxItemSize(r));
    if (r->used + size + 8 > r->capacity || r->count == MAX_ITEMS) return 0;
    int slot = (r->head + r->count) % MAX_ITEMS;
    r->items[slot] = calloc(1, size); r->sizes[slot] = size; r->used += size + 8; r->count++;
    *item = r->items[slot]; return pdTRUE;
}
BaseType_t xRingbufferSendComplete(RingbufHandle_t r, void *item) { (void)r; (void)item; return pdTRUE; }
void *xRingbufferReceive(RingbufHandle_t r, size_t *size, TickType_t wait)
{
    (void)wait;
    if (r->count == 0) return NULL;
    *size = r->sizes[r->head]; return r->items[r->head];
}
void vRingbufferReturnItem(RingbufHandle_t r, void *item)
{
    assert(item == r->items[r->head]);
    r->used -= r->sizes[r->head] + 8; free(item);
    r->head = (r->head + 1) % MAX_ITEMS; r->count--;
}
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack,
    void *arg, UBaseType_t prio, TaskHandle_t *handle, BaseType_t core, uint32_t caps)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)handle; (void)core; (void)caps;
  created++; return task_fail ? 0 : pdPASS; }
/* The flush waits by sleeping; while it sleeps the console task writes one message. */
void vTaskDelay(TickType_t ticks) { (void)ticks; delays++; now_us += 1000; write_next(0); }
int64_t esp_timer_get_time(void) { return now_us; }
void esp_rom_output_tx_wait_idle(uint8_t n) { assert(n == 0); idle_waits++; }

static int direct(const char *format, va_list args)
{ direct_calls++; printf("[direct]"); return vprintf(format, args); }
vprintf_like_t esp_log_set_vprintf(vprintf_like_t func) { vprintf_like_t old = current; current = func; return old; }
uint32_t esp_log_timestamp(void) { return 42; }

static void log_line(const char *format, ...)
{ va_list args; va_start(args, format); current(format, args); va_end(args); }

static char *captured(void)
{
    fflush(stdout);
    FILE *f = fopen(getenv("CONSOLE_OUT"), "rb"); assert(f);
    static char buf[1 << 16]; size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f);
    return buf;
}

int main(void)
{
    assert(freopen(getenv("CONSOLE_OUT"), "w", stdout));
    current = direct;

    /* A failed task start leaves the blocking path in place. */
    task_fail = 1;
    assert(jetkvm_console_start() == ESP_ERR_NO_MEM && current == direct);
    task_fail = 0;
    assert(jetkvm_console_start() == ESP_OK && current != direct && created == 2);

    /* Messages are queued, not written, and a log call never waits. */
    log_line("I (1) a: first %d\n", 1);
    log_line("I (2) b: second %s\n", "two");
    assert(strlen(captured()) == 0 && ring.count == 2);

    /* The console task writes them in order. */
    write_next(0);
    write_next(0);
    assert(strcmp(captured(), "I (1) a: first 1\nI (2) b: second two\n") == 0);

    /* A full queue drops and counts; the log call still does not wait. */
    char line[200]; memset(line, 'x', sizeof(line) - 2); line[sizeof(line) - 2] = '\n'; line[sizeof(line) - 1] = 0;
    for (int i = 0; i < 200; i++) log_line("%s", line);
    int dropped = 200 - ring.count;
    assert(dropped > 0 && ring_waits == 0 && direct_calls == 0);

    /* The drop count is reported once when the task next writes... */
    write_next(0);
    char expected[128];
    snprintf(expected, sizeof(expected), "W (42) console: %d log messages dropped; the console was busy\n", dropped);
    assert(strstr(captured(), expected));
    /* ...and new drops within a second wait for the next report. */
    ring.capacity = ring.used; log_line("I (9) z: lost\n"); ring.capacity = 16 * 1024;
    write_next(0);
    assert(strstr(captured(), "W (42) console: 1 log") == NULL);

    while (ring.count > 0) write_next(0);

    /* A message longer than the queue can hold is cut and ends with a newline. */
    static char huge[16 * 1024]; memset(huge, 'h', sizeof(huge) - 2); huge[sizeof(huge) - 2] = '\n';
    log_line("%s", huge);
    int before = ring.count; assert(before > 0);
    size_t cut = ring.sizes[(ring.head + ring.count - 1) % MAX_ITEMS];
    char *last = ring.items[(ring.head + ring.count - 1) % MAX_ITEMS];
    assert(cut == xRingbufferGetMaxItemSize(&ring) && last[cut - 2] == '\n' && last[cut - 1] == 0);
    assert(direct_calls == 0);

    /* The flush switches to direct output, waits until the console task has
     * written everything queued, reports the pending drop, and waits for the UART. */
    int queued = ring.count;
    jetkvm_console_flush();
    assert(ring.count == 0 && delays == queued && current == direct && idle_waits == 1);
    assert(strstr(captured(), "W (42) console: 1 log messages dropped"));
    log_line("I (3) c: after\n");
    assert(direct_calls == 1 && strstr(captured(), "[direct]I (3) c: after\n"));

    /* A reservation that is never completed cannot hold the flush past its deadline. */
    current = queue_vprintf;
    __atomic_fetch_add(&s_reserved, 1U, __ATOMIC_ACQ_REL);
    delays = 0;
    jetkvm_console_flush();
    assert(delays == CONSOLE_FLUSH_DEADLINE_US / 1000);
    return 0;
}
'''

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    for name, text in stubs.items():
        path = tmp / "stubs" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    source = component / "tests" / "_console_test.c"
    try:
        source.write_text(test)
        binary = tmp / "test"
        subprocess.run(
            ["cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-I", str(tmp / "stubs"), "-I", str(component / "include"),
             str(source), "-o", str(binary)],
            check=True)
        subprocess.run([str(binary)], check=True, env={"CONSOLE_OUT": str(tmp / "out.txt")})
    finally:
        source.unlink(missing_ok=True)
print("jetkvm_console queue test passed")

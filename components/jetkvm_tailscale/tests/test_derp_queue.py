#!/usr/bin/env python3
"""Exercise production relay enqueue ordering and congestion feedback."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = Path(os.environ.get("DERP_QUEUE_TEST_SOURCE", root / "third_party/microlink/components/microlink/src/ml_derp.c")).read_text()

def function(name, code=source):
    m = re.search(r"^(?:static )?[^\n]+\b" + name + r"\([^;]*?\)\s*\{", code, re.M)
    assert m, name
    pos, depth = m.end(), 1
    while depth:
        depth += (code[pos] == "{") - (code[pos] == "}")
        pos += 1
    return code[m.start():pos]

header = Path(os.environ.get("DERP_QUEUE_TEST_HEADER", root / "third_party/microlink/components/microlink/include/microlink_internal.h")).read_text()
depth_define = re.search(r"^#define ML_DERP_TX_QUEUE_DEPTH\s+[^\n]+", header, re.M)[0]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG -1
#define ESP_ERR_NO_MEM -2
#define ESP_ERR_TIMEOUT -3
#define DERP_FRAME_SEND_PACKET 4
#define pdTRUE 1
#define ML_DERP_TX_QUEUE_DEPTH 512
#define MAX_PACKET 1500
#define CONFIG_SPIRAM 1
#define MALLOC_CAP_SPIRAM 4
#define MALLOC_CAP_8BIT 8
typedef int esp_err_t;
typedef struct { uint8_t *data; size_t len; uint8_t dest_pubkey[32], frame_type; } ml_derp_tx_item_t;
typedef struct { ml_derp_tx_item_t items[ML_DERP_TX_QUEUE_DEPTH]; unsigned count, capacity; } queue_t;
typedef struct { queue_t *derp_tx_queue; uint32_t derp_tx_queue_peak, derp_tx_queue_dropped; } microlink_t;
static unsigned live_packets, live_bytes, maximum_bytes;
static bool fail_allocate;
static void *heap_caps_malloc(size_t n, unsigned caps) {
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (fail_allocate) return NULL;
    assert(n <= MAX_PACKET);
    size_t *p = malloc(sizeof(*p) + n);
    assert(p); *p = n;
    ++live_packets; live_bytes += n;
    if (live_bytes > maximum_bytes) maximum_bytes = live_bytes;
    return p + 1;
}
static void packet_free(void *p) {
    if (!p) return;
    size_t *owned = (size_t *)p - 1;
    assert(live_packets && live_bytes >= *owned);
    --live_packets; live_bytes -= *owned; free(owned);
}
#define free packet_free
static unsigned internal_allocations;
void *internal_packet_malloc(size_t n) { (void)n; ++internal_allocations; return NULL; }
#define malloc internal_packet_malloc
static unsigned uxQueueMessagesWaiting(queue_t *q) { return q->count; }
static int xQueueReceive(queue_t *q, ml_derp_tx_item_t *item, unsigned ticks) {
    assert(ticks == 0);
    if (!q->count) return 0;
    *item = q->items[0];
    --q->count;
    memmove(q->items, q->items + 1, q->count * sizeof(*item));
    return pdTRUE;
}
static int xQueueSend(queue_t *q, const ml_derp_tx_item_t *item, unsigned ticks) {
    assert(ticks == 0);
    if (q->count == q->capacity) return 0;
    q->items[q->count++] = *item;
    return pdTRUE;
}
static int xQueueSendToFront(queue_t *q, const ml_derp_tx_item_t *item, unsigned ticks) {
    assert(ticks == 0);
    if (q->count == q->capacity) return 0;
    memmove(q->items + 1, q->items, q->count * sizeof(*item));
    q->items[0] = *item; ++q->count;
    return pdTRUE;
}
'''
suffix = r'''
static void drain(queue_t *q) {
    ml_derp_tx_item_t item;
    while (xQueueReceive(q, &item, 0)) free(item.data);
    assert(!live_packets && !live_bytes);
}
int main(void) {
    queue_t q = {.capacity = 2};
    microlink_t ml = {.derp_tx_queue = &q};
    uint8_t key[32] = {0x73}, data[MAX_PACKET] = {4,0,0,0};
    data[4] = 1; assert(ml_derp_queue_send(&ml,key,data,sizeof(data)) == ESP_OK);
    data[4] = 2; assert(ml_derp_queue_send(&ml,key,data,sizeof(data)) == ESP_OK);
    data[4] = 3; assert(ml_derp_queue_send(&ml,key,data,sizeof(data)) == ESP_ERR_TIMEOUT);
    assert(q.count == 2 && q.items[0].data[4] == 1 && q.items[1].data[4] == 2);
    assert(ml.derp_tx_queue_dropped == 1 && ml.derp_tx_queue_peak == 2);
    ml_derp_tx_item_t out;
    assert(xQueueReceive(&q,&out,0));
    assert(out.len == sizeof(data) && !memcmp(out.dest_pubkey,key,32)); free(out.data);
    assert(ml_derp_queue_send(&ml,key,data,sizeof(data)) == ESP_OK);
    assert(q.items[0].data[4] == 2 && q.items[1].data[4] == 3);
    data[0] = 2; data[4] = 9;
    assert(ml_derp_queue_send(&ml,key,data,148) == ESP_OK);
    assert(q.count == 2 && q.items[0].data[0] == 2 && q.items[1].data[4] == 3);
    drain(&q);
    data[0] = 4;
    fail_allocate = true;
    assert(ml_derp_queue_send(&ml,key,data,100) == ESP_ERR_NO_MEM);
    assert(!q.count && !live_packets && !internal_allocations);
    fail_allocate = false;
    q.capacity = ML_DERP_TX_QUEUE_DEPTH;
    /* Largest encoded access unit: ~400 MTU packets, without losing the
     * beginning of its keyframe while the relay task is waiting to run. */
    for (unsigned round=0; round<100; ++round) {
        for (unsigned i=0; i<400; ++i) {
            memcpy(data+4,&i,sizeof(i));
            assert(ml_derp_queue_send(&ml,key,data,sizeof(data)) == ESP_OK);
        }
        for (unsigned i=0; i<400; ++i) {
            assert(xQueueReceive(&q,&out,0));
            unsigned actual; memcpy(&actual,out.data+4,sizeof(actual));
            assert(actual == i && out.len == sizeof(data)); free(out.data);
        }
        assert(!q.count && !live_packets && !live_bytes);
    }
    for (unsigned i=0; i<ML_DERP_TX_QUEUE_DEPTH; ++i)
        assert(ml_derp_queue_send(&ml,key,data,sizeof(data)) == ESP_OK);
    assert(ml_derp_queue_send(&ml,key,data,sizeof(data)) == ESP_ERR_TIMEOUT);
    assert(q.count == ML_DERP_TX_QUEUE_DEPTH);
    drain(&q);
    assert(maximum_bytes <= (ML_DERP_TX_QUEUE_DEPTH+1)*MAX_PACKET);
    puts("Relay queue: complete keyframe burst, FIFO preserved under congestion, feedback and handshake priority, bounded memory: PASS");
}
'''
with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / "queue.c"
    binary = Path(directory) / "queue"
    c.write_text(prefix.replace("#define ML_DERP_TX_QUEUE_DEPTH 512", depth_define) + function("ml_packet_malloc", header) + function("note_derp_queue_depth") + function("ml_derp_queue_send") + suffix)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(c), "-o", str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

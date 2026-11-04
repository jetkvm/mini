#!/usr/bin/env python3
"""Verify live arena snapshots avoid heap walks and expose cache freshness."""
from pathlib import Path
import re
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
source = (component / 'peer_memory_arena.c').read_text()
source = source[:source.index('/* Strong definitions')]
source = re.sub(r'^#include .+\n', '', source, flags=re.M)
header = (component / 'peer_memory_arena.h').read_text()
snapshot = re.search(r'typedef struct \{.*?\} webrtc_peer_memory_arena_snapshot_t;', header, re.S)[0]
stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
typedef int portMUX_TYPE;
typedef void *multi_heap_handle_t;
typedef struct {size_t total_free_bytes, minimum_free_bytes, largest_free_block,
 allocated_blocks, total_allocated_bytes;} multi_heap_info_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_TIMEOUT 4
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) do {assert(*(lock)==0);++*(lock);} while(0)
#define portEXIT_CRITICAL(lock) do {assert(*(lock)==1);--*(lock);} while(0)
#define ESP_LOGI(tag,...) ((void)(tag))
#define ESP_LOGE(tag,...) ((void)(tag))
static unsigned walks;
static uint64_t now_us=100;
static bool invalidate_during_walk;
static multi_heap_info_t fake_info={.total_free_bytes=1024,.minimum_free_bytes=1024,.largest_free_block=1024};
static void invalidate_detail_cache(void);
static int64_t esp_timer_get_time(void){return now_us;}
static void *heap_caps_aligned_alloc(size_t alignment,size_t size,int caps)
{(void)alignment;(void)caps;return malloc(size);}
static void heap_caps_free(void *p){free(p);}
static void *multi_heap_register(void *base,size_t size){(void)size;return base;}
static void multi_heap_set_lock(void *heap,void *lock){(void)heap;(void)lock;}
static void multi_heap_get_info(void *heap,multi_heap_info_t *info)
{
 assert(heap);++walks;*info=fake_info;
 if(invalidate_during_walk){invalidate_detail_cache();invalidate_during_walk=false;}
}
static size_t multi_heap_free_size(void *heap){assert(heap);return fake_info.total_free_bytes;}
static size_t multi_heap_minimum_free_size(void *heap){assert(heap);return fake_info.minimum_free_bytes;}
static void vTaskDelay(unsigned ticks){(void)ticks;}
'''
prototypes = r'''
esp_err_t webrtc_peer_memory_arena_end(void);
void webrtc_peer_memory_arena_get_snapshot(webrtc_peer_memory_arena_snapshot_t *snapshot);
'''
checks = r'''
int main(void)
{
 webrtc_peer_memory_arena_snapshot_t s;
 webrtc_peer_memory_arena_get_snapshot_cached(&s);
 assert(!s.active && !s.detailed_sample_timestamp_us && walks==0);
 assert(webrtc_peer_memory_arena_begin(2048)==ESP_OK && walks==1);
 webrtc_peer_memory_arena_get_snapshot_cached(&s);
 assert(s.active && s.capacity_bytes==2048 && s.largest_free_block==1024);
 assert(s.detailed_sample_timestamp_us==100 && walks==1);
 fake_info.total_free_bytes=700;fake_info.minimum_free_bytes=650;
 fake_info.largest_free_block=333;fake_info.allocated_blocks=9;
 now_us=200;
 webrtc_peer_memory_arena_get_snapshot_cached(&s);
 assert(s.free_bytes==700 && s.minimum_free_bytes==650);
 assert(s.largest_free_block==1024 && s.allocated_blocks==0);
 assert(s.detailed_sample_timestamp_us==100 && walks==1);
 webrtc_peer_memory_arena_get_snapshot(&s);
 assert(s.largest_free_block==333 && s.allocated_blocks==9);
 assert(s.detailed_sample_timestamp_us==200 && walks==2);
 webrtc_peer_memory_arena_get_snapshot_cached(&s);
 assert(s.largest_free_block==333 && s.detailed_sample_timestamp_us==200 && walks==2);
 invalidate_during_walk=true;now_us=300;
 webrtc_peer_memory_arena_get_snapshot(&s);
 webrtc_peer_memory_arena_get_snapshot_cached(&s);
 assert(!s.detailed_sample_timestamp_us && !s.largest_free_block);
 fake_info.allocated_blocks=0;
 assert(webrtc_peer_memory_arena_end()==ESP_OK);
 webrtc_peer_memory_arena_get_snapshot_cached(&s);
 assert(!s.active && !s.detailed_sample_timestamp_us);
 now_us=400;fake_info.largest_free_block=800;
 assert(webrtc_peer_memory_arena_begin(3072)==ESP_OK);
 webrtc_peer_memory_arena_get_snapshot_cached(&s);
 assert(s.capacity_bytes==3072 && s.largest_free_block==800);
 assert(s.detailed_sample_timestamp_us==400);
 assert(webrtc_peer_memory_arena_end()==ESP_OK);
 puts("peer memory snapshots: no live heap walk, fresh free/min, timestamp and generation invalidation: OK");
}
'''
with tempfile.TemporaryDirectory(prefix='webrtc-peer-snapshot-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(stubs + snapshot + prototypes + source + checks)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-O2', str(path / 'test.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)

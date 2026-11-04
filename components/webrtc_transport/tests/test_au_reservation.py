#!/usr/bin/env python3
"""Exercise production AU lease, generation, FIFO and DMA cache transitions."""
from pathlib import Path
import re
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
source = (component / 'webrtc_transport.c').read_text()
header = (component / 'include/webrtc_transport.h').read_text()
internal = (component / 'webrtc_transport_ctx_internal.h').read_text()

def function(signature):
    start = source.index(signature)
    pos = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]

def structure(text, name):
    return re.search(r'typedef struct \{(?:(?!typedef struct).)*?\} '+name+r';', text, re.S)[0]

functions = '\n'.join(function(s) for s in (
    'static bool starts_with_annexb(',
    'static void inspect_annexb(',
    'static void flush_ready_slots_locked(',
    'static int reserve_slot_locked(',
    'static bool service_video_locked(',
    'static int take_next_ready_slot(',
    'static TaskHandle_t free_sending_slot_locked(',
    'static void release_sending_slot(',
    'static bool reservation_matches_locked(',
    'static bool finish_access_unit(',
    'static esp_err_t lease_access_unit(',
    'bool webrtc_transport_reserve_access_unit(',
    'esp_err_t webrtc_transport_reserve_guest_access_unit(',
    'void webrtc_transport_cancel_access_unit(',
    'bool webrtc_transport_commit_access_unit(',
    'static void note_cpu_writes(',
    'bool webrtc_transport_commit_cpu_access_unit(',
    'void webrtc_transport_cancel_cpu_access_unit(',
    'static bool webrtc_transport_send_annexb_traced(',
))
stat_fields = sorted(set(re.findall(r'ctx->stats\.(\w+)', functions)) - {'stage_age'})
types = '\n'.join([
    re.search(r'typedef enum \{[^}]+\} au_slot_state_t;', internal)[0],
    structure(internal, 'au_slot_t'),
    structure(header, 'app_h264_frame_meta_t'),
    structure(header, 'webrtc_transport_au_reservation_t'),
])
preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_WEBRTC_TRANSPORT_AU_SLOTS 2
#define CONFIG_WEBRTC_TRANSPORT_MAX_AU_BYTES 512
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
typedef void *TaskHandle_t;
static int s_task, notified;
static TaskHandle_t xTaskGetCurrentTaskHandle(void) {return &s_task;}
static void xTaskNotifyGive(TaskHandle_t task) {assert(task==&s_task); ++notified;}
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 4
typedef int esp_err_t;
static int lock_depth;
#define portENTER_CRITICAL(mux) do {(void)(mux); assert(!lock_depth); ++lock_depth;} while(0)
#define portEXIT_CRITICAL(mux) do {(void)(mux); assert(lock_depth==1); --lock_depth;} while(0)
'''
context = r'''
typedef struct {
    int lock;
    au_slot_t slots[2];
    bool started, session_active, mode_valid, video_paused, waiting_for_sync, have_last_pts, guest_mode;
    uint16_t width, height, fps;
    uint32_t generation, video_epoch, active_peer_generation, submitters;
    TaskHandle_t slot_waiter;
    uint32_t sync_idr_inflight_generation, sync_idr_committed_generation;
    uint64_t next_au_order;
    struct { struct {uint32_t epoch;} stage_age;
        /* STATS */
    } stats;
} transport_ctx_t;
static transport_ctx_t s_ctx;
static unsigned notifications, clean_calls;
static size_t cleaned_bytes;
static int clean_error;
static void notify_peer_task(transport_ctx_t *ctx) {(void)ctx; assert(!lock_depth); ++notifications;}
static int64_t esp_timer_get_time(void) {return 2000;}
static esp_err_t esp_cache_msync(void *data,size_t size,int flags)
{
    assert(!lock_depth && data && size%128==0 && flags==ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    assert(s_ctx.submitters>0); ++clean_calls; cleaned_bytes=size; return clean_error;
}
static esp_err_t webrtc_transport_set_mode(uint16_t width,uint16_t height,uint16_t fps)
{
    if(!s_ctx.started) return -1;
    s_ctx.width=width;s_ctx.height=height;s_ctx.fps=fps;s_ctx.mode_valid=true;return 0;
}
void webrtc_transport_cancel_access_unit(webrtc_transport_au_reservation_t *reservation);
'''.replace('/* STATS */', '\n'.join('uint64_t '+f+';' for f in stat_fields))
checks = r'''
static const uint8_t idr[] = {0,0,0,1,0x67,0x42,0,0x1f,0,0,0,1,0x68,0x12,0,0,0,1,0x65,1};
static app_h264_frame_meta_t meta = {.width=1920,.height=1080,.fps=30,
 .keyframe=true,.rtp_timestamp_90khz=90000,.capture_sequence=7,
 .capture_eof_timestamp_us=1000,.encode_start_timestamp_us=1100,.encode_done_timestamp_us=1800};
static void reset(void)
{
 static uint8_t pool[2][512];
 s_ctx=(transport_ctx_t){.started=true,.session_active=true,.generation=7,.active_peer_generation=7};
 for(unsigned i=0;i<2;i++){s_ctx.slots[i].data=s_ctx.slots[i].own=pool[i];s_ctx.slots[i].capacity=512;}
 s_ctx.stats.stage_age.epoch=3;
 notifications=clean_calls=notified=0;cleaned_bytes=0;clean_error=0;
}
static void fill(webrtc_transport_au_reservation_t *r){memcpy(r->data,idr,sizeof(idr));}
int main(void)
{
 webrtc_transport_au_reservation_t a={0},b={0},none={0};
 reset();
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&a));
 assert(a.capacity==512 && s_ctx.submitters==1 && s_ctx.stats.access_units_offered==0);
 webrtc_transport_au_reservation_t unchanged=a;
 assert(!webrtc_transport_reserve_access_unit(1920,1080,30,&a));
 assert(memcmp(&a,&unchanged,sizeof(a))==0 && s_ctx.submitters==1);
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&b));
 assert(!webrtc_transport_reserve_access_unit(1920,1080,30,&none));
 assert(!none.data && s_ctx.submitters==2 && clean_calls==0);
 fill(&b);assert(webrtc_transport_commit_access_unit(&b,sizeof(idr),&meta));
 assert(!b.data && s_ctx.submitters==1 && take_next_ready_slot(&s_ctx, false)==-1);
 fill(&a);assert(webrtc_transport_commit_access_unit(&a,sizeof(idr),&meta));
 assert(!a.data && s_ctx.submitters==0 && s_ctx.stats.access_units_offered==2);
 assert(s_ctx.stats.access_units_enqueued==2);
 assert(take_next_ready_slot(&s_ctx, false)==0);release_sending_slot(&s_ctx,0);
 assert(take_next_ready_slot(&s_ctx, false)==1);release_sending_slot(&s_ctx,1);
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&a));
 assert(clean_calls==0); /* DMA-only reuse has no full-buffer cache walk. */
 webrtc_transport_au_reservation_t stale=a;
 flush_ready_slots_locked(&s_ctx);
 assert(s_ctx.slots[a.slot].state==AU_SLOT_FILLING && s_ctx.submitters==1);
 webrtc_transport_cancel_access_unit(&a);
 assert(!a.data && s_ctx.submitters==0);
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&a));
 webrtc_transport_cancel_access_unit(&stale); /* Old token cannot release reuse. */
 assert(s_ctx.submitters==1 && s_ctx.slots[a.slot].state==AU_SLOT_FILLING);
 fill(&a);s_ctx.generation++;
 assert(!webrtc_transport_commit_access_unit(&a,sizeof(idr),&meta));
 assert(!a.data && s_ctx.submitters==0 && s_ctx.stats.access_units_no_session==1);
 /* A mode change keeps the peer generation; the slot's video epoch is stale. */
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&a));
 fill(&a);s_ctx.video_epoch++;
 assert(!webrtc_transport_commit_access_unit(&a,sizeof(idr),&meta));
 assert(!a.data && s_ctx.submitters==0 && s_ctx.stats.access_units_no_session==2);
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&a));
 fill(&a);s_ctx.started=false;
 assert(!webrtc_transport_commit_access_unit(&a,sizeof(idr),&meta));
 assert(s_ctx.submitters==0); /* Stop can now reclaim the pool. */

 reset();
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&a));fill(&a);
 uint8_t *sending_data=a.data;
 assert(webrtc_transport_commit_access_unit(&a,sizeof(idr),&meta));
 assert(take_next_ready_slot(&s_ctx, false)==0);
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&b));fill(&b);
 assert(webrtc_transport_commit_access_unit(&b,sizeof(idr),&meta));
 s_ctx.video_paused=true;flush_ready_slots_locked(&s_ctx);
 assert(s_ctx.slots[0].state==AU_SLOT_SENDING && s_ctx.slots[1].state==AU_SLOT_FREE);
 s_ctx.video_paused=false;
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&a));
 assert(a.slot==1 && a.data!=sending_data);
 memset(a.data,42,a.capacity); /* New DMA output cannot overwrite the sender. */
 assert(memcmp(sending_data,idr,sizeof(idr))==0);
 assert(!webrtc_transport_reserve_access_unit(1920,1080,30,&b));
 release_sending_slot(&s_ctx,0);
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&b) && b.slot==0);
 webrtc_transport_cancel_access_unit(&a);webrtc_transport_cancel_access_unit(&b);

 reset();
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&a));
 assert(!webrtc_transport_commit_access_unit(&a,513,&meta));
 assert(!a.data && s_ctx.stats.access_units_oversize==1 && s_ctx.submitters==0);
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&a));
 a.data[0]=42;
 assert(!webrtc_transport_commit_access_unit(&a,1,&meta));
 assert(s_ctx.stats.access_units_bad_annexb==1 && s_ctx.submitters==0);

 reset();
 s_ctx.waiting_for_sync=true;s_ctx.sync_idr_inflight_generation=7;meta.sync_idr_generation=7;
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&a));fill(&a);
 assert(webrtc_transport_commit_access_unit(&a,sizeof(idr),&meta));
 assert(s_ctx.sync_idr_inflight_generation==0 && s_ctx.sync_idr_committed_generation==7);
 assert(s_ctx.stats.sync_idr_committed==1 && s_ctx.slots[0].sync_idr_generation==7);
 meta.sync_idr_generation=0;

 reset();
 assert(webrtc_transport_set_mode(1920,1080,30)==0);
 s_ctx.video_epoch=5; /* A copied AU takes the current epoch too. */
 uint8_t copied[200]={0,0,0,1,0x61,1};
 assert(webrtc_transport_send_annexb_traced(copied,sizeof(copied),90000,false,&meta));
 assert(s_ctx.slots[0].cpu_dirty_bytes==200);
 assert(take_next_ready_slot(&s_ctx, false)==0);release_sending_slot(&s_ctx,0);
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&a));
 assert(clean_calls==1 && cleaned_bytes==256 && s_ctx.slots[0].cpu_dirty_bytes==0);
 webrtc_transport_cancel_access_unit(&a);
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&a));
 assert(clean_calls==1);webrtc_transport_cancel_access_unit(&a);
 s_ctx.slots[0].cpu_dirty_bytes=384;clean_error=-1;
 assert(!webrtc_transport_reserve_access_unit(1920,1080,30,&a));
 assert(!a.data && s_ctx.submitters==0 && s_ctx.slots[0].cpu_dirty_bytes==384);
 assert(cleaned_bytes==384);

 /* Guest leases: the mode is set elsewhere; a full pool parks the caller
  * until the peer releases a slot. */
 reset();
 assert(webrtc_transport_reserve_guest_access_unit(1920,1080,30,&a)==ESP_ERR_INVALID_STATE);
 assert(!a.data && s_ctx.submitters==0);
 assert(webrtc_transport_set_mode(1920,1080,30)==0);
 assert(webrtc_transport_reserve_guest_access_unit(1280,720,60,&a)==ESP_ERR_INVALID_STATE);
 assert(webrtc_transport_reserve_guest_access_unit(1920,1080,30,&a)==ESP_OK);
 unchanged=a;
 assert(webrtc_transport_reserve_guest_access_unit(1920,1080,30,&a)==ESP_ERR_INVALID_ARG);
 assert(memcmp(&a,&unchanged,sizeof(a))==0);
 fill(&a);assert(webrtc_transport_commit_cpu_access_unit(&a,sizeof(idr),&meta));
 assert(s_ctx.slots[0].cpu_dirty_bytes==sizeof(idr));
 assert(webrtc_transport_reserve_guest_access_unit(1920,1080,30,&b)==ESP_OK);
 assert(webrtc_transport_reserve_guest_access_unit(1920,1080,30,&none)==ESP_ERR_NO_MEM);
 assert(!none.data && s_ctx.slot_waiter==&s_task && notified==0);
 webrtc_transport_cancel_cpu_access_unit(&b,600); /* clamped to the lease */
 assert(s_ctx.slots[1].cpu_dirty_bytes==512 && notified==0);
 assert(take_next_ready_slot(&s_ctx, false)==0);release_sending_slot(&s_ctx,0);
 assert(notified==1 && !s_ctx.slot_waiter);
 /* The DMA producer's next lease of the slots writes the CPU's bytes back. */
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&a) && a.slot==0);
 assert(clean_calls==1 && cleaned_bytes==128 && s_ctx.slots[0].cpu_dirty_bytes==0);
 assert(webrtc_transport_reserve_access_unit(1920,1080,30,&b) && b.slot==1);
 assert(clean_calls==2 && cleaned_bytes==512);
 webrtc_transport_cancel_access_unit(&a);webrtc_transport_cancel_access_unit(&b);
 s_ctx.session_active=false; /* no viewer */
 assert(webrtc_transport_reserve_guest_access_unit(1920,1080,30,&a)==ESP_ERR_INVALID_STATE);
 assert(!lock_depth);
 puts("AU leases: FIFO, generation, video epoch, stop lifetime, stale tokens, IDR, DMA cache prefixes and guest CPU leases: OK");
}
'''
with tempfile.TemporaryDirectory(prefix='webrtc-au-lease-') as directory:
    path=Path(directory)
    (path/'test.c').write_text(preamble+types+context+functions+checks)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)

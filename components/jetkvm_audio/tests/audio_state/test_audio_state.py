#!/usr/bin/env python3
"""Exercise the production preference/queue state machine with fault providers."""
from pathlib import Path
import re, subprocess, tempfile
root = Path(__file__).resolve().parents[4]
source = (root / 'components/jetkvm_audio/jetkvm_audio.c').read_text()
def function(name):
    match = re.search(r'^(?:static )?[^\n]+\b' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    end = match.end(); depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}'); end += 1
    return source[match.start():end] + '\n'
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define JETKVM_AUDIO_PACKET_MAX 1275
#define PACKET_SLOTS __PACKET_SLOTS__
#define pdTRUE 1
#define portMAX_DELAY 0xffffffff
#define NVS_READWRITE 1
#define ESP_OK 0
#define pdMS_TO_TICKS(n) (n)
typedef int nvs_handle_t,esp_err_t;
static void *s_mutex=(void *)1,*s_task=(void *)2;
typedef struct { uint32_t pts; int size; uint8_t data[JETKVM_AUDIO_PACKET_MAX]; } packet_t;
static packet_t packets[PACKET_SLOTS],*s_packets=packets;
static struct { unsigned dropped_frames; } s_stats;
static unsigned s_read,s_count;
static uint32_t s_generation;
static bool s_enabled,s_usb_ready,s_peer_connected,s_streaming,s_usb_paused,held,save_fails,s_saving;
static unsigned saved;
static int xSemaphoreTake(void *p,unsigned ticks) {(void)ticks;assert(p);if(held)return 0;held=true;return 1;}
static void xSemaphoreGive(void *p) {assert(p&&held);held=false;}
static int nvs_open(const char *ns,int mode,int *h) {assert(!held&&!strcmp(ns,"jk_audio")&&mode==1);*h=1;return 0;}
static int nvs_set_u8(int h,const char *key,uint8_t v) {assert(h==1&&!strcmp(key,"enabled"));saved=v;return 0;}
static int nvs_commit(int h) {assert(h==1&&!held&&s_saving);return save_fails;}
static void nvs_close(int h) {assert(h==1);}
'''
prefix = prefix.replace('__PACKET_SLOTS__', re.search(r'JETKVM_AUDIO_PACKET_SLOTS (\d+)', (root/'components/jetkvm_audio/include/jetkvm_audio.h').read_text()).group(1))
functions = ''.join(function(n) for n in ['capture_pts','capture_discontinuous','capture_read_skipped','invalidate','lock','unlock','get_enabled','set_enabled','queue_packet','jetkvm_audio_usb_ready','jetkvm_audio_usb_pause','jetkvm_audio_set_peer_connected','jetkvm_audio_pop','jetkvm_audio_stream_stopped'])
main = r'''
int main(void) {
 uint8_t output[JETKVM_AUDIO_PACKET_MAX], bytes[]={1,2,3};int size;uint32_t pts;bool enabled;
 assert(set_enabled(true,NULL)&&!s_enabled&&!saved); /* No USB class. */
 jetkvm_audio_usb_ready(true);jetkvm_audio_set_peer_connected(true);s_streaming=true;
 assert(!set_enabled(true,NULL)&&s_enabled&&saved);
 assert(get_enabled(&enabled,NULL)&&enabled);
 for(unsigned i=0;i<=PACKET_SLOTS;++i)queue_packet(s_generation,bytes,3,20*(i+1));
 assert(s_count==PACKET_SLOTS&&s_stats.dropped_frames==1);
 assert(jetkvm_audio_pop(output,&size,&pts)&&size==3&&pts==40&&!memcmp(output,bytes,3));
 save_fails=true;assert(set_enabled(false,NULL)&&s_enabled&&s_count==PACKET_SLOTS-1&&!s_saving);save_fails=false;
 s_saving=true;assert(set_enabled(false,NULL)&&s_enabled);s_saving=false;
 uint32_t stale=s_generation;jetkvm_audio_set_peer_connected(false);
 jetkvm_audio_set_peer_connected(true);queue_packet(stale,bytes,3,80);assert(!s_count);
 queue_packet(s_generation,bytes,0,80);queue_packet(s_generation,bytes,1276,80);assert(!s_count);
 queue_packet(s_generation,bytes,3,80);assert(s_count==1);
 jetkvm_audio_usb_pause(true);assert(s_streaming&&!s_count);
 assert(set_enabled(true,NULL));queue_packet(s_generation,bytes,3,80);assert(!s_count);
 jetkvm_audio_usb_pause(false); /* Uninstall failed; host is still in alt 1. */
 queue_packet(s_generation,bytes,3,80);assert(s_count==1&&s_streaming);
 jetkvm_audio_usb_pause(true);jetkvm_audio_stream_stopped();jetkvm_audio_usb_pause(false);
 queue_packet(s_generation,bytes,3,80);assert(!s_count&&!s_streaming); /* Real detach wins. */
 jetkvm_audio_stream_stopped();assert(!jetkvm_audio_pop(output,&size,&pts));
 s_streaming=true;queue_packet(s_generation,bytes,3,100);jetkvm_audio_usb_ready(false);
 assert(!s_count&&!s_streaming);assert(get_enabled(&enabled,NULL)&&!enabled);
 assert(!set_enabled(false,NULL)&&!s_enabled);
 uint32_t next=0;bool started=false,anchored=false;
 assert(capture_pts(&next,&started,&anchored,1000,0)==980);
 assert(capture_pts(&next,&started,&anchored,1025,0)==1000); /* Normal scheduling jitter. */
 assert(capture_pts(&next,&started,&anchored,1100,7680)==1020); /* FIFO backlog is not lost time. */
 assert(capture_pts(&next,&started,&anchored,1700,0)==1040); /* Elapsed drift alone never skips samples. */
 assert(capture_discontinuous(true,false,1700,1100));
 started=false; /* Reader detects the actual long interruption. */
 assert(capture_pts(&next,&started,&anchored,1700,0)==1680);
 assert(!capture_discontinuous(true,false,1040,1000));
 assert(capture_discontinuous(true,false,1041,1000));
 assert(capture_discontinuous(true,true,1010,1000));
 assert(!capture_discontinuous(false,true,1010,1000));
 assert(!capture_discontinuous(true,false,5,UINT32_MAX-14));
 assert(!capture_read_skipped(16000,3456,3840,8192)); /* FIFO index wraps. */
 assert(capture_read_skipped(16000,3480,3840,8192)); /* 24 overwritten bytes. */
 assert(!capture_read_skipped(100,100,0,8192));
 assert(capture_read_skipped(100,124,0,8192));
 next=0;started=false;anchored=false;
 for (unsigned frame=0;frame<30000;++frame) {
   /* Ten minutes at +/-1000 ppm between USB samples and local timer. */
   uint32_t now=1000+frame*20+frame/50;
   assert(capture_pts(&next,&started,&anchored,now,0)==980+frame*20);
 }
 next=0;started=false;anchored=false;
 for (unsigned frame=0;frame<30000;++frame) {
   uint32_t now=1000+frame*20-frame/50;
   assert(capture_pts(&next,&started,&anchored,now,0)==980+frame*20);
 }
 started=false;
 uint32_t expected=next;
 assert(capture_pts(&next,&started,&anchored,600441,0)==expected); /* Negative drift then real gap. */
 next=UINT32_MAX-19;started=true;anchored=true;
 assert(capture_pts(&next,&started,&anchored,0,0)==UINT32_MAX-19);
 assert(capture_pts(&next,&started,&anchored,20,0)==0); /* Millisecond counter wrap. */
 puts("Audio: bounded queue, stale-session rejection, stop/disable flush and persistence failure: PASS");
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'test.c').write_text(prefix+functions+main)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)

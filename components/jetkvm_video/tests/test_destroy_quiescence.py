#!/usr/bin/env python3
"""Run production csi_destroy against resource-ownership and failure mocks."""
from pathlib import Path
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
source = (component / 'jetkvm_video.c').read_text()
start = source.index('static esp_err_t csi_destroy(app_ctx_t *ctx)\n{')
pos = source.index('{', start) + 1
depth = 1
while depth:
    depth += (source[pos] == '{') - (source[pos] == '}')
    pos += 1
production = source[start:pos]
start = source.index('bool jetkvm_video_is_hdmi_connected(void)\n{')
production += '\n' + source[start:source.index('\n}', start) + 2]

preamble = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_NOT_ALLOWED 3
#define ESP_FAIL 4
#define ESP_ERR_NO_MEM 5
#define CSI_FRAME_BUFFER_COUNT 6
#define DMA_CACHE_FASTPATH_INVALID_HANDLE (-1)
#define DIRECT_H264_DRAIN_MS 3000
#define SCREEN_PREVIEW_DRAIN_MS 3000
#define TAG "mock"
#define ESP_LOGE(tag,...) do { (void)(tag); if(0) printf(__VA_ARGS__); } while(0)
#define ESP_LOGW(tag,...) ESP_LOGE(tag,__VA_ARGS__)
typedef struct {
 void *cam, *h264_enc;
 struct { void *isp, *active_candidate; } bridge;
 unsigned char *capture_buffers[CSI_FRAME_BUFFER_COUNT];
 int capture_cache_fastpaths[CSI_FRAME_BUFFER_COUNT];
 void *frame_buffer, *active_preset;
 size_t frame_buffer_size, capture_buffer_capacity;
 unsigned capture_buffer_count, capture_next_buffer, free_buffers,
  inflight_buffers, ready_buffers, locked_buffers, csi_no_free_buffers,
  current_capture_sequence, capture_width, capture_height, frames_consumed,
  consecutive_timeouts;
 uint32_t capture_buffer_sequences[CSI_FRAME_BUFFER_COUNT];
 int64_t capture_buffer_timestamps_us[CSI_FRAME_BUFFER_COUNT];
 int64_t current_capture_timestamp_us, screen_preview_last_enqueue_us;
 uint32_t active_timings[8];
 bool direct_h264_overlap_enabled, frame_buffer_owned_by_driver,
  video_sleep_resume_direct_pipeline, screen_preview_logged, direct_active,
  h264_mode_refused;
} app_ctx_t;
static unsigned char raw_buffers[CSI_FRAME_BUFFER_COUNT][16];
static volatile bool s_video_hdmi_connected;
bool jetkvm_video_is_hdmi_connected(void);
static int cam_token, encoder_token, isp_token, candidate_token;
static struct {
 int stop_result, drain_result, preview_result, disable_result, del_result,
     transport_result;
 unsigned stop, drain, preview, preview_stop, transport, sync_failed,
     disable, del, isp_disable, isp_del, cache_free;
 bool producer_stopped, encoder_drained, preview_drained, camera_consumed;
} calls;
static const char *esp_err_to_name(int err) { (void)err; return "injected"; }
static void direct_h264_pipeline_deactivate(app_ctx_t *ctx)
{ ctx->direct_active=false; }
static int esp_cam_ctlr_stop(void *cam)
{
 assert(cam==&cam_token); ++calls.stop;
 if(calls.stop_result==ESP_OK || calls.stop_result==ESP_ERR_INVALID_STATE)
  calls.producer_stopped=true;
 return calls.stop_result;
}
static int direct_h264_pipeline_drain(app_ctx_t *ctx,unsigned timeout)
{
 assert(!ctx->direct_active && calls.producer_stopped);
 assert(timeout==DIRECT_H264_DRAIN_MS); ++calls.drain;
 calls.encoder_drained=calls.drain_result==ESP_OK;
 return calls.drain_result;
}
static int screen_preview_pipeline_drain(app_ctx_t *ctx,unsigned timeout)
{
 (void)ctx; assert(calls.encoder_drained); assert(timeout==SCREEN_PREVIEW_DRAIN_MS);
 ++calls.preview; calls.preview_drained=calls.preview_result==ESP_OK;
 return calls.preview_result;
}
static int jetkvm_screen_preview_stop(void) { ++calls.preview_stop; return ESP_OK; }
static int webrtc_transport_clear_mode_and_wait(unsigned timeout)
{
 assert(timeout==3000 && calls.encoder_drained && calls.preview_drained);
 ++calls.transport; return calls.transport_result;
}
static void h264_sync_idr_production_complete(app_ctx_t *ctx, bool success)
{
 (void)ctx; assert(!success);
 assert(calls.encoder_drained && calls.preview_drained && calls.transport);
 ++calls.sync_failed;
}
static int esp_cam_ctlr_disable(void *cam)
{
 assert(cam==&cam_token && calls.producer_stopped && calls.sync_failed);
 ++calls.disable; return calls.disable_result;
}
static int esp_cam_ctlr_del(void *cam)
{
 assert(cam==&cam_token && calls.disable); ++calls.del;
 calls.camera_consumed=calls.del_result!=ESP_ERR_TIMEOUT && calls.del_result!=ESP_ERR_NOT_ALLOWED;
 return calls.del_result;
}
static int esp_isp_disable(void *isp)
{ assert(isp==&isp_token && calls.camera_consumed); ++calls.isp_disable; return ESP_OK; }
static int esp_isp_del_processor(void *isp)
{ assert(isp==&isp_token && calls.isp_disable); ++calls.isp_del; return ESP_OK; }
static int dma_cache_fastpath_unregister(int handle)
{
 assert(handle>0 && handle<=CSI_FRAME_BUFFER_COUNT);
 assert(calls.camera_consumed && calls.encoder_drained && calls.preview_drained);
 ++calls.cache_free; return ESP_OK;
}
static app_ctx_t initial(void)
{
 memset(&calls,0,sizeof(calls));
 s_video_hdmi_connected=true;
 app_ctx_t ctx={.cam=&cam_token,.h264_enc=&encoder_token,
  .bridge={.isp=&isp_token,.active_candidate=&candidate_token},
  .frame_buffer=raw_buffers[0],.active_preset=&candidate_token,
  .frame_buffer_size=16,.capture_buffer_capacity=16,
  .capture_buffer_count=CSI_FRAME_BUFFER_COUNT,.capture_next_buffer=3,
  .free_buffers=1,.inflight_buffers=2,.ready_buffers=4,.locked_buffers=8,
  .csi_no_free_buffers=9,.current_capture_sequence=77,.capture_width=1920,
  .capture_height=1080,.frames_consumed=66,.consecutive_timeouts=2,
  .current_capture_timestamp_us=123456,.screen_preview_last_enqueue_us=99,
  .direct_h264_overlap_enabled=true,.frame_buffer_owned_by_driver=true,
  .video_sleep_resume_direct_pipeline=true,.screen_preview_logged=true,
  .direct_active=true};
 for(unsigned i=0;i<CSI_FRAME_BUFFER_COUNT;i++) {
  ctx.capture_buffers[i]=raw_buffers[i]; ctx.capture_cache_fastpaths[i]=(int)i+1;
  ctx.capture_buffer_sequences[i]=77+i; ctx.capture_buffer_timestamps_us[i]=123456+i;
 }
 for(unsigned i=0;i<8;i++) ctx.active_timings[i]=i+1;
 return ctx;
}
static void raw_preserved(const app_ctx_t *ctx)
{
 assert(ctx->cam==&cam_token && ctx->bridge.isp==&isp_token);
 assert(ctx->frame_buffer==raw_buffers[0] && ctx->capture_buffer_count==CSI_FRAME_BUFFER_COUNT);
 assert(ctx->frame_buffer_size==16 && ctx->capture_buffer_capacity==16);
 assert(ctx->capture_width==1920 && ctx->capture_height==1080);
 assert(ctx->locked_buffers==8 && ctx->current_capture_sequence==77);
 for(unsigned i=0;i<CSI_FRAME_BUFFER_COUNT;i++) {
  assert(ctx->capture_buffers[i]==raw_buffers[i]);
  assert(ctx->capture_cache_fastpaths[i]==(int)i+1);
  assert(ctx->capture_buffer_sequences[i]==77+i);
 }
 assert(!calls.cache_free && !calls.isp_del);
 assert(!ctx->direct_active);
 assert(jetkvm_video_is_hdmi_connected());
}
/* The encoder and the capture buffers outlive teardown; a rebuild reuses them. */
static void torn_down(const app_ctx_t *ctx)
{
 assert(!ctx->cam && ctx->h264_enc==&encoder_token && !ctx->bridge.isp);
 assert(!ctx->frame_buffer && !ctx->frame_buffer_size && !ctx->capture_buffer_capacity);
 assert(!ctx->capture_buffer_count && !ctx->capture_width && !ctx->capture_height);
 assert(!ctx->locked_buffers && !ctx->ready_buffers && !ctx->inflight_buffers);
 assert(!ctx->current_capture_sequence && !ctx->active_preset && !ctx->bridge.active_candidate);
 assert(!ctx->direct_h264_overlap_enabled && !ctx->screen_preview_logged);
 for(unsigned i=0;i<CSI_FRAME_BUFFER_COUNT;i++) {
  assert(ctx->capture_buffers[i]==raw_buffers[i]);
  assert(ctx->capture_cache_fastpaths[i]==DMA_CACHE_FASTPATH_INVALID_HANDLE);
  assert(!ctx->capture_buffer_sequences[i] && !ctx->capture_buffer_timestamps_us[i]);
 }
 assert(calls.cache_free==CSI_FRAME_BUFFER_COUNT);
 assert(calls.isp_del==1 && calls.del==1 && calls.sync_failed==1);
 assert(jetkvm_video_is_hdmi_connected());
}
'''
checks = r'''
int main(void)
{
 const int stop_errors[]={ESP_ERR_TIMEOUT,ESP_ERR_NOT_ALLOWED,ESP_FAIL};
 for(unsigned i=0;i<sizeof(stop_errors)/sizeof(stop_errors[0]);i++) {
  app_ctx_t ctx=initial(); calls.stop_result=stop_errors[i];
  assert(csi_destroy(&ctx)==stop_errors[i]); raw_preserved(&ctx);
  assert(ctx.h264_enc==&encoder_token && calls.stop==1);
  assert(!calls.drain && !calls.preview && !calls.transport && !calls.sync_failed && !calls.del);
 }
 app_ctx_t ctx=initial(); calls.drain_result=ESP_ERR_TIMEOUT;
 assert(csi_destroy(&ctx)==ESP_ERR_TIMEOUT); raw_preserved(&ctx);
 assert(ctx.h264_enc==&encoder_token && calls.drain==1);
 assert(!calls.preview && !calls.transport && !calls.sync_failed && !calls.del);

 ctx=initial(); calls.preview_result=ESP_ERR_TIMEOUT;
 assert(csi_destroy(&ctx)==ESP_ERR_TIMEOUT); raw_preserved(&ctx);
 assert(ctx.h264_enc==&encoder_token && calls.preview_stop==1);
 assert(!calls.transport && !calls.sync_failed && !calls.del);

 const int retained_delete_errors[]={ESP_ERR_TIMEOUT,ESP_ERR_NOT_ALLOWED};
 for(unsigned i=0;i<2;i++) {
  ctx=initial(); calls.del_result=retained_delete_errors[i];
  assert(csi_destroy(&ctx)==retained_delete_errors[i]); raw_preserved(&ctx);
  /* H264 has already drained before delete, so its pending sync IDR is
   * failed; the camera/raw frame ownership must remain intact. */
  assert(ctx.h264_enc==&encoder_token && calls.sync_failed==1 && calls.del==1);
  assert(!calls.camera_consumed && !ctx.direct_h264_overlap_enabled);
 }
 ctx=initial(); calls.disable_result=ESP_FAIL; calls.del_result=ESP_ERR_TIMEOUT;
 assert(csi_destroy(&ctx)==ESP_ERR_TIMEOUT); raw_preserved(&ctx);

 /* Once delete has consumed the camera, secondary errors must never leave
  * a dangling ctx->cam or stale capture bookkeeping. */
 ctx=initial(); calls.del_result=ESP_FAIL;
 assert(csi_destroy(&ctx)==ESP_FAIL); torn_down(&ctx);
 ctx=initial(); calls.disable_result=ESP_ERR_NO_MEM; calls.del_result=ESP_FAIL;
 assert(csi_destroy(&ctx)==ESP_ERR_NO_MEM); torn_down(&ctx);
 ctx=initial(); calls.transport_result=ESP_ERR_TIMEOUT;
 assert(csi_destroy(&ctx)==ESP_ERR_TIMEOUT); torn_down(&ctx);
 ctx=initial(); calls.stop_result=ESP_ERR_INVALID_STATE; calls.disable_result=ESP_ERR_INVALID_STATE;
 assert(csi_destroy(&ctx)==ESP_OK); torn_down(&ctx);
 ctx=initial(); assert(csi_destroy(&ctx)==ESP_OK); torn_down(&ctx);
 ctx=initial(); s_video_hdmi_connected=false;
 assert(csi_destroy(&ctx)==ESP_OK && !jetkvm_video_is_hdmi_connected());
 puts("CSI destroy: failed stop/drain preserve owners; unsafe delete retains raw/camera; consumed delete clears capture state and keeps encoder/buffers: PASS");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='video-destroy-quiescence-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(preamble + production + checks)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)

#!/usr/bin/env python3
"""Prepared encoder metadata is published before producer activation."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'jetkvm_video.c').read_text()
a = source.index('static void publish_prepared_h264_mode(')
b = source.index('\nstatic void direct_h264_pipeline_activate(', a)
function = source[a:b]
activation = source[b:source.index('\nstatic ', b + 1)]
assert activation.index('publish_prepared_h264_mode(ctx)') < activation.index('ctx->direct_h264_active = true')
code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define ESP_OK 0
#define ESP_ERR_NOT_SUPPORTED 0x106
#define TAG "video"
#define ESP_LOGW(tag, format, ...) ((void)tag, (void)format, ++warnings)
typedef int esp_err_t;
typedef struct {bool preview_only_active; void *h264_enc; uint16_t h264_width,h264_height,h264_fps; bool h264_mode_refused;} app_ctx_t;
static int calls,warnings,result;
static esp_err_t webrtc_transport_set_mode(uint16_t w,uint16_t h,uint16_t f) {
 assert(w==1280 && h==720 && f==60); ++calls; return result;
}
''' + function + r'''
int main(void) {
 app_ctx_t c={.h264_width=1280,.h264_height=720,.h264_fps=60};
 publish_prepared_h264_mode(&c); assert(calls==0); /* no allocation yet */
 c.h264_enc=&c;c.preview_only_active=true;
 publish_prepared_h264_mode(&c); assert(calls==0); /* preview must not advertise H264 */
 c.preview_only_active=false;
 publish_prepared_h264_mode(&c); assert(calls==1 && warnings==0); /* no first frame needed */
 assert(!c.h264_mode_refused);
 result=1;publish_prepared_h264_mode(&c);assert(calls==2 && warnings==1);
 assert(!c.h264_mode_refused); /* only a mode the transport cannot stream */
 result=ESP_ERR_NOT_SUPPORTED;publish_prepared_h264_mode(&c);assert(calls==3 && c.h264_mode_refused);
 result=ESP_OK;publish_prepared_h264_mode(&c);assert(calls==4 && !c.h264_mode_refused);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.c').write_text(code)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('Prepared H264 mode: publish before frames; preview/unallocated exclusions and failure fallback: PASS')

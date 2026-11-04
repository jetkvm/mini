#!/usr/bin/env python3
"""Exercise takeover notification ordering and session identity checks."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
def function(source, signature):
    a=source.index(signature);b=source.index('{',a)+1;depth=1
    while depth:
        depth+=(source[b]=='{')-(source[b]=='}');b+=1
    return source[a:b]
ws=(root/'webrtc_websocket.c').read_text();sessions=(root/'webrtc_sessions.c').read_text()
stub=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define HTTPD_WS_TYPE_CLOSE 8
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
typedef struct {int type;uint8_t *payload;size_t len;} httpd_ws_frame_t;
typedef struct {int server;} websocket_listener_t;
typedef struct {websocket_listener_t *owner;int fd;} websocket_client_t;
typedef struct {bool started;uint32_t session_id;} transport_ctx_t;
static transport_ctx_t ctx;
static int sequence;
static transport_ctx_t *webrtc_transport_context(void) {return &ctx;}
static int send_text(websocket_client_t *c,const char *text,size_t n) {
 (void)c;assert(sequence++==0);assert(n==strlen(text));assert(strcmp(text,"{\"type\":\"other-session-connected\"}")==0);return 0;
}
static int httpd_ws_send_frame_async(int server,int fd,httpd_ws_frame_t *frame) {
 assert(sequence++==1 && server==7 && fd==9);assert(frame->type==8 && frame->len==2);
 assert(((unsigned)frame->payload[0]<<8|frame->payload[1])==4001);return 0;
}
enum {WS_CLOSE_SUPERSEDED=3};
static void note_close(websocket_client_t *c,int reason) {(void)c;assert(reason==WS_CLOSE_SUPERSEDED);assert(sequence==2);}
static void close_client(websocket_client_t *c) {(void)c;assert(sequence++==2);}
'''
test=r'''
int main(void) {
 websocket_listener_t owner={.server=7};websocket_client_t client={.owner=&owner,.fd=9};
 supersede_client(&client);assert(sequence==3);
 ctx.started=true;ctx.session_id=8;
 assert(!webrtc_transport_session_superseded(8)); /* HDMI rebuild keeps identity */
 assert(!webrtc_transport_session_superseded(0)); /* no offer yet */
 assert(webrtc_transport_session_superseded(7)); /* another listener/cloud took over */
 ctx.started=false;assert(!webrtc_transport_session_superseded(7));
 puts("WebSocket takeover notification and session identity: PASS");
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'test.c').write_text(stub+function(ws,'static void supersede_client(')+function(sessions,'bool webrtc_transport_session_superseded(')+test)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)

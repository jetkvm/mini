#!/usr/bin/env python3
"""A signaling socket whose epoch the idle timeout ended closes with 4003.

Upstream #1788: the page then opens the login page with the reason instead
of reconnecting. Another revocation (a password change or a new login) keeps
the plain close, after which the page probes /device.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]


def function(source, signature):
    a = source.index(signature)
    b = source.index('{', a) + 1
    depth = 1
    while depth:
        depth += (source[b] == '{') - (source[b] == '}')
        b += 1
    return source[a:b]


ws = (root / 'webrtc_websocket.c').read_text()
stub = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define HTTPD_WS_TYPE_CLOSE 8
typedef struct {int type;uint8_t *payload;size_t len;} httpd_ws_frame_t;
typedef struct {int server;} websocket_listener_t;
typedef struct {websocket_listener_t *owner;int fd;uint32_t access_epoch;} websocket_client_t;
enum {WS_CLOSE_EPOCH=1, WS_CLOSE_EXPIRED=6};
static uint32_t expired_epoch;
static int close_code, reason, closed;
static bool jetkvm_access_epoch_expired(uint32_t epoch) {return epoch && epoch == expired_epoch;}
static int httpd_ws_send_frame_async(int server,int fd,httpd_ws_frame_t *frame) {
 assert(server==7 && fd==9 && !closed);assert(frame->type==HTTPD_WS_TYPE_CLOSE && frame->len==2);
 close_code=(int)((unsigned)frame->payload[0]<<8|frame->payload[1]);return 0;
}
static void note_close(websocket_client_t *c,int why) {(void)c;reason=why;}
static void close_client(websocket_client_t *c) {(void)c;++closed;}
'''
test = r'''
int main(void) {
 websocket_listener_t owner={.server=7};
 websocket_client_t client={.owner=&owner,.fd=9,.access_epoch=3};
 expired_epoch=3;
 close_revoked_client(&client);
 assert(close_code==4003 && reason==WS_CLOSE_EXPIRED && closed==1);
 close_code=0;closed=0;expired_epoch=2;
 close_revoked_client(&client);
 assert(close_code==0 && reason==WS_CLOSE_EPOCH && closed==1);
 puts("WebSocket close of an expired login: PASS");
}
'''
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory)
    (p / 'test.c').write_text(stub + function(ws, 'static void close_revoked_client(') + test)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(p / 'test.c'), '-o', str(p / 'test')],
                   check=True)
    subprocess.run([str(p / 'test')], check=True)

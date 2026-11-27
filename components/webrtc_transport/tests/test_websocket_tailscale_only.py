#!/usr/bin/env python3
"""A signaling socket that "Web interface through Tailscale only" does not
admit ends at its next poll, and a frame on it is not read.

The Host check admits a signaling socket once, at its upgrade. A socket from
the local network that is open when the setting turns on must not go on: it
could send a new offer and start a session. Compiles poll_client() and
websocket_handler() of webrtc_websocket.c against stand-ins.
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
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
enum {ESP_OK=0, ESP_FAIL=-1, ESP_ERR_NO_MEM=0x101, ESP_ERR_INVALID_SIZE=0x104};
enum {HTTP_GET=1, HTTP_POST=3, HTTPD_400_BAD_REQUEST=400};
enum {HTTPD_WS_TYPE_TEXT=1, HTTPD_WS_TYPE_CLOSE=8, HTTPD_WS_TYPE_PONG=10};
enum {WEBRTC_SIGNAL_NONE=0};
enum {MSG_PEEK=1, MSG_DONTWAIT=2};
#define WS_MESSAGE_MAX 4096
#define WS_IDLE_TIMEOUT_US 30000000
enum {WS_CLOSE_IDLE, WS_CLOSE_EPOCH, WS_CLOSE_REBUILD, WS_CLOSE_SUPERSEDED, WS_CLOSE_SEND_FAILURE,
      WS_CLOSE_PEER, WS_CLOSE_EXPIRED, WS_CLOSE_NOT_ADMITTED};
typedef struct {int type;bool final;size_t len;uint8_t *payload;} httpd_ws_frame_t;
typedef struct {int type;} webrtc_signal_event_t;
typedef struct websocket_listener websocket_listener_t;
typedef struct {websocket_listener_t *owner;uint32_t access_epoch;int fd;uint32_t session_id;
    uint32_t generation;int64_t last_receive_us;} websocket_client_t;
struct websocket_listener {int server;atomic_bool running, poll_pending;websocket_client_t *client;};
typedef struct {void *user_ctx;int method;void *sess_ctx;} httpd_req_t;

static bool epoch_valid = true, admitted = true;
static int revoked, closed, close_reason = -1, session_polls, kept_alive, frames_read, signals;
static bool jetkvm_access_expire_idle(void) {return false;}
static bool jetkvm_access_epoch_valid(uint32_t epoch) {(void)epoch;return epoch_valid;}
static bool jetkvm_access_admits_socket(int fd) {assert(fd==9);return admitted;}
static void close_revoked_client(websocket_client_t *c) {(void)c;++revoked;}
static void note_close(websocket_client_t *c,int reason) {(void)c;close_reason=reason;}
static void close_client(websocket_client_t *c) {(void)c;++closed;}
static void supersede_client(websocket_client_t *c) {(void)c;assert(0);}
static int64_t esp_timer_get_time(void) {return 1000;}
static long recv(int fd,void *buffer,size_t size,int flags) {(void)fd;(void)buffer;(void)size;(void)flags;return 1;}
static void httpd_sess_update_lru_counter(int server,int fd) {assert(server==7 && fd==9);++kept_alive;}
static esp_err_t webrtc_transport_websocket_session_poll(uint32_t session,webrtc_signal_event_t *event,uint32_t *generation) {
 assert(session==5);event->type=WEBRTC_SIGNAL_NONE;*generation=2;++session_polls;return ESP_OK;
}
static void webrtc_transport_signal_event_release(webrtc_signal_event_t *event) {(void)event;}
static bool webrtc_transport_session_superseded(uint32_t session) {(void)session;return false;}
static esp_err_t send_event(websocket_client_t *c,const webrtc_signal_event_t *event) {(void)c;(void)event;return ESP_OK;}
static void webrtc_transport_signal_record_drop(void) {}
static esp_err_t httpd_resp_send_err(httpd_req_t *req,int status,const char *text) {(void)req;(void)status;(void)text;return ESP_FAIL;}
/* One text frame "ping": the length first, then the payload. */
static esp_err_t httpd_ws_recv_frame(httpd_req_t *req,httpd_ws_frame_t *frame,size_t size) {
 (void)req;++frames_read;
 if(!size){frame->type=HTTPD_WS_TYPE_TEXT;frame->final=true;frame->len=4;return ESP_OK;}
 assert(size==4);memcpy(frame->payload,"ping",4);return ESP_OK;
}
static void *jetkvm_memory_policy_control_alloc(size_t size) {return malloc(size);}
static void jetkvm_memory_policy_control_free(void *p) {free(p);}
static esp_err_t receive_signal(websocket_client_t *c,const uint8_t *wire,size_t size) {
 (void)c;assert(size==4 && !memcmp(wire,"ping",4));++signals;return ESP_OK;
}
'''
test = r'''
int main(void) {
 websocket_listener_t owner={.server=7};
 websocket_client_t client={.owner=&owner,.access_epoch=3,.fd=9,.session_id=5,.generation=2,.last_receive_us=1000};
 owner.client=&client;atomic_store(&owner.running,true);
 httpd_req_t frame={.user_ctx=&owner,.method=HTTP_POST,.sess_ctx=&client};

 /* An admitted socket: the poll serves its session, and a frame is read. */
 poll_client(&owner);
 assert(session_polls==1 && kept_alive==1 && !closed && !revoked);
 assert(websocket_handler(&frame)==ESP_OK && frames_read==2 && signals==1);

 /* The setting turned on and this socket came from the local network: the
  * poll closes it, with its session, and does not serve it. */
 admitted=false;
 poll_client(&owner);
 assert(closed==1 && close_reason==WS_CLOSE_NOT_ADMITTED && session_polls==1 && kept_alive==1 && !revoked);
 /* A frame that arrives before that poll is not read and ends the socket. */
 assert(websocket_handler(&frame)==ESP_FAIL && frames_read==2 && signals==1);

 /* A revoked login keeps its own close, with its own reason. */
 epoch_valid=false;
 poll_client(&owner);
 assert(revoked==1 && closed==1);
 puts("Signaling socket that Tailscale-only access does not admit: closed at the poll, frame not read: PASS");
}
'''
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory)
    (p / 'test.c').write_text(stub + function(ws, 'static void poll_client(') +
                              function(ws, 'static esp_err_t websocket_handler(') + test)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)

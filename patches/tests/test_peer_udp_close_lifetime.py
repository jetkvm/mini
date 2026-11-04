#!/usr/bin/env python3
"""Force close before reference acquisition and while a descriptor is owned."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
REL = Path('third_party/esp_peer/src/transport/udp.c')
source = (ROOT / REL).read_text()
def function(text, signature):
    a = text.index(signature); b = text.index('{', a); depth = 1; end = b + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}'); end += 1
    return text[a:end]
prefix = r'''
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#define WEAK
#define TAG "udp"
#define ESP_LOGE(...) ((void)0)
typedef struct { int fd,ipv6_fd,sin6_scope_id; atomic_int user_count; } udp_socket_t;
typedef struct { int family; uint16_t port; uint8_t ipv4[4],ipv6[16]; } esp_peer_addr_t;
static udp_socket_t socket_state;
static bool inject_before_increment, recycled, violation;
void udp_socket_close(udp_socket_t *s);
static void peer_atomic_inc(atomic_int *v) {
 if (inject_before_increment) {
  inject_before_increment=false; udp_socket_close(&socket_state);
 }
 atomic_fetch_add(v,1);
}
static int peer_atomic_dec(atomic_int *v) { return atomic_fetch_sub(v,1)-1; }
static int peer_atomic_load(atomic_int *v) { return atomic_load(v); }
static int mock_shutdown(int fd,int how) { assert(fd==5 && how==SHUT_RDWR); return 0; }
static int mock_close(int fd) { assert(fd==5); recycled=true; return 0; }
static void media_lib_thread_sleep(int ms) {
 assert(ms==10);
 /* A previously referenced operation reaches its saved descriptor before
  * retiring. The descriptor may be shut down, but must not belong to HTTP. */
 if(recycled)violation=true;
 atomic_store(&socket_state.user_count,0);
}
static ssize_t mock_sendto(int fd,const void *buf,size_t len,int flags,const struct sockaddr *addr,socklen_t size) {
 (void)buf;(void)flags;(void)addr;(void)size;assert(fd==5);
 if(recycled)violation=true;
 return len;
}
static void measure_start(const char *tag) {(void)tag;}
static void measure_stop(const char *tag) {(void)tag;}
#define sendto mock_sendto
#define close mock_close
#define shutdown mock_shutdown
'''
suffix = r'''
int main(void) {
 esp_peer_addr_t addr={.family=AF_INET};uint8_t payload=0;
 socket_state=(udp_socket_t){.fd=5,.ipv6_fd=-1};
 inject_before_increment=true;
 udp_socket_sendto(&socket_state,&addr,&payload,1);
 if(violation)return 3;
 assert(atomic_load(&socket_state.user_count)==0);
 socket_state=(udp_socket_t){.fd=5,.ipv6_fd=-1,.user_count=1};
 recycled=false;
 udp_socket_close(&socket_state);
 if(violation)return 4;
 assert(recycled && socket_state.fd==-1 && socket_state.ipv6_fd==-1);
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='udp-close-lifetime-') as directory:
    work = Path(directory)
    body = '\n'.join(function(source, signature) for signature in (
        'static void udp_add_user(', 'static void udp_dec_user(',
        'void WEAK udp_socket_close(', 'int WEAK udp_socket_sendto(',
    ))
    c = work / 'test.c'
    binary = work / 'test'
    c.write_text(prefix + body + suffix)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    # The caller owns a reference before dispatch snapshots either descriptor.
    receiver=function(source,'int WEAK udp_socket_recvfrom_nowait(')
    assert receiver.index('udp_add_user') < receiver.index('udp_socket_recv_dispatch') < receiver.index('udp_dec_user')
print('UDP lifetime: referenced send/receive retirement preserves HTTP: PASS')

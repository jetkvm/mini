#!/usr/bin/env python3
"""A delayed signaling close must not reset a recycled HTTPD session slot."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'webrtc_websocket.c').read_text()
start = source.index('static void close_client(')
end = source.index('\nstatic void free_client(', start)
function = source[start:end]
prefix = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SHUT_RDWR 2
typedef struct websocket_client websocket_client_t;
typedef struct { int server; websocket_client_t *client; } websocket_listener_t;
struct websocket_client { websocket_listener_t *owner; int fd; uint32_t session_id; };
static bool socket_live, pending_slot_close;
static unsigned closed_session;
static void webrtc_transport_session_close(uint32_t id) { closed_session = id; }
int shutdown(int fd, int how) {
    if (fd != 9 || how != SHUT_RDWR) return -1;
    socket_live = false;
    return 0;
}
/* IDF queues a pointer to its reusable session slot, not a socket generation. */
int httpd_sess_trigger_close(int server, int fd) {
    (void)server; (void)fd; pending_slot_close = true; return 0;
}
'''
suffix = r'''
int main(void) {
    websocket_listener_t owner = {.server=1};
    websocket_client_t old = {.owner=&owner,.fd=9,.session_id=7};
    websocket_client_t replacement = {.owner=&owner,.fd=10,.session_id=8};
    for (int superseded=0; superseded<2; ++superseded) {
        owner.client = superseded ? &replacement : &old;
        old.session_id = 7; socket_live = true; pending_slot_close = false;
        close_client(&old);
        if (closed_session != 7 || old.session_id != 0) return 1;
        if (owner.client != (superseded ? &replacement : 0)) return 2;
        /* HTTPD handles EOF, then accepts /device in the same slot. */
        socket_live = true;
        if (pending_slot_close) socket_live = false; /* delayed work drains */
        if (!socket_live) return 3;
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    legacy = function.replace('(void)shutdown(client->fd, SHUT_RDWR);',
                              '(void)httpd_sess_trigger_close(owner->server, client->fd);')
    for name, body, expected in [('current', function, 0), ('queued-close', legacy, 3)]:
        c = path / (name + '.c'); binary = path / name
        c.write_text(prefix + body + suffix)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)])
        assert result.returncode == expected, (name, result.returncode)
print('WebSocket recycled HTTP session: synchronous close passes; legacy queued close reproduces reset: PASS')

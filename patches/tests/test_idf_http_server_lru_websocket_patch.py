#!/usr/bin/env python3
"""The LRU purge must take a WebSocket only when no plain session can go.

The real enum_function from ESP-IDF's httpd_sess.c is compiled with a small
harness, once pristine and once with the patch applied, and run against the
same session tables in both iteration orders. Pristine picks the WebSocket
whenever its counter is the lowest; patched picks the oldest plain session
and falls back to the oldest WebSocket only when every slot is a WebSocket.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SDK = Path(os.environ.get('IDF_PATH', Path.home() / '.espressif/v6.0.2/esp-idf'))
SESS = 'components/esp_http_server/src/httpd_sess.c'
PATCH = ROOT / 'patches/esp-idf-6.0.2-http-server-lru-websocket.patch'


def function(text, signature):
    a = text.index(signature)
    b = text.index('{', a)
    end = b + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[a:end]


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define CONFIG_HTTPD_WS_SUPPORT 1
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define LOG_FMT(x) x
typedef struct { int dummy; } fd_set;
#define FD_SET(fd, set) ((void)(fd), (void)(set))
struct httpd_data { int unused; };
struct sock_db { int fd; bool for_async_req; bool ws_handshake_done; uint64_t lru_counter; void *ctx; };
typedef enum { HTTPD_TASK_INIT, HTTPD_TASK_GET_ACTIVE, HTTPD_TASK_GET_FREE, HTTPD_TASK_FIND_FD,
               HTTPD_TASK_SET_DESCRIPTOR, HTTPD_TASK_DELETE_INVALID, HTTPD_TASK_FIND_LOWEST_LRU,
               HTTPD_TASK_CLOSE } task_t;
typedef struct { task_t task; int fd; fd_set *fdset; int max_fd; struct httpd_data *hd;
                 uint64_t lru_counter; struct sock_db *session; } enum_context_t;
static const char *TAG = "t";
static bool fd_is_valid(int fd) { (void)fd; return true; }
static void httpd_sess_delete(struct httpd_data *hd, struct sock_db *sd) { (void)hd; (void)sd; }
ENUM_FUNCTION
/* Run FIND_LOWEST_LRU over `n` sessions, forwards or backwards; return the chosen fd. */
static int lowest(struct sock_db *s, int n, bool reverse) {
    enum_context_t ctx = { .task = HTTPD_TASK_FIND_LOWEST_LRU, .lru_counter = UINT64_MAX, .session = NULL };
    for (int k = 0; k < n; k++) {
        int i = reverse ? n - 1 - k : k;
        if (!enum_function(&s[i], &ctx)) break;
    }
    return ctx.session ? ctx.session->fd : -1;
}
int main(void) {
    /* A WebSocket with the lowest counter next to two plain sessions. */
    struct sock_db mixed[3] = {
        { .fd = 10, .ws_handshake_done = true,  .lru_counter = 5 },
        { .fd = 11, .ws_handshake_done = false, .lru_counter = 7 },
        { .fd = 12, .ws_handshake_done = false, .lru_counter = 9 },
    };
    int a = lowest(mixed, 3, false), b = lowest(mixed, 3, true);
    /* Only WebSockets: the oldest one must still be chosen. */
    struct sock_db ws_only[2] = {
        { .fd = 20, .ws_handshake_done = true, .lru_counter = 4 },
        { .fd = 21, .ws_handshake_done = true, .lru_counter = 2 },
    };
    int c = lowest(ws_only, 2, false), d = lowest(ws_only, 2, true);
    /* A busy async slot is never a candidate. */
    struct sock_db busy[2] = {
        { .fd = 30, .for_async_req = true, .lru_counter = 1 },
        { .fd = 31, .lru_counter = 3 },
    };
    int e = lowest(busy, 2, false);
    printf("%d %d %d %d %d\n", a, b, c, d, e);
    assert(c == 21 && d == 21 && e == 31);
    return a == 11 && b == 11 ? 0 : 3;
}
'''

with tempfile.TemporaryDirectory() as directory:
    p = Path(directory)
    target = p / SESS
    target.parent.mkdir(parents=True, exist_ok=True)
    # The pristine SDK file, whether or not configure already patched the tree.
    target.write_bytes(subprocess.check_output(['git', 'show', f'HEAD:{SESS}'], cwd=SDK))
    pristine = target.read_text()
    subprocess.run(['git', 'apply', str(PATCH)], cwd=p, check=True)
    patched = target.read_text()
    assert patched != pristine
    for name, source, expected in (('pristine', pristine, 3), ('patched', patched, 0)):
        c = p / f'{name}.c'
        c.write_text(HARNESS.replace('ENUM_FUNCTION', function(source, 'static int enum_function')))
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(p / name)], check=True)
        result = subprocess.run([str(p / name)], capture_output=True, text=True)
        assert result.returncode == expected, (name, result.returncode, result.stdout)
print('LRU purge: pristine takes the lowest-counter WebSocket, patched takes the oldest plain session in both iteration orders: PASS')

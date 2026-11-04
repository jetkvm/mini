#!/usr/bin/env python3
"""The LRU purge must close only an idle session that has answered a request,
and close it at once.

An asset connection waiting for its first answer keeps a low LRU counter, so
it must not be purged. The real session functions of httpd_sess.c run in a
small harness, with the WebSocket patch only and with this patch on top,
against the same session tables in both iteration orders. httpd_main.c is
checked for the direct close.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SDK = Path(os.environ.get('IDF_PATH', Path.home() / '.espressif/v6.0.2/esp-idf'))
SESS = 'components/esp_http_server/src/httpd_sess.c'
MAIN = 'components/esp_http_server/src/httpd_main.c'
EARLIER = ROOT / 'patches/esp-idf-6.0.2-http-server-lru-websocket.patch'
PATCH = ROOT / 'patches/esp-idf-6.0.2-http-server-lru-unanswered.patch'


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
#define MSG_PEEK 1
#define MSG_DONTWAIT 2
#define HTTP_SERVER_EVENT_DISCONNECTED 7
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
static TickType_t now = 100000;
static inline TickType_t xTaskGetTickCount(void) { return now; }
typedef struct { int dummy; } fd_set;
#define FD_SET(fd, set) ((void)(fd), (void)(set))
struct httpd_data { int http_server_state; };
struct sock_db { int fd; bool for_async_req; bool ws_handshake_done; uint64_t lru_counter; void *ctx;
                 bool lru_socket; void *handle; size_t pending_len; bool answered; TickType_t accept_tick; };
typedef enum { HTTPD_TASK_INIT, HTTPD_TASK_GET_ACTIVE, HTTPD_TASK_GET_FREE, HTTPD_TASK_FIND_FD,
               HTTPD_TASK_SET_DESCRIPTOR, HTTPD_TASK_DELETE_INVALID, HTTPD_TASK_FIND_LOWEST_LRU,
               HTTPD_TASK_CLOSE } task_t;
typedef struct { task_t task; int fd; fd_set *fdset; int max_fd; struct httpd_data *hd;
                 uint64_t lru_counter; struct sock_db *session; } enum_context_t;
static const char *TAG = "t";
/* Bytes that wait in each socket, by fd. */
static int unread[64];
static int deleted = -1;
static inline long recv_stub(int fd, void *buffer, size_t length, int flags) {
    (void)buffer; (void)length;
    assert(flags == (MSG_PEEK | MSG_DONTWAIT));
    return unread[fd];
}
#define recv recv_stub
static inline bool httpd_sess_pending(struct httpd_data *hd, struct sock_db *session) {
    (void)hd;
    return session->pending_len != 0;
}
static bool fd_is_valid(int fd) { (void)fd; return true; }
static void httpd_sess_delete(struct httpd_data *hd, struct sock_db *sd) { (void)hd; deleted = sd->fd; }
/* The session table that httpd_sess_enum() walks. */
static struct sock_db *table;
static int table_n;
typedef int (*httpd_session_enum_function)(struct sock_db *session, void *context);
static inline void httpd_sess_enum(struct httpd_data *hd, httpd_session_enum_function f, void *context) {
    (void)hd;
    for (int i = 0; i < table_n; i++) {
        if (!f(&table[i], context)) break;
    }
}
PATCHED_HELPERS
ENUM_FUNCTION
/* Run FIND_LOWEST_LRU over `n` sessions, forwards or backwards; return the chosen fd. */
static int lowest(struct sock_db *s, int n, bool reverse) {
    static struct httpd_data hd;
    table = s;
    table_n = n;
    enum_context_t ctx = { .task = HTTPD_TASK_FIND_LOWEST_LRU, .lru_counter = UINT64_MAX, .hd = &hd };
    for (int k = 0; k < n; k++) {
        int i = reverse ? n - 1 - k : k;
        if (!enum_function(&s[i], &ctx)) break;
    }
    return ctx.session ? ctx.session->fd : -1;
}
static bool both(struct sock_db *s, int n, int fd) { return lowest(s, n, false) == fd && lowest(s, n, true) == fd; }
int main(void) {
    int wrong = 0;
    /* A page's asset connection with its request unread and the counter of
     * its accept, an idle keep-alive poller, and the signaling WebSocket. */
    struct sock_db page[3] = {
        { .fd = 10, .lru_counter = 3, .accept_tick = now - 5 },
        { .fd = 11, .lru_counter = 8, .answered = true },
        { .fd = 12, .ws_handshake_done = true, .lru_counter = 1, .answered = true },
    };
    unread[10] = 1;
    if (!both(page, 3, 11)) wrong |= 1;
    /* An asset connection that the browser opened and has not used yet. */
    struct sock_db opened[2] = {
        { .fd = 15, .lru_counter = 2, .accept_tick = now - 5 },
        { .fd = 16, .lru_counter = 9, .answered = true },
    };
    if (!both(opened, 2, 16)) wrong |= 2;
    /* A pipelined request already read into the session's buffer. */
    struct sock_db pipelined[2] = {
        { .fd = 20, .lru_counter = 1, .pending_len = 5, .answered = true },
        { .fd = 21, .lru_counter = 2, .answered = true },
    };
    if (!both(pipelined, 2, 21)) wrong |= 4;
    /* Nothing that may go: nothing is closed. */
    struct sock_db busy[2] = {
        { .fd = 30, .lru_counter = 1, .answered = true },
        { .fd = 31, .lru_counter = 2, .accept_tick = now - 9999 },
    };
    unread[30] = 1;
    if (!both(busy, 2, -1)) wrong |= 8;
    /* A connection that has sent nothing for as long as a browser keeps an
     * unused one may go. */
    busy[1].accept_tick = now - 10000;
    if (!both(busy, 2, 31)) wrong |= 32;
    /* Sessions that may go keep the order of the earlier patches: the oldest
     * plain one, the WebSocket last, an async slot never. */
    struct sock_db idle[3] = {
        { .fd = 40, .ws_handshake_done = true, .lru_counter = 1, .answered = true },
        { .fd = 41, .for_async_req = true, .lru_counter = 2, .answered = true },
        { .fd = 42, .lru_counter = 6, .answered = true },
    };
    assert(both(idle, 3, 42));
    idle[2].lru_counter = 0;
    assert(both(idle, 3, 42));
    assert(both(idle, 2, 40));
    /* The signaling WebSocket beside a plain session whose request waits:
     * nothing goes, since the plain one frees its slot once it is answered. */
    struct sock_db signaling[2] = {
        { .fd = 60, .ws_handshake_done = true, .lru_counter = 1, .answered = true },
        { .fd = 61, .lru_counter = 5, .answered = true },
    };
    unread[61] = 1;
    if (!both(signaling, 2, -1)) wrong |= 64;
    /* Only WebSockets: the oldest goes. */
    struct sock_db sockets[2] = {
        { .fd = 62, .ws_handshake_done = true, .lru_counter = 2, .answered = true },
        { .fd = 63, .ws_handshake_done = true, .lru_counter = 1, .answered = true },
    };
    assert(both(sockets, 2, 63));
    printf("wrong %d\n", wrong);
    return wrong;
}
'''

with tempfile.TemporaryDirectory() as directory:
    p = Path(directory)
    for name in (SESS, MAIN):
        target = p / name
        target.parent.mkdir(parents=True, exist_ok=True)
        # The pristine SDK file, whether or not configure already patched the tree.
        target.write_bytes(subprocess.check_output(['git', 'show', f'HEAD:{name}'], cwd=SDK))
    # The header is not compiled here; the harness declares the session's fields.
    subprocess.run(['git', 'apply', str(EARLIER)], cwd=p, check=True)
    earlier = (p / SESS).read_text()
    pristine_accept = function((p / MAIN).read_text(), 'static esp_err_t httpd_accept_conn')
    subprocess.run(['git', 'apply', '--include', SESS, '--include', MAIN, str(PATCH)], cwd=p, check=True)
    patched = (p / SESS).read_text()
    main = (p / MAIN).read_text()
    assert patched != earlier
    # The server loop asks for a session that may go before it accepts, and
    # without one leaves the connection in the backlog and looks again later.
    server = function(main, 'static esp_err_t httpd_server')
    assert 'httpd_sess_lru_available(hd)' in server and 'wait_for_slot ? &slot_poll : NULL' in server
    # A timed-out poll still serves the requests that wait in session buffers.
    timeout = server[server.index('if (active_cnt == 0)'):]
    timeout = timeout[:timeout.index('}')]
    assert 'FD_ZERO(&read_set);' in timeout and 'return' not in timeout
    # The purge closes its session at once: a queued close names a slot, and
    # a second one queued for the same slot closed the session that took it.
    assert 'return httpd_sess_close_lru(hd);' in pristine_accept
    accept = function(main, 'static esp_err_t httpd_accept_conn')
    assert 'return httpd_sess_close_lru_direct(hd);' in accept
    assert 'httpd_sess_close_lru(hd)' not in accept
    assert function(patched, 'bool httpd_sess_lru_available').count('HTTPD_TASK_FIND_LOWEST_LRU') == 1
    assert 'session->answered = true;' in function(patched, 'esp_err_t httpd_sess_process')
    assert 'session->accept_tick = xTaskGetTickCount();' in function(patched, 'esp_err_t httpd_sess_new')
    for name, source, expected in (('earlier', earlier, 1 | 2 | 4 | 8 | 32 | 64), ('patched', patched, 0)):
        helpers = ''
        if name == 'patched':
            # The patch's helpers, from its constant to the end of httpd_sess_purgeable().
            purgeable = function(source, 'static bool httpd_sess_purgeable')
            helpers = source[source.index('#define HTTPD_UNUSED_SESSION_MS 10000'):
                             source.index(purgeable) + len(purgeable)]
        c = p / f'{name}.c'
        c.write_text(HARNESS.replace('PATCHED_HELPERS', helpers)
                     .replace('ENUM_FUNCTION', function(source, 'static int enum_function')))
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(p / name)], check=True)
        result = subprocess.run([str(p / name)], capture_output=True, text=True)
        assert result.returncode == expected, (name, result.returncode, result.stdout, result.stderr)
print('LRU purge: only an idle session that has answered a request goes, an unused one after 10 s, '
      'a WebSocket not while a plain session waits, and the purge closes it at once: PASS')

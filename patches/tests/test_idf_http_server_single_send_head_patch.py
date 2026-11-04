#!/usr/bin/env python3
"""A response head must leave HTTPD in one send, with the same bytes.

The real httpd_resp_send and httpd_resp_send_chunk run in a harness that
records every send, with and without the patch. Both must put the same bytes
on the wire (also with short sends) and fire the headers-sent event after the
same byte; with the patch the whole head is one send.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SDK = Path(os.environ.get('IDF_PATH', Path.home() / '.espressif/v6.0.2/esp-idf'))
TXRX = 'components/esp_http_server/src/httpd_txrx.c'
# The only project patch before this one that edits httpd_txrx.c.
EARLIER = ROOT / 'patches/esp-idf-6.0.2-http-server-large-body.patch'
PATCH = ROOT / 'patches/esp-idf-6.0.2-http-server-single-send-head.patch'


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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_HTTPD_RESP_HDR 0xb006
#define ESP_ERR_HTTPD_RESP_SEND 0xb007
#define ESP_ERR_HTTPD_ALLOC_MEM 0xb008
#define ESP_ERR_HTTPD_INVALID_REQ 0xb009
#define HTTPD_RESP_USE_STRLEN -1
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define LOG_FMT(x) x
__attribute__((unused)) static const char *TAG = "t";
typedef int (*send_fn_t)(void *, int, const char *, size_t, int);
struct sock_db { void *handle; int fd; send_fn_t send_fn; };
struct resp_hdr { const char *field; const char *value; };
struct httpd_req_aux {
    struct sock_db *sd; const char *status; const char *content_type;
    struct resp_hdr *resp_hdrs; unsigned resp_hdrs_count; unsigned req_hdrs_count;
    size_t max_req_hdr_len; bool first_chunk_sent;
};
typedef struct { void *handle; struct httpd_req_aux *aux; } httpd_req_t;
struct httpd_data { int http_server_state; };
typedef struct { int fd; int data_len; } esp_http_server_event_data;
enum { HTTP_SERVER_EVENT_HEADERS_SENT = 1, HTTP_SERVER_EVENT_SENT_DATA = 2 };
static bool httpd_valid_req(httpd_req_t *r) { return r != NULL; }

static char wire[8192];
static size_t wire_len, max_write;
static unsigned sends;
static size_t headers_sent_at = SIZE_MAX;
static int record(void *h, int fd, const char *buf, size_t len, int flags) {
    (void)h; (void)fd; (void)flags;
    size_t n = max_write && len > max_write ? max_write : len;
    assert(wire_len + n <= sizeof(wire));
    memcpy(wire + wire_len, buf, n);
    wire_len += n;
    ++sends;
    return (int)n;
}
static void esp_http_server_dispatch_event(int id, const void *data, size_t size) {
    (void)data; (void)size;
    if (id == HTTP_SERVER_EVENT_HEADERS_SENT) headers_sent_at = wire_len;
}

FUNCTIONS

static void report(const char *name) {
    printf("%s sends=%u headers_at=%zd bytes=", name, sends,
           headers_sent_at == SIZE_MAX ? (ssize_t)-1 : (ssize_t)headers_sent_at);
    for (size_t i = 0; i < wire_len; ++i) printf("%02x", (unsigned char)wire[i]);
    printf("\n");
    wire_len = 0; sends = 0; headers_sent_at = SIZE_MAX;
}

int main(int argc, char **argv) {
    max_write = argc > 1 ? (size_t)atoi(argv[1]) : 0;
    struct sock_db sd = { .fd = 3, .send_fn = record };
    struct httpd_data hd = {0};
    struct resp_hdr asset[] = {
        {"Content-Encoding", "gzip"}, {"Cache-Control", "public, max-age=31536000, immutable"},
        {"ETag", "\"sha256-0beac8d2617a7d3dd0409a0aab81028768a5d9f178f02a857d035c2476f59cfc\""},
        {"Accept-Ranges", "bytes"}, {"X-Content-Type-Options", "nosniff"},
        {"X-Frame-Options", "DENY"}, {"Connection", "close"},
    };
    struct httpd_req_aux ra = { .sd = &sd, .status = "200 OK", .content_type = "text/javascript",
                                .max_req_hdr_len = 1024 };
    httpd_req_t req = { .handle = &hd, .aux = &ra };

    assert(httpd_resp_send(&req, "hello", HTTPD_RESP_USE_STRLEN) == ESP_OK);
    report("plain");

    ra.resp_hdrs = asset; ra.resp_hdrs_count = 7;
    char body[128];
    memset(body, 'x', sizeof(body));
    assert(httpd_resp_send(&req, body, sizeof(body)) == ESP_OK);
    report("asset");

    ra.status = "404 Not Found"; ra.content_type = "text/plain"; ra.resp_hdrs_count = 2;
    assert(httpd_resp_send(&req, NULL, 0) == ESP_OK);
    report("empty");

    ra.status = "200 OK"; ra.content_type = "text/event-stream"; ra.resp_hdrs_count = 3;
    ra.first_chunk_sent = false;
    assert(httpd_resp_send_chunk(&req, "abc", 3) == ESP_OK);
    assert(httpd_resp_send_chunk(&req, "defg", HTTPD_RESP_USE_STRLEN) == ESP_OK);
    assert(httpd_resp_send_chunk(&req, NULL, 0) == ESP_OK);
    report("chunked");
    return 0;
}
'''


def build(work, name, source):
    functions = '\n'.join(function(source, signature) for signature in (
        'static esp_err_t httpd_send_all(',
        *(['static esp_err_t httpd_send_head('] if 'httpd_send_head(' in source else []),
        'esp_err_t httpd_resp_send(httpd_req_t *r',
        'esp_err_t httpd_resp_send_chunk(httpd_req_t *r',
    ))
    c = work / f'{name}.c'
    c.write_text(HARNESS.replace('FUNCTIONS', functions))
    subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
                    '-Wno-format', '-Wno-sign-compare', '-fsanitize=address,undefined',
                    str(c), '-o', str(work / name)], check=True)
    return work / name


def run(binary, max_write):
    out = subprocess.run([str(binary), str(max_write)], check=True, capture_output=True, text=True).stdout
    return {line.split()[0]: dict(field.split('=', 1) for field in line.split()[1:])
            for line in out.splitlines()}


with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    target = work / TXRX
    target.parent.mkdir(parents=True, exist_ok=True)
    # Pristine checkout so the result does not depend on whether configure
    # already patched this SDK in place.
    target.write_bytes(subprocess.check_output(['git', 'show', f'HEAD:{TXRX}'], cwd=SDK))
    subprocess.run(['git', 'apply', '--include', TXRX, str(EARLIER)], cwd=work, check=True)
    before = target.read_text()
    subprocess.run(['git', 'apply', str(PATCH)], cwd=work, check=True)
    after = target.read_text()
    assert before != after
    old, new = build(work, 'before', before), build(work, 'after', after)
    for max_write in (0, 7):
        a, b = run(old, max_write), run(new, max_write)
        for case in a:
            assert a[case]['bytes'] == b[case]['bytes'], (case, max_write)
            assert a[case]['headers_at'] == b[case]['headers_at'], (case, max_write)
    a, b = run(old, 0), run(new, 0)
    # One send for the head, one for the body; a chunk is size, data, CRLF.
    assert (b['plain']['sends'], b['asset']['sends'], b['empty']['sends']) == ('2', '2', '1'), b
    assert b['chunked']['sends'] == str(1 + 3 + 3 + 2), b['chunked']
    assert int(a['asset']['sends']) == 1 + 4 * 7 + 1 + 1, a['asset']
print('HTTP response head: one send instead of one per header fragment, identical bytes and event position: PASS')

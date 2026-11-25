#!/usr/bin/env python3
"""A client that sends its request headers slowly must lose its connection
once the header time limit of webrtc_http.c is over, on HTTP and on HTTPS.

The real header loop of httpd_parse.c, the real recv functions of
httpd_txrx.c and the TLS receive function of https_server.c run against a
simulated socket and clock. The simulated socket keeps SO_RCVTIMEO as lwIP
does, in whole milliseconds with 0 for no timeout. The simulated TLS layer
calls the receive function until a full record is in, as mbedtls does. The
SDK without this patch, with the earlier HTTP server patches only, must fail
the slow-client cases.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SDK = Path(os.environ.get('IDF_PATH', Path.home() / '.espressif/v6.0.2/esp-idf'))
PARSE = 'components/esp_http_server/src/httpd_parse.c'
TXRX = 'components/esp_http_server/src/httpd_txrx.c'
HEADER = 'components/esp_http_server/include/esp_http_server.h'
HTTPS = 'components/esp_https_server/src/https_server.c'
PATCH = ROOT / 'patches/esp-idf-6.0.2-http-server-header-deadline.patch'
CMAKE = (ROOT / 'CMakeLists.txt').read_text()
EARLIER = re.findall(r'apply_idf_patch\((patches/esp-idf-6\.0\.2-http-server-[\w-]+\.patch)\)', CMAKE)
assert EARLIER[-1] == f'patches/{PATCH.name}', EARLIER
EARLIER = EARLIER[:-1]


def function(text, signature):
    a = text.index(signature)
    b = text.index('{', a)
    end = b + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[a:end]


def optional(text, signature):
    return function(text, signature) if signature in text else ''


# The limit that the firmware sets on both listeners.
configure = function((ROOT / 'components/webrtc_transport/webrtc_http.c').read_text(),
                     'static void configure_http')
LIMIT_MS = int(re.search(r'config->req_hdr_timeout_ms = (\d+);', configure)[1])
RECV_WAIT_S = int(re.search(r'config->recv_wait_timeout = (\d+);', configure)[1])
assert 0 < LIMIT_MS

HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 0x107
typedef int esp_err_t;
/* Unevaluated, but the arguments are type checked against the format. */
#define ESP_LOGD(tag, fmt, ...) ((void)(tag), (void)sizeof(printf(fmt, ##__VA_ARGS__)))
#define ESP_LOGW(tag, fmt, ...) ((void)(tag), (void)sizeof(printf(fmt, ##__VA_ARGS__)))
#define LOG_FMT(x) x
#define HTTPD_SOCK_ERR_FAIL -1
#define HTTPD_SOCK_ERR_INVALID -2
#define HTTPD_SOCK_ERR_TIMEOUT -3
#define MBEDTLS_ERR_SSL_TIMEOUT -0x6800
#define MBEDTLS_ERR_NET_RECV_FAILED -0x004C
#define PARSER_BLOCK_SIZE 128
#define CONFIG_ESP_TLS_USING_MBEDTLS 1
typedef enum { HTTPD_400_BAD_REQUEST = 400, HTTPD_408_REQ_TIMEOUT = 408 } httpd_err_code_t;
typedef void *httpd_handle_t;
static const char *TAG = "t";

/* The simulated clock and client. */
static int64_t now_us;
static int64_t esp_timer_get_time(void) { return now_us; }
static int64_t arrivals[64];            /* when each byte comes, in us */
static int arrival_n, arrival_next;
static int64_t client_gone_us;          /* the client closes here */
static int rcvtimeo_ms = RECV_WAIT_S * 1000;   /* lwIP: 0 is no timeout */
static int sent_408;

static int stub_setsockopt(int fd, int level, int name, const void *value, socklen_t length) {
    (void)fd; (void)length;
    assert(level == SOL_SOCKET && name == SO_RCVTIMEO);
    const struct timeval *tv = value;
    rcvtimeo_ms = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
    return 0;
}
#define setsockopt stub_setsockopt
static long stub_recv(int fd, void *buffer, size_t length, int flags) {
    (void)fd; (void)flags; assert(length > 0);
    int64_t next = arrival_next < arrival_n ? arrivals[arrival_next] : client_gone_us;
    if (next < now_us) next = now_us;
    if (rcvtimeo_ms && next > now_us + (int64_t)rcvtimeo_ms * 1000) {
        now_us += (int64_t)rcvtimeo_ms * 1000;
        errno = EAGAIN;
        return -1;
    }
    now_us = next;
    if (arrival_next >= arrival_n) return 0;
    arrival_next++;
    ((char *)buffer)[0] = 'x';
    return 1;
}
#define recv stub_recv

typedef struct { uint16_t recv_wait_timeout; uint32_t req_hdr_timeout_ms; } httpd_config_t;
struct sock_db { int fd; };
struct httpd_req_aux { struct sock_db *sd; };
typedef struct httpd_req { void *handle; } httpd_req_t;
struct httpd_data {
    httpd_config_t config;
    httpd_req_t hd_req;
    struct httpd_req_aux hd_req_aux;
    int64_t hdr_deadline_us;
    int hdr_deadline_fd;
};
typedef struct { int dummy; } http_parser;
enum { PARSING_URL, PARSING_BODY, PARSING_COMPLETE };
typedef struct { int status; size_t raw_datalen; httpd_err_code_t error; } parser_data_t;

/* The headers are complete once this many bytes are in. */
static int request_len, received;
/* The request has a body: as cb_headers_complete() and cb_on_body() do, the
 * parse is complete only when the first byte of the body is in. */
static bool has_body;
static parser_data_t *current;
static bool retry_408;                  /* an error handler that asks to retry */
static int uri_calls;
static void parse_init(httpd_req_t *r, http_parser *parser, parser_data_t *data) {
    (void)r; (void)parser; current = data; received = 0;
}
static int parse_block(http_parser *parser, size_t offset, size_t length) {
    (void)parser;
    received += (int)length;
    (void)offset;
    if (received >= request_len)
        current->status = has_body && received == request_len ? PARSING_BODY : PARSING_COMPLETE;
    return 0;
}
static esp_err_t httpd_req_handle_err(httpd_req_t *r, httpd_err_code_t error) {
    (void)r;
    if (error == HTTPD_408_REQ_TIMEOUT) sent_408++;
    return retry_408 ? ESP_OK : ESP_FAIL;
}
static esp_err_t httpd_uri(struct httpd_data *hd) { (void)hd; uri_calls++; return ESP_OK; }

TXRX_FUNCTIONS

/* https_server.c */
typedef struct { int fd; } mbedtls_net_context;
static int mbedtls_net_recv(void *ctx, unsigned char *buf, size_t len) {
    long n = stub_recv(((mbedtls_net_context *)ctx)->fd, buf, len, 0);
    return n < 0 ? MBEDTLS_ERR_NET_RECV_FAILED : (int)n;
}
typedef struct httpd_ssl_transport_ctx { httpd_handle_t server; mbedtls_net_context net; } httpd_ssl_transport_ctx_t;
BIO_FUNCTION
#ifndef HAVE_BIO
/* The SDK's own: esp-tls gives mbedtls mbedtls_net_recv(). */
static int httpd_ssl_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    return mbedtls_net_recv(&((httpd_ssl_transport_ctx_t *)ctx)->net, buf, len);
}
#endif
static httpd_ssl_transport_ctx_t tls_session;
static int record_len;
/* esp_tls_conn_read(): mbedtls fetches the whole record before it returns. */
static int tls_recv(httpd_handle_t hd, int fd, char *buf, size_t len, int flags) {
    (void)hd; (void)fd; (void)flags; (void)len;
    unsigned char record[64];
    for (int in = 0; in < record_len;) {
        int n = httpd_ssl_bio_recv(&tls_session, record + in, (size_t)(record_len - in));
        if (n <= 0) return n == 0 ? 0 : n;
        in += n;
    }
    buf[0] = 'x';
    return 1;
}

static int (*recv_fn)(httpd_handle_t, int, char *, size_t, int);
/* read_block() of httpd_parse.c, for one transport call. */
static int read_block(httpd_req_t *r, http_parser *parser, size_t offset, size_t length) {
    (void)parser; (void)offset;
    char buf[PARSER_BLOCK_SIZE];
    struct httpd_data *hd = r->handle;
    int n = recv_fn(hd, hd->hd_req_aux.sd->fd, buf, length < sizeof(buf) ? length : sizeof(buf), 0);
    if (n < 0) {
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
            return httpd_req_handle_err(r, HTTPD_408_REQ_TIMEOUT) == ESP_OK ?
                   HTTPD_SOCK_ERR_TIMEOUT : HTTPD_SOCK_ERR_FAIL;
        return HTTPD_SOCK_ERR_FAIL;
    }
    return n == 0 ? HTTPD_SOCK_ERR_FAIL : n;
}

PARSE_FUNCTIONS

#define S(x) ((int64_t)((x) * 1000000.0))
static struct sock_db session = { .fd = 7 };
static struct httpd_data server = {
    .config = { .recv_wait_timeout = RECV_WAIT_S, .req_hdr_timeout_ms = LIMIT_MS },
};
/* Run one request; return when the server let go of it, in us after start. */
static int64_t request(int len, const double *times, int n, double gone) {
    int64_t start = now_us;
    arrival_n = n; arrival_next = 0; request_len = len;
    for (int i = 0; i < n; i++) arrivals[i] = start + S(times[i]);
    client_gone_us = start + S(gone);
    server.hd_req.handle = &server;
    server.hd_req_aux.sd = &session;
    esp_err_t ret = httpd_parse_req(&server);
    if (ret != ESP_OK) uri_calls = -1;
    return now_us - start;
}
static double trickle[40];

int main(void) {
    int wrong = 0;
    const int64_t limit = (int64_t)LIMIT_MS * 1000;
    for (int i = 0; i < 40; i++) trickle[i] = i * (RECV_WAIT_S - 1);
    now_us = S(100);
    tls_session.server = &server;
    tls_session.net.fd = session.fd;

    /* HTTP: one byte before each recv timeout, for minutes. */
    recv_fn = httpd_default_recv;
    int64_t held = request(1000, trickle, 40, 600);
    if (uri_calls != -1 || held > limit + 1000) wrong |= 1;
    printf("http trickle: closed after %.3f s\n", held / 1e6);

    /* A byte comes 0.5 ms before the limit: the last recv must not wait
     * without a timeout (lwIP takes 0 ms as none). */
    uri_calls = 0;
    double late[] = {0, LIMIT_MS / 1000.0 - 0.0005, 1000};
    held = request(1000, late, 3, 2000);
    if (uri_calls != -1 || held > limit + 1000) wrong |= 2;

    /* An error handler that asks to retry on 408 does not keep the loop. */
    uri_calls = 0; retry_408 = true;
    double one[] = {0};
    held = request(1000, one, 1, 600);
    if (uri_calls != -1 || held > limit + 1000) wrong |= 4;
    retry_408 = false;

    /* HTTPS: one TLS record of 40 bytes, one byte at a time. */
    recv_fn = tls_recv; record_len = 40;
    uri_calls = 0;
    held = request(1000, trickle, 40, 600);
    if (uri_calls != -1 || held > limit + 1000) wrong |= 8;
    printf("https trickle: closed after %.3f s\n", held / 1e6);
    record_len = 1;

    /* A browser: headers in three segments within 0.2 s, on HTTP and HTTPS. */
    for (int tls = 0; tls < 2; tls++) {
        recv_fn = tls ? tls_recv : httpd_default_recv;
        uri_calls = 0;
        double quick[] = {0, 0.1, 0.2};
        request(3, quick, 3, 600);
        assert(uri_calls == 1);
        /* After the headers each recv waits recv_wait_timeout again, so the
         * body of an upload and WebSocket frames have no time limit. */
        assert(rcvtimeo_ms == RECV_WAIT_S * 1000);
        now_us += limit * 4;
        arrivals[0] = now_us + S(RECV_WAIT_S - 0.5); arrival_n = 1; arrival_next = 0;
        char c;
        assert(recv_fn(&server, session.fd, &c, 1, 0) == 1);
        /* The next request on a kept-alive connection gets its own time. */
        uri_calls = 0;
        double next[] = {LIMIT_MS / 1000.0 * 0.6, LIMIT_MS / 1000.0 * 0.9};
        request(2, next, 2, 600);
        assert(uri_calls == 1);
        /* An upload whose headers are complete in time: the first byte of
         * its body comes after the limit, but before recv_wait_timeout. */
        uri_calls = 0; has_body = true;
        double upload[] = {0, LIMIT_MS / 1000.0 * 0.8, LIMIT_MS / 1000.0 * 0.8 + RECV_WAIT_S - 0.5};
        assert(upload[2] > LIMIT_MS / 1000.0);
        request(2, upload, 3, 600);
        assert(uri_calls == 1);
        assert(rcvtimeo_ms == RECV_WAIT_S * 1000);
        has_body = false;
    }
    printf("wrong %d\n", wrong);
    return wrong;
}
'''


def build(sources, name, directory, patched):
    parse, txrx, https = sources
    parse_functions = ''.join(optional(parse, s) + '\n' for s in (
        'static void hdr_deadline_start', 'static bool hdr_deadline_over',
        'static void hdr_deadline_stop'))
    parse_functions += function(parse, 'static esp_err_t httpd_parse_req')
    txrx_functions = function(txrx, 'static int httpd_sock_err') + '\n'
    txrx_functions += optional(txrx, 'esp_err_t httpd_sess_arm_recv_timeout') + '\n'
    txrx_functions += function(txrx, 'int httpd_default_recv')
    bio = optional(https, 'static int httpd_ssl_bio_recv')
    if bio:
        bio = '#define HAVE_BIO 1\n' + bio
    assert bool(bio) == patched
    source = (HARNESS.replace('TXRX_FUNCTIONS', txrx_functions)
              .replace('BIO_FUNCTION', bio)
              .replace('PARSE_FUNCTIONS', parse_functions))
    c = directory / f'{name}.c'
    c.write_text(source)
    subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter',
                    f'-DLIMIT_MS={LIMIT_MS}', f'-DRECV_WAIT_S={RECV_WAIT_S}',
                    str(c), '-o', str(directory / name)], check=True)
    return subprocess.run([str(directory / name)], capture_output=True, text=True)


with tempfile.TemporaryDirectory() as temp:
    work = Path(temp)
    for name in (PARSE, TXRX, HEADER, HTTPS, 'components/esp_http_server/src/esp_httpd_priv.h',
                 'components/esp_http_server/src/httpd_sess.c',
                 'components/esp_http_server/src/httpd_main.c'):
        target = work / name
        target.parent.mkdir(parents=True, exist_ok=True)
        # The pristine SDK file, whether or not configure already patched the tree.
        target.write_bytes(subprocess.check_output(['git', 'show', f'HEAD:{name}'], cwd=SDK))
    for earlier in EARLIER:
        subprocess.run(['git', 'apply', str(ROOT / earlier)], cwd=work, check=True)
    read = lambda: tuple((work / name).read_text() for name in (PARSE, TXRX, HTTPS))
    earlier = read()
    subprocess.run(['git', 'apply', str(PATCH)], cwd=work, check=True)
    patched = read()
    header = (work / HEADER).read_text()
    config = header[header.index('typedef struct httpd_config'):header.index('} httpd_config_t;')]
    assert 'uint32_t req_hdr_timeout_ms;' in config
    # Every TLS session reads through the receive function that knows the limit.
    open_fn = function(patched[2], 'static esp_err_t httpd_ssl_open')
    assert re.search(r'mbedtls_ssl_set_bio\(esp_tls_get_ssl_context\(tls\), transport_ctx,\s*'
                     r'httpd_ssl_bio_send, httpd_ssl_bio_recv, NULL\)', open_fn)
    with tempfile.TemporaryDirectory() as built:
        result = build(earlier, 'earlier', Path(built), False)
        # Without the patch the trickles hold the server for minutes.
        assert result.returncode == 1 | 2 | 4 | 8, (result.returncode, result.stdout, result.stderr)
        result = build(patched, 'patched', Path(built), True)
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
print(f'HTTP and HTTPS headers: a slow client goes after {LIMIT_MS} ms, a browser, '
      'its upload body and its next request do not: PASS')

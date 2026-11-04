#!/usr/bin/env python3
"""Run the real HTTPS range reader against a scripted HTTP client.

Checks which TLS reservation each image read takes: a connection's when the
request may connect and handshake, the smaller one of a request only when
the last response left the keep-alive connection open less than 4 s ago, and
a connection's again before a retry opens a new connection.
"""

from pathlib import Path
import subprocess
import tempfile

COMPONENT = Path(__file__).resolve().parents[1]

HEADERS = {
    "esp_err.h": r'''
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_INVALID_RESPONSE 0x108
''',
    "esp_crt_bundle.h": "#pragma once\n#include \"esp_err.h\"\nesp_err_t esp_crt_bundle_attach(void *conf);\n",
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
    "jetkvm_virtual_media.h": "#pragma once\n#define JETKVM_VIRTUAL_MEDIA_URL_CAPACITY 2048U\n",
    "jetkvm_memory_policy.h": r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t generation; uint8_t slot; bool active; } jetkvm_memory_policy_reservation_t;
bool jetkvm_memory_policy_wait_reserve_tls(const char *owner, jetkvm_memory_policy_reservation_t *r);
bool jetkvm_memory_policy_wait_reserve_tls_open_connection(const char *owner, jetkvm_memory_policy_reservation_t *r);
void jetkvm_memory_policy_release(jetkvm_memory_policy_reservation_t *r);
void *jetkvm_memory_policy_control_alloc(size_t size);
void jetkvm_memory_policy_control_free(void *memory);
''',
    "esp_http_client.h": r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct fake_client *esp_http_client_handle_t;
typedef enum { HTTP_EVENT_ERROR, HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_DATA } esp_http_client_event_id_t;
typedef enum { HTTP_METHOD_GET } esp_http_client_method_t;
typedef enum { HTTP_STATE_UNINIT, HTTP_STATE_INIT, HTTP_STATE_CONNECTING, HTTP_STATE_CONNECTED } esp_http_state_t;
typedef struct {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    void *data;
    int data_len;
    void *user_data;
    char *header_key;
    char *header_value;
} esp_http_client_event_t;
typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *event);
typedef struct {
    const char *url;
    esp_http_client_method_t method;
    int timeout_ms;
    http_event_handle_cb event_handler;
    void *user_data;
    esp_err_t (*crt_bundle_attach)(void *conf);
    bool keep_alive_enable;
    bool disable_auto_redirect;
    int max_redirection_count;
    int buffer_size;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_http_state_t esp_http_client_get_state(esp_http_client_handle_t client);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t client);
''',
}

HARNESS = r'''
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "http_range_client.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "jetkvm_memory_policy.h"

struct fake_client {
    esp_http_client_config_t config;
    esp_http_state_t state;
    int status;
    char range[64];
};
static int64_t now_us = 1000000;
static int clients_created;
/* Next perform: 0 = 206 with the requested bytes, 1 = transport error. */
static int fail_next_perform;
/* How many of the next responses are 302 redirects to another origin. */
static int redirect_next;
/* Whether the server keeps the connection open after a response. */
static bool server_keeps_alive = true;
static bool refuse_connection, refuse_request;
/* How long the next wait for a request's reservation takes. */
static int64_t request_wait_us;
static char log_[512];
static int active_tickets;
/* Every new connection (a TLS handshake) must happen under a connection's
 * reservation. */
static bool holding_connection;
static int connects, redirects;

int64_t esp_timer_get_time(void) { return now_us; }
esp_err_t esp_crt_bundle_attach(void *conf) { (void)conf; return ESP_OK; }
void *jetkvm_memory_policy_control_alloc(size_t size) { return calloc(1, size); }
void jetkvm_memory_policy_control_free(void *memory) { free(memory); }

static bool ticket(bool refuse, const char *kind, const char *owner,
                   jetkvm_memory_policy_reservation_t *r) {
    assert(!r->active);
    strcat(log_, kind); strcat(log_, ":"); strcat(log_, owner); strcat(log_, " ");
    if (refuse) return false;
    r->active = true; active_tickets++;
    holding_connection = strcmp(kind, "connection") == 0;
    return true;
}
bool jetkvm_memory_policy_wait_reserve_tls(const char *owner, jetkvm_memory_policy_reservation_t *r)
{ return ticket(refuse_connection, "connection", owner, r); }
bool jetkvm_memory_policy_wait_reserve_tls_open_connection(const char *owner, jetkvm_memory_policy_reservation_t *r)
{ now_us += request_wait_us; request_wait_us = 0; return ticket(refuse_request, "request", owner, r); }
void jetkvm_memory_policy_release(jetkvm_memory_policy_reservation_t *r)
{ if (r->active) { r->active = false; active_tickets--; holding_connection = false; } }

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    struct fake_client *c = calloc(1, sizeof(*c));
    c->config = *config; c->state = HTTP_STATE_INIT; clients_created++;
    return c;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value)
{ assert(strcmp(key, "Range") == 0); snprintf(c->range, sizeof(c->range), "%s", value); return ESP_OK; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->status; }
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t c) {
    /* Another origin: the next perform connects and handshakes. */
    redirects++;
    c->state = HTTP_STATE_INIT;
    return ESP_OK;
}
esp_http_state_t esp_http_client_get_state(esp_http_client_handle_t c) { return c->state; }
esp_err_t esp_http_client_close(esp_http_client_handle_t c) { c->state = HTTP_STATE_INIT; return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { free(c); return ESP_OK; }
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) {
    if (c->state != HTTP_STATE_CONNECTED) {
        connects++;
        assert(holding_connection && "a new connection needs a connection's reservation");
    }
    if (fail_next_perform) { fail_next_perform--; c->status = 0; c->state = HTTP_STATE_UNINIT; return ESP_FAIL; }
    /* Like esp_http_client: without disable_auto_redirect it follows the
     * redirect itself, connecting to the new origin inside this call. */
    while (redirect_next && !c->config.disable_auto_redirect) {
        redirect_next--;
        redirects++;
        connects++;
        assert(holding_connection && "a new connection needs a connection's reservation");
    }
    if (redirect_next) {
        redirect_next--;
        c->status = 302;
        c->state = HTTP_STATE_CONNECTED;
        static char body[8192];
        esp_http_client_event_t event = { .event_id = HTTP_EVENT_ON_DATA, .client = c,
            .user_data = c->config.user_data, .data = body, .data_len = sizeof(body) };
        c->config.event_handler(&event); /* larger than the read buffer */
        return ESP_OK;
    }
    c->status = 206;
    uint64_t first = 0, last = 0;
    assert(sscanf(c->range, "bytes=%" SCNu64 "-%" SCNu64, &first, &last) == 2);
    char header[96];
    snprintf(header, sizeof(header), "bytes %" PRIu64 "-%" PRIu64 "/4096", first, last);
    esp_http_client_event_t event = { .event_id = HTTP_EVENT_ON_HEADER, .client = c,
        .user_data = c->config.user_data, .header_key = "Content-Range", .header_value = header };
    c->config.event_handler(&event);
    static char body[4096];
    event = (esp_http_client_event_t){ .event_id = HTTP_EVENT_ON_DATA, .client = c,
        .user_data = c->config.user_data, .data = body, .data_len = (int)(last - first + 1) };
    c->config.event_handler(&event);
    c->state = server_keeps_alive ? HTTP_STATE_CONNECTED : HTTP_STATE_INIT;
    return ESP_OK;
}

static esp_err_t read_one(jetkvm_http_range_reader_t *reader) {
    static char buffer[512];
    log_[0] = '\0';
    esp_err_t result = jetkvm_http_range_reader_read(reader, 4096, 0, buffer, sizeof(buffer));
    assert(active_tickets == 0);
    return result;
}

int main(void) {
    jetkvm_http_range_reader_t *reader = NULL;
    assert(jetkvm_http_range_reader_open("https://example.com/a.iso", &reader) == ESP_OK);

    /* No response yet: the first read connects. */
    assert(read_one(reader) == ESP_OK);
    assert(strcmp(log_, "connection:https-media-connect ") == 0);
    /* The next read, 50 ms later on the open connection, takes a request's
     * reservation. */
    now_us += 50000;
    assert(read_one(reader) == ESP_OK);
    assert(strcmp(log_, "request:https-media-range ") == 0);
    /* 4 s idle: the server may have closed it, so the read may connect. */
    now_us += 4000000;
    assert(read_one(reader) == ESP_OK);
    assert(strcmp(log_, "connection:https-media-connect ") == 0);
    /* The server closed the connection after its response. */
    server_keeps_alive = false;
    now_us += 50000;
    assert(read_one(reader) == ESP_OK);
    now_us += 50000;
    assert(read_one(reader) == ESP_OK);
    assert(strcmp(log_, "connection:https-media-connect ") == 0);
    server_keeps_alive = true;
    assert(read_one(reader) == ESP_OK);
    /* A failed request on the open connection: the retry opens a new one
     * and takes a connection's reservation before it does. */
    now_us += 50000;
    fail_next_perform = 1;
    int before = clients_created;
    assert(read_one(reader) == ESP_OK);
    assert(strcmp(log_, "request:https-media-range connection:https-media-connect ") == 0);
    assert(clients_created == before + 1);
    /* If a connection's reservation is refused for that retry, the read fails. */
    now_us += 50000;
    fail_next_perform = 1;
    refuse_connection = true;
    assert(read_one(reader) == ESP_ERR_NO_MEM);
    assert(strcmp(log_, "request:https-media-range connection:https-media-connect ") == 0);
    /* A failed read leaves no open connection behind. */
    refuse_connection = false;
    now_us += 50000;
    assert(read_one(reader) == ESP_OK);
    assert(strcmp(log_, "connection:https-media-connect ") == 0);
    /* A refused request's reservation fails the read without a request, and
     * the connection stays open: the next read asks for a request's
     * reservation again. */
    now_us += 50000;
    refuse_request = true;
    assert(read_one(reader) == ESP_ERR_NO_MEM);
    assert(strcmp(log_, "request:https-media-range ") == 0);
    refuse_request = false;
    now_us += 50000;
    assert(read_one(reader) == ESP_OK);
    assert(strcmp(log_, "request:https-media-range ") == 0);
    /* The wait for a request's reservation outlasts the connection's 4 s: the
     * read trades it for a connection's before it sends the request. */
    now_us += 3000000;
    request_wait_us = 2000000;
    assert(read_one(reader) == ESP_OK);
    assert(strcmp(log_, "request:https-media-range connection:https-media-connect ") == 0);
    /* A wait that the connection outlives keeps the request's reservation. */
    now_us += 50000;
    request_wait_us = 2000000;
    assert(read_one(reader) == ESP_OK);
    assert(strcmp(log_, "request:https-media-range ") == 0);
    now_us += 50000;
    /* A redirect to another origin on the open connection: the reader takes
     * a connection's reservation before following it (the fake client
     * asserts that every new connection happens under one). A redirect
     * body larger than the read buffer is not an overflow. */
    now_us += 50000;
    before = clients_created;
    int redirects_before = redirects, connects_before = connects;
    redirect_next = 1;
    assert(read_one(reader) == ESP_OK);
    assert(strcmp(log_, "request:https-media-range connection:https-media-connect ") == 0);
    assert(redirects == redirects_before + 1 && connects == connects_before + 1);
    assert(clients_created == before);
    /* The redirected connection stays open for the next read. */
    now_us += 50000;
    assert(read_one(reader) == ESP_OK);
    assert(strcmp(log_, "request:https-media-range ") == 0);
    /* If a connection's reservation is refused, the redirect is not
     * followed and the read fails without a retry. */
    now_us += 50000;
    redirect_next = 1;
    refuse_connection = true;
    connects_before = connects;
    assert(read_one(reader) == ESP_ERR_NO_MEM);
    assert(strcmp(log_, "request:https-media-range connection:https-media-connect ") == 0);
    assert(connects == connects_before && clients_created == before);
    refuse_connection = false;
    /* A redirect loop stops after 5 redirects; the read fails, then retries
     * once from a new client, which stops the same way. */
    now_us += 50000;
    redirects_before = redirects;
    redirect_next = 1000;
    assert(read_one(reader) == ESP_ERR_NOT_FOUND);
    assert(redirects == redirects_before + 10);
    redirect_next = 0;
    jetkvm_http_range_reader_close(reader);

    /* The URL check holds a connection's reservation and follows redirects
     * with it. */
    jetkvm_http_range_check_t check;
    log_[0] = '\0';
    redirect_next = 2;
    assert(jetkvm_http_range_check("https://example.com/a.iso", &check) == ESP_OK);
    assert(check.usable && check.size == 4096);
    assert(strcmp(log_, "connection:https-media-check ") == 0 && active_tickets == 0);
    puts("HTTPS range reader TLS reservation per request: PASS");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="range-tls-") as directory:
    path = Path(directory)
    for name, text in HEADERS.items():
        (path / name).write_text(text)
    (path / "test.c").write_text(HARNESS)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-D_GNU_SOURCE",
                    "-fsanitize=address,undefined", "-I" + str(path),
                    "-I" + str(COMPONENT), str(path / "test.c"),
                    str(COMPONENT / "http_range_client.c"),
                    str(COMPONENT / "http_range_contract.c"),
                    "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True,
                   env={"ASAN_OPTIONS": "detect_leaks=0"})

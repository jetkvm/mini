/* SPDX-License-Identifier: MIT */

#include "http_range_client.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "http_range_contract.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_virtual_media.h"

#define RANGE_TIMEOUT_MS 5000
#define CHECK_TIMEOUT_MS 10000
#define RANGE_RETRIES 2U
#define REDIRECT_LIMIT 5U
#define CONTENT_RANGE_CAPACITY 96U
/* Servers close idle keep-alive connections, after 5 s at the earliest by
 * default (Apache). A request sooner than this after the last response is
 * taken to reuse the open connection. */
#define OPEN_CONNECTION_IDLE_US (4 * 1000 * 1000)

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t received;
    bool overflow;
    char content_range[CONTENT_RANGE_CAPACITY];
} request_state_t;

struct jetkvm_http_range_reader {
    char *url;
    esp_http_client_handle_t client;
    request_state_t request;
    int64_t last_response_us;
};

static bool url_uses_tls(const char *url)
{
    return url != NULL && strncmp(url, "https://", 8U) == 0;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    request_state_t *state = event != NULL ? event->user_data : NULL;
    if (state == NULL) {
        return ESP_OK;
    }
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key != NULL &&
        event->header_value != NULL &&
        strcasecmp(event->header_key, "Content-Range") == 0) {
        snprintf(state->content_range, sizeof(state->content_range), "%s",
                 event->header_value);
    } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        int status = esp_http_client_get_status_code(event->client);
        if (status >= 300 && status < 400) {
            return ESP_OK; /* a redirect's body; perform_range follows it */
        }
        if (status >= 200 && status < 300 && status != 206) {
            state->overflow = true;
            (void)esp_http_client_close(event->client);
            return ESP_FAIL;
        }
        size_t incoming = (size_t)event->data_len;
        size_t available = state->received < state->capacity ?
            state->capacity - state->received : 0U;
        size_t copy = incoming < available ? incoming : available;
        if (copy > 0U && state->buffer != NULL) {
            memcpy(state->buffer + state->received, event->data, copy);
        }
        state->received += copy;
        state->overflow |= copy != incoming;
        if (state->overflow) {
            /* IDF 6.0.2's HTTP parser ignores the event callback's return
             * value, so returning ESP_FAIL alone does not stop a server that
             * ignored Range from streaming the entire image. Closing the
             * transport bounds the rejected response to the current RX
             * chunk. */
            (void)esp_http_client_close(event->client);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_http_client_handle_t create_client(const char *url, int timeout_ms,
                                               request_state_t *request)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = timeout_ms,
        .event_handler = http_event,
        .user_data = request,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        /* perform_range follows redirects, so a TLS reservation can be taken
         * for the new connection a redirect may open. */
        .disable_auto_redirect = true,
        .max_redirection_count = REDIRECT_LIMIT,
        .buffer_size = 4096,
    };
    return esp_http_client_init(&config);
}

static bool is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

/* before_redirect runs before each redirect is followed, since that may open
 * a new connection; it returns false to stop with ESP_ERR_NO_MEM. */
static esp_err_t perform_range(esp_http_client_handle_t client,
                               request_state_t *state, uint64_t first,
                               uint64_t last, uint64_t expected_total,
                               bool (*before_redirect)(void *), void *ctx)
{
    char range[64];
    snprintf(range, sizeof(range), "bytes=%" PRIu64 "-%" PRIu64, first, last);
    esp_err_t result = esp_http_client_set_header(client, "Range", range);
    int status = 0;
    for (unsigned redirects = 0U; result == ESP_OK; ++redirects) {
        memset(state->content_range, 0, sizeof(state->content_range));
        state->received = 0U;
        state->overflow = false;
        result = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        if (result != ESP_OK || !is_redirect(status)) {
            break;
        }
        if (redirects == REDIRECT_LIMIT) {
            return ESP_ERR_NOT_FOUND;
        }
        if (before_redirect != NULL && !before_redirect(ctx)) {
            return ESP_ERR_NO_MEM;
        }
        result = esp_http_client_set_redirection(client);
    }
    if (status != 206) {
        return status >= 200 && status < 300 ? ESP_ERR_NOT_SUPPORTED :
               result != ESP_OK ? result : ESP_ERR_NOT_FOUND;
    }
    if (state->overflow) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (result != ESP_OK) {
        return result;
    }
    uint64_t response_first = 0U;
    uint64_t response_last = 0U;
    uint64_t response_total = 0U;
    if (!jetkvm_http_parse_content_range(state->content_range,
                                          &response_first, &response_last,
                                          &response_total) ||
        response_first != first || response_last != last ||
        (expected_total != 0U && response_total != expected_total) ||
        state->received != (size_t)(last - first + 1U)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static void unusable(jetkvm_http_range_check_t *check, const char *reason)
{
    check->usable = false;
    check->size = 0U;
    snprintf(check->reason, sizeof(check->reason), "%s", reason);
}

static const char *status_text(int status)
{
    switch (status) {
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 416: return "Range Not Satisfiable";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "";
    }
}

esp_err_t jetkvm_http_range_check(const char *url,
                                  jetkvm_http_range_check_t *check)
{
    if (url == NULL || check == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(check, 0, sizeof(*check));
    if (!jetkvm_http_url_is_supported(url)) {
        bool other_scheme = strstr(url, "://") != NULL &&
            strncmp(url, "http://", 7U) != 0 &&
            strncmp(url, "https://", 8U) != 0;
        unusable(check, other_scheme ?
            "Only HTTP and HTTPS image URLs can be mounted." :
            "Enter a valid HTTP or HTTPS image URL.");
        return ESP_OK;
    }
    jetkvm_memory_policy_reservation_t tls_memory = {0};
    /* A mount waits for other owners' reservations (an update check, the OS
     * Services index refresh) instead of failing over them. */
    if (url_uses_tls(url) && !jetkvm_memory_policy_wait_reserve_tls(
            "https-media-check", &tls_memory)) {
        return ESP_ERR_NO_MEM;
    }
    uint8_t byte = 0U;
    request_state_t state = {.buffer = &byte, .capacity = 1U};
    esp_http_client_handle_t client = create_client(url, CHECK_TIMEOUT_MS, &state);
    if (client == NULL) {
        jetkvm_memory_policy_release(&tls_memory);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = perform_range(client, &state, 0U, 0U, 0U, NULL, NULL);
    int status = esp_http_client_get_status_code(client);
    uint64_t first = 0U;
    uint64_t last = 0U;
    uint64_t total = 0U;
    bool parsed = jetkvm_http_parse_content_range(state.content_range,
                                                   &first, &last, &total);
    esp_http_client_cleanup(client);
    jetkvm_memory_policy_release(&tls_memory);
    if (result == ESP_OK && parsed && total > 0U) {
        check->usable = true;
        check->size = total;
        return ESP_OK;
    }
    size_t content_range_length = strlen(state.content_range);
    if (content_range_length >= 2U &&
        strcmp(state.content_range + content_range_length - 2U, "/*") == 0) {
        unusable(check,
                 "The URL is available, but the server did not report the image size.");
    } else if (result == ESP_ERR_NOT_SUPPORTED &&
               status >= 200 && status < 300) {
        unusable(check,
                 "The URL is available, but the server does not support byte-range requests.");
    } else if (status >= 300) {
        char reason[sizeof(check->reason)];
        const char *text = status_text(status);
        if (text[0] != '\0') {
            snprintf(reason, sizeof(reason),
                     "The URL is not available (HTTP %d %s). Check the image URL and try again.",
                     status, text);
        } else {
            snprintf(reason, sizeof(reason),
                     "The URL is not available (HTTP %d). Check the image URL and try again.",
                     status);
        }
        unusable(check, reason);
    } else if (result == ESP_ERR_INVALID_RESPONSE) {
        unusable(check,
                 "The URL is available, but the server returned an unreadable range response.");
    } else {
        unusable(check,
                 "The URL is not available. Check the image URL and try again.");
    }
    return ESP_OK;
}

esp_err_t jetkvm_http_range_reader_open(const char *url,
                                        jetkvm_http_range_reader_t **reader)
{
    if (url == NULL || reader == NULL || !jetkvm_http_url_is_supported(url)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t url_size = strnlen(url, JETKVM_VIRTUAL_MEDIA_URL_CAPACITY);
    if (url_size == JETKVM_VIRTUAL_MEDIA_URL_CAPACITY) {
        return ESP_ERR_INVALID_SIZE;
    }
    jetkvm_http_range_reader_t *created =
        jetkvm_memory_policy_control_alloc(sizeof(*created));
    if (created == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(created, 0, sizeof(*created));
    created->url = jetkvm_memory_policy_control_alloc(url_size + 1U);
    if (created->url != NULL) {
        memcpy(created->url, url, url_size + 1U);
    }
    created->client = create_client(url, RANGE_TIMEOUT_MS, &created->request);
    if (created->url == NULL || created->client == NULL) {
        jetkvm_http_range_reader_close(created);
        return ESP_ERR_NO_MEM;
    }
    *reader = created;
    return ESP_OK;
}

void jetkvm_http_range_reader_close(jetkvm_http_range_reader_t *reader)
{
    if (reader == NULL) {
        return;
    }
    if (reader->client != NULL) {
        esp_http_client_cleanup(reader->client);
    }
    jetkvm_memory_policy_control_free(reader->url);
    jetkvm_memory_policy_control_free(reader);
}

typedef struct {
    jetkvm_memory_policy_reservation_t memory;
    bool open_connection;
} read_ticket_t;

/* Before a request can open a new connection (a retry, or following a
 * redirect), trade the request's reservation for a connection's, which
 * covers the handshake. */
static bool ticket_for_new_connection(void *ctx)
{
    read_ticket_t *ticket = ctx;
    if (!ticket->open_connection) {
        return true;
    }
    jetkvm_memory_policy_release(&ticket->memory);
    ticket->open_connection = false;
    return jetkvm_memory_policy_wait_reserve_tls("https-media-connect",
                                                 &ticket->memory);
}

/* Whether the last response left a connection that the server has most
 * likely not closed yet. */
static bool connection_still_open(const jetkvm_http_range_reader_t *reader)
{
    return reader->last_response_us != 0 &&
           esp_http_client_get_state(reader->client) == HTTP_STATE_CONNECTED &&
           esp_timer_get_time() - reader->last_response_us <
               OPEN_CONNECTION_IDLE_US;
}

esp_err_t jetkvm_http_range_reader_read(jetkvm_http_range_reader_t *reader,
                                        uint64_t image_size, uint64_t offset,
                                        void *buffer, size_t size)
{
    if (reader == NULL || buffer == NULL || size == 0U ||
        offset >= image_size || size > image_size - offset) {
        return ESP_ERR_INVALID_ARG;
    }
    reader->request.buffer = buffer;
    reader->request.capacity = size;
    /* What the keep-alive client holds is already out of the free memory,
     * so a reservation lasts for one request only. A request that may
     * connect and handshake takes a connection's reservation; one on the
     * connection that the last response left open takes the smaller one of
     * a request. Both wait for other owners' reservations to end instead of
     * failing the host's read. */
    const bool tls = url_uses_tls(reader->url);
    read_ticket_t ticket = {
        .open_connection = tls && connection_still_open(reader),
    };
    if (tls && !(ticket.open_connection ?
            jetkvm_memory_policy_wait_reserve_tls_open_connection(
                "https-media-range", &ticket.memory) :
            jetkvm_memory_policy_wait_reserve_tls("https-media-connect",
                                                  &ticket.memory))) {
        /* No request was sent; a refused read keeps the connection's state. */
        return ESP_ERR_NO_MEM;
    }
    /* The wait can outlast the connection's idle time; a request that may
     * now have to connect needs a connection's reservation. */
    if (ticket.open_connection && !connection_still_open(reader) &&
        !ticket_for_new_connection(&ticket)) {
        return ESP_ERR_NO_MEM;
    }
    reader->last_response_us = 0;
    esp_err_t result = ESP_FAIL;
    for (unsigned attempt = 0U; attempt < RANGE_RETRIES; ++attempt) {
        result = perform_range(reader->client, &reader->request, offset,
                               offset + size - 1U, image_size,
                               ticket_for_new_connection, &ticket);
        if (result == ESP_OK || result == ESP_ERR_INVALID_RESPONSE ||
            result == ESP_ERR_NOT_SUPPORTED || result == ESP_ERR_NO_MEM) {
            break;
        }
        esp_http_client_cleanup(reader->client);
        reader->client = create_client(reader->url, RANGE_TIMEOUT_MS,
                                       &reader->request);
        if (reader->client == NULL) {
            jetkvm_memory_policy_release(&ticket.memory);
            return ESP_ERR_NO_MEM;
        }
        /* The retry opens a new connection. */
        if (!ticket_for_new_connection(&ticket)) {
            return ESP_ERR_NO_MEM;
        }
    }
    jetkvm_memory_policy_release(&ticket.memory);
    if (result == ESP_OK) {
        reader->last_response_us = esp_timer_get_time();
    }
    return result;
}

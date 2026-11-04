#include "jetkvm_access.h"
/* Local implementation of JetKVM's browser-offerer WebSocket protocol. */
#include "webrtc_websocket.h"

#include <stdatomic.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "cJSON.h"
#include "http_candidate_policy.h"
#include "jetkvm_cloud_signal.h"
#include "jetkvm_memory_policy.h"
#include "webrtc_transport_private.h"
#include "websocket_origin.h"

#define WS_MESSAGE_MAX JETKVM_CLOUD_SIGNAL_MAX_MESSAGE_SIZE
#define WS_IDLE_TIMEOUT_US (90LL * 1000000LL)

static const char *TAG = "webrtc_ws";

/* Why a signaling socket was closed, logged with the session and generation
 * so a browser reconnect storm can be attributed from the console. */
typedef enum {
    WS_CLOSE_IDLE,         /* no frame from the browser for WS_IDLE_TIMEOUT_US */
    WS_CLOSE_EPOCH,        /* access epoch revoked (password or auth change) */
    WS_CLOSE_REBUILD,      /* transport generation changed: HDMI mode or peer failure */
    WS_CLOSE_SUPERSEDED,   /* another page took the session over */
    WS_CLOSE_SEND_FAILURE, /* an outbound signaling frame could not be sent */
    WS_CLOSE_PEER,         /* the browser (or HTTPD on a protocol error) closed the socket */
    WS_CLOSE_COUNT,
} ws_close_reason_t;
static const char *const s_close_names[WS_CLOSE_COUNT] = {
    "idle", "epoch", "rebuild", "superseded", "send-failure", "peer",
};

typedef struct websocket_listener websocket_listener_t;
typedef struct {
    websocket_listener_t *owner;
    uint32_t access_epoch;
    int fd;
    uint32_t session_id;
    uint32_t generation;
    int64_t last_receive_us;
    webrtc_http_candidate_policy_t candidates;
} websocket_client_t;

/* Client state and all socket writes belong exclusively to the HTTPD task.
 * The timer only schedules one bounded drain; it never touches a socket or
 * allocates signaling payloads. Outbound data retains the transport's shared
 * signaling budget until sent/released. */
struct websocket_listener {
    httpd_handle_t server;
    websocket_client_t *client;
    esp_timer_handle_t timer;
    atomic_bool running, poll_pending;
    atomic_uint timer_callbacks;
};
static websocket_listener_t s_listeners[2];

static esp_err_t send_text(websocket_client_t *client, const char *text, size_t size)
{
    websocket_listener_t *owner = client->owner;
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len = size,
    };
    return httpd_ws_send_frame_async(owner->server, client->fd, &frame);
}

static esp_err_t send_metadata(websocket_client_t *client)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = root ? cJSON_AddObjectToObject(root, "data") : NULL;
    char *wire = NULL;
    if (data && cJSON_AddStringToObject(root, "type", "device-metadata") &&
        cJSON_AddStringToObject(data, "deviceVersion", esp_app_get_description()->version)) {
        wire = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(root);
    if (!wire) return ESP_ERR_NO_MEM;
    esp_err_t err = send_text(client, wire, strlen(wire));
    cJSON_free(wire);
    return err;
}

static void note_close(websocket_client_t *client, ws_close_reason_t reason)
{
    ESP_LOGI(TAG, "signaling socket fd=%d closed: %s (session %" PRIu32 " generation %" PRIu32 ")",
             client->fd, s_close_names[reason], client->session_id, client->generation);
}

static void close_client(websocket_client_t *client)
{
    websocket_listener_t *owner = client->owner;
    if (owner->client == client) owner->client = NULL;
    webrtc_transport_session_close(client->session_id);
    client->session_id = 0;
    /* Already on the HTTPD task: shut down this socket now, then let HTTPD
     * receive EOF and free its session normally. IDF trigger_close queues a
     * sock_db pointer; a peer EOF can free/reuse that slot before the queued
     * work runs, which would reset an unrelated replacement HTTP request. */
    (void)shutdown(client->fd, SHUT_RDWR);
}

static void free_client(void *context)
{
    websocket_listener_t *owner = ((websocket_client_t *)context)->owner;
    websocket_client_t *client = context;
    if (owner->client == client) {
        /* Still the active client: HTTPD closed the socket for us, either on
         * the browser's close frame or EOF, or on a receive error. */
        note_close(client, WS_CLOSE_PEER);
        owner->client = NULL;
    }
    /* Session IDs prevent a delayed old socket close from closing its replacement. */
    webrtc_transport_session_close(client->session_id);
    free(client);
}

static void supersede_client(websocket_client_t *client)
{
    /* Use the signaling socket even if the old peer has not opened RPC yet.
     * A distinct close code is a fallback if the notification is not consumed. */
    static const char notice[] = "{\"type\":\"other-session-connected\"}";
    (void)send_text(client, notice, sizeof(notice) - 1);
    uint8_t code[] = {0x0f, 0xa1}; /* private-use close code 4001 */
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_CLOSE, .payload = code, .len = sizeof(code),
    };
    (void)httpd_ws_send_frame_async(client->owner->server, client->fd, &frame);
    note_close(client, WS_CLOSE_SUPERSEDED);
    close_client(client);
}

static esp_err_t send_event(websocket_client_t *client, const webrtc_signal_event_t *event)
{
    char *wire = jetkvm_memory_policy_control_alloc(WS_MESSAGE_MAX + 1U);
    if (!wire) return ESP_ERR_NO_MEM;
    const char *payload = event->data;
    size_t payload_size = event->size;
    char *rewritten = NULL;
    webrtc_http_candidate_policy_t device_policy;
    if (webrtc_http_candidate_policy_for_device(&client->candidates, &device_policy)) {
        rewritten = jetkvm_memory_policy_control_alloc(WS_MESSAGE_MAX + 1U);
        if (!rewritten) { jetkvm_memory_policy_control_free(wire); return ESP_ERR_NO_MEM; }
        webrtc_http_device_signal_t result = webrtc_http_candidate_policy_rewrite_device_signal(
            &device_policy, client->session_id, event->type == WEBRTC_SIGNAL_SDP,
            payload, payload_size, rewritten, WS_MESSAGE_MAX + 1U);
        if (result != WEBRTC_HTTP_DEVICE_SIGNAL_SEND) {
            jetkvm_memory_policy_control_free(rewritten);
            jetkvm_memory_policy_control_free(wire);
            /* A candidate that the policy does not admit is discarded and
             * signaling goes on. */
            return result == WEBRTC_HTTP_DEVICE_SIGNAL_SKIP ? ESP_OK : ESP_ERR_INVALID_ARG;
        }
        payload = rewritten;
        payload_size = strlen(rewritten);
    }
    size_t size = 0;
    jetkvm_cloud_signal_status_t status;
    if (event->type == WEBRTC_SIGNAL_SDP) {
        status = jetkvm_cloud_signal_encode_answer(
            payload, payload_size, wire, WS_MESSAGE_MAX + 1U, &size);
    } else {
        const char *candidate = payload;
        size_t length = payload_size;
        if (length >= 2 && memcmp(candidate, "a=", 2) == 0) {
            candidate += 2;
            length -= 2;
        }
        while (length && (candidate[length - 1] == '\r' || candidate[length - 1] == '\n')) --length;
        const jetkvm_cloud_signal_candidate_view_t view = {
            .candidate = candidate, .candidate_length = length,
            .sdp_mid = "0", .sdp_mid_length = 1, .sdp_mline_index = 0,
        };
        status = jetkvm_cloud_signal_encode_candidate(&view, wire, WS_MESSAGE_MAX + 1U, &size);
    }
    esp_err_t err = status == JETKVM_CLOUD_SIGNAL_OK ?
        send_text(client, wire, size) : ESP_FAIL;
    jetkvm_memory_policy_control_free(wire);
    jetkvm_memory_policy_control_free(rewritten);
    return err;
}

static void poll_client(void *arg)
{
    websocket_listener_t *owner = arg;
    (void)arg;
    if (!atomic_load(&owner->running) || !owner->client) goto done;
    websocket_client_t *client = owner->client;
    if (!jetkvm_access_epoch_valid(client->access_epoch)) {
        note_close(client, WS_CLOSE_EPOCH);
        close_client(client);
        goto done;
    }
    if (esp_timer_get_time() - client->last_receive_us > WS_IDLE_TIMEOUT_US) {
        /* HTTPD may have spent the interval serving a synchronous upload.
         * Let its receive handler consume queued heartbeats before deciding
         * the browser is idle. Only a complete received frame refreshes the
         * timestamp; a disconnected or genuinely idle socket still closes. */
        uint8_t pending;
        if (recv(client->fd, &pending, sizeof(pending), MSG_PEEK | MSG_DONTWAIT) <= 0) {
            note_close(client, WS_CLOSE_IDLE);
            close_client(client);
            goto done;
        }
    }
    /* Keep an active signaling socket out of HTTP keep-alive LRU eviction. */
    httpd_sess_update_lru_counter(owner->server, client->fd);
    for (unsigned i = 0; client->session_id && i < 8; ++i) {
        webrtc_signal_event_t event = {0};
        uint32_t generation = 0;
        esp_err_t err = webrtc_transport_websocket_session_poll(client->session_id, &event, &generation);
        if (err != ESP_OK || generation != client->generation) {
            /* Rebuilds (HDMI mode, peer failure) need a fresh browser offer.
             * A new socket also isolates late candidates from the old offer. */
            webrtc_transport_signal_event_release(&event);
            if (webrtc_transport_session_superseded(client->session_id)) {
                supersede_client(client);
            } else {
                note_close(client, WS_CLOSE_REBUILD);
                close_client(client);
            }
            goto done;
        }
        if (event.type == WEBRTC_SIGNAL_NONE) break;
        err = send_event(client, &event);
        webrtc_transport_signal_event_release(&event);
        if (err != ESP_OK) {
            webrtc_transport_signal_record_drop();
            note_close(client, WS_CLOSE_SEND_FAILURE);
            close_client(client);
            goto done;
        }
    }
done:
    atomic_store(&owner->poll_pending, false);
}

static void schedule_poll(void *arg)
{
    websocket_listener_t *owner = arg;
    (void)arg;
    atomic_fetch_add(&owner->timer_callbacks, 1);
    if (atomic_load(&owner->running) && !atomic_exchange(&owner->poll_pending, true)) {
        if (httpd_queue_work(owner->server, poll_client, owner) != ESP_OK) {
            atomic_store(&owner->poll_pending, false);
        }
    }
    atomic_fetch_sub(&owner->timer_callbacks, 1);
}

static bool socket_ipv4(int fd, bool peer, webrtc_http_ipv4_t *result)
{
    struct sockaddr_storage address = {0};
    socklen_t size = sizeof(address);
    int err = peer ? getpeername(fd, (struct sockaddr *)&address, &size) :
                     getsockname(fd, (struct sockaddr *)&address, &size);
    if (err) return false;
    if (address.ss_family == AF_INET) {
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)&address;
        uint32_t ip = ntohl(ipv4->sin_addr.s_addr);
        for (unsigned i = 0; i < 4; ++i) result->octet[i] = ip >> (24 - 8 * i);
        return true;
    }
    if (address.ss_family == AF_INET6) {
        const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)&address;
        const uint8_t *bytes = ipv6->sin6_addr.s6_addr;
        static const uint8_t mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(bytes, mapped_prefix, sizeof(mapped_prefix)) == 0) {
            memcpy(result->octet, bytes + sizeof(mapped_prefix), sizeof(result->octet));
            return true;
        }
    }
    return false;
}

/* Log only the candidate's eight base fields, never SDP credentials or
 * candidate extensions. Bound and sanitize input from the browser. */
static void log_candidate(uint32_t session_id, const char *source,
                          const char *candidate, size_t size)
{
    char summary[384];
    size_t used = 0;
    unsigned fields = 0;
    bool in_field = false;
    for (size_t i = 0; i < size && used + 1U < sizeof(summary); ++i) {
        unsigned char value = (unsigned char)candidate[i];
        if (value == ' ') {
            if (in_field && ++fields == 8U) break;
            in_field = false;
        } else {
            in_field = true;
        }
        summary[used++] = value >= 0x20U && value <= 0x7eU ? (char)value : '?';
    }
    summary[used] = '\0';
    ESP_LOGI("webrtc_ice", "session=%lu %s %s",
             (unsigned long)session_id, source, summary);
}

static void log_sdp_candidates(uint32_t session_id, const char *sdp, size_t size)
{
    for (size_t start = 0; start < size;) {
        size_t end = start;
        while (end < size && sdp[end] != '\r' && sdp[end] != '\n') ++end;
        if (end - start >= 12U && memcmp(sdp + start, "a=candidate:", 12U) == 0) {
            log_candidate(session_id, "offer-original", sdp + start + 2U, end - start - 2U);
        }
        start = end + 1U;
    }
}

static esp_err_t receive_signal(websocket_client_t *client, const uint8_t *wire, size_t size)
{
    if (size == 4 && memcmp(wire, "ping", 4) == 0) {
        return send_text(client, "pong", 4);
    }
    /* The shared codec validates complete JSON, duplicate fields, UTF-8,
     * base64 and size/depth limits before any SDP enters the peer task. */
    jetkvm_cloud_signal_offer_t offer = {0};
    if (jetkvm_cloud_signal_parse_offer(wire, size, &offer) == JETKVM_CLOUD_SIGNAL_OK) {
        esp_err_t err = webrtc_transport_websocket_session_create(
            client->access_epoch, &client->session_id, &client->generation);
        if (err == ESP_OK) {
            webrtc_http_ipv4_t peer, device;
            bool have_peer = socket_ipv4(client->fd, true, &peer);
            bool have_device = socket_ipv4(client->fd, false, &device);
            webrtc_http_candidate_policy_begin(&client->candidates, client->session_id,
                have_peer ? &peer : NULL, have_device ? &device : NULL);
            char *sdp = jetkvm_memory_policy_control_alloc(JETKVM_CLOUD_SIGNAL_MAX_SDP_SIZE + 1U);
            if (!sdp) {
                err = ESP_ERR_NO_MEM;
            } else {
                log_sdp_candidates(client->session_id, offer.sdp.data, offer.sdp.length);
                webrtc_http_candidate_result_t result = webrtc_http_candidate_policy_rewrite_sdp(
                    &client->candidates, client->session_id, offer.sdp.data, offer.sdp.length,
                    sdp, JETKVM_CLOUD_SIGNAL_MAX_SDP_SIZE + 1U);
                err = result == WEBRTC_HTTP_CANDIDATE_OK ?
                    webrtc_transport_session_submit(client->session_id, client->generation,
                        WEBRTC_SIGNAL_SDP, sdp, strlen(sdp)) : ESP_ERR_INVALID_ARG;
                jetkvm_memory_policy_control_free(sdp);
            }
        }
        jetkvm_cloud_signal_offer_free(&offer);
        return err;
    }
    jetkvm_cloud_signal_candidate_t candidate = {0};
    if (jetkvm_cloud_signal_parse_candidate(wire, size, &candidate) != JETKVM_CLOUD_SIGNAL_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    char rewritten[WEBRTC_HTTP_CANDIDATE_REWRITTEN_MAX];
    log_candidate(client->session_id, "trickle-original",
                  candidate.candidate.data, candidate.candidate.length);
    webrtc_http_candidate_result_t rewrite = webrtc_http_candidate_policy_rewrite(
        &client->candidates, client->session_id, candidate.candidate.data,
        candidate.candidate.length, rewritten, sizeof(rewritten));
    jetkvm_cloud_signal_candidate_free(&candidate);
    /* A candidate is optional: unsupported addresses, early trickle, local
     * limits or queue pressure must not tear down a usable candidate pair. */
    if (rewrite != WEBRTC_HTTP_CANDIDATE_OK) {
        ESP_LOGI("webrtc_ice", "session=%lu candidate rejected policy_result=%d",
                 (unsigned long)client->session_id, (int)rewrite);
        webrtc_transport_signal_record_drop();
        return ESP_OK;
    }
    esp_err_t err = webrtc_transport_session_submit(client->session_id, client->generation,
        WEBRTC_SIGNAL_CANDIDATE, rewritten, strlen(rewritten));
    /* Submit already counts allocation/budget/queue failures. */
    if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_ARG) {
        webrtc_transport_signal_record_drop();
    }
    return ESP_OK;
}

static esp_err_t websocket_check_origin(httpd_req_t *req)
{
    if (jetkvm_access_check_request(req, true) != ESP_OK) return ESP_FAIL;
    char host[320];
    char origin[sizeof(host) + 8];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
        !webrtc_websocket_origin_allowed(origin, host)) {
        (void)httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Device UI origin required");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t websocket_open(httpd_req_t *req)
{
    websocket_listener_t *owner = req->user_ctx;
    websocket_client_t *client = calloc(1, sizeof(*client));
    if (!client) return ESP_ERR_NO_MEM;
    client->owner = owner;
    client->access_epoch = jetkvm_access_request_epoch(req);
    if (!jetkvm_access_lock_epoch(client->access_epoch)) { free(client); return ESP_FAIL; }
    client->fd = httpd_req_to_sockfd(req);
    client->last_receive_us = esp_timer_get_time();
    if (owner->client) supersede_client(owner->client);
    owner->client = client;
    req->sess_ctx = client;
    req->free_ctx = free_client;
    jetkvm_access_unlock();
    return send_metadata(client);
}

static esp_err_t websocket_handler(httpd_req_t *req)
{
    websocket_listener_t *owner = req->user_ctx;
    if (req->method == HTTP_GET) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "WebSocket upgrade required");
    }
    websocket_client_t *client = req->sess_ctx;
    if (!client || client != owner->client ||
        !jetkvm_access_epoch_valid(client->access_epoch)) return ESP_FAIL;
    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) return err;
    if (frame.type == HTTPD_WS_TYPE_CLOSE) return ESP_FAIL;
    if (frame.type == HTTPD_WS_TYPE_PONG) {
        uint8_t payload[125];
        if (!frame.final || frame.len > sizeof(payload)) return ESP_ERR_INVALID_SIZE;
        frame.payload = payload;
        if (frame.len) err = httpd_ws_recv_frame(req, &frame, sizeof(payload));
        if (err == ESP_OK) client->last_receive_us = esp_timer_get_time();
        return err;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT || !frame.final ||
        frame.len == 0 || frame.len > WS_MESSAGE_MAX) return ESP_ERR_INVALID_SIZE;
    uint8_t *wire = jetkvm_memory_policy_control_alloc(frame.len + 1U);
    if (!wire) return ESP_ERR_NO_MEM;
    frame.payload = wire;
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err == ESP_OK) {
        wire[frame.len] = 0;
        client->last_receive_us = esp_timer_get_time();
        err = receive_signal(client, wire, frame.len);
    }
    jetkvm_memory_policy_control_free(wire);
    if (err != ESP_OK) webrtc_transport_signal_record_drop();
    return err;
}

esp_err_t webrtc_websocket_start(httpd_handle_t server)
{
    websocket_listener_t *owner = NULL;
    for (size_t i = 0; i < 2; ++i) if (!s_listeners[i].server) { owner = &s_listeners[i]; break; }
    if (!owner) return ESP_ERR_NO_MEM;
    memset(owner, 0, sizeof(*owner));
    owner->server = server;
    const httpd_uri_t uri = {
        .uri = "/webrtc/signaling/client", .method = HTTP_GET,
        .handler = websocket_handler, .is_websocket = true, .user_ctx = owner,
        /* Reject foreign pages before upgrading or replacing the active client. */
        .ws_pre_handshake_cb = websocket_check_origin,
        /* IDF 6 does not invoke the URI handler for the upgrade request. */
        .ws_post_handshake_cb = websocket_open,
    };
    esp_err_t err = httpd_register_uri_handler(server, &uri);
    if (err != ESP_OK) return err;
    atomic_store(&owner->poll_pending, false);
    const esp_timer_create_args_t args = {.callback = schedule_poll, .arg = owner, .name = "webrtc_ws"};
    err = esp_timer_create(&args, &owner->timer);
    if (err == ESP_OK) {
        atomic_store(&owner->running, true);
        err = esp_timer_start_periodic(owner->timer, 50000);
    }
    if (err != ESP_OK) webrtc_websocket_stop(server);
    return err;
}

void webrtc_websocket_stop(httpd_handle_t server)
{
    websocket_listener_t *owner = NULL;
    for (size_t i = 0; i < 2; ++i) if (s_listeners[i].server == server) owner = &s_listeners[i];
    if (!owner) return;
    atomic_store(&owner->running, false);
    if (owner->timer) {
        (void)esp_timer_stop(owner->timer);
        (void)esp_timer_delete(owner->timer);
        owner->timer = NULL;
    }
    /* No timer callback may enqueue work after the caller stops HTTPD. */
    while (atomic_load(&owner->timer_callbacks)) vTaskDelay(1);
}

void webrtc_websocket_forget(httpd_handle_t server)
{
    /* HTTPD has joined, including queued work and socket free callbacks. */
    for (size_t i = 0; i < 2; ++i)
        if (s_listeners[i].server == server) memset(&s_listeners[i], 0, sizeof(s_listeners[i]));
}

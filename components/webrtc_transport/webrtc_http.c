#include "jetkvm_tls.h"
#include "esp_https_server.h"
#include "freertos/semphr.h"
#include "jetkvm_access.h"
/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "webrtc_transport_private.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_api.h"
#include "diagnostics.h"
#include "jetkvm_virtual_media_http.h"
#include "jetkvm_virtual_media.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "http_candidate_policy.h"
#include "jetkvm_ethernet_stats.h"
#include "jetkvm_hid_input.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_timing_log.h"
#include "jetkvm_usb_service.h"
#include "playout_delay.h"
#include "web_app.h"
#include "webrtc_transport.h"
#include "webrtc_stats_json.h"
#include "webrtc_websocket.h"

#define SIGNAL_BODY_MAX (36U * 1024U)

_Static_assert(sizeof(void *) == JETKVM_MEMORY_LWIP_MBOX_POINTER_BYTES,
               "lwIP mailbox memory budget requires 32-bit pointers");

static const char *TAG = "webrtc_http";
static httpd_handle_t s_server;
static httpd_handle_t s_secure_server;
static webrtc_http_candidate_policy_t s_candidate_policies[2];
static webrtc_http_candidate_policy_t *candidate_policy(httpd_req_t *req)
{
    return &s_candidate_policies[req->handle == s_server ? 0 : 1];
}

extern const unsigned char acceptance_page_html_start[]
    asm("_binary_acceptance_page_html_start");
extern const unsigned char acceptance_page_html_end[]
    asm("_binary_acceptance_page_html_end");

static void set_common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
}

/* Consumes the rendered string, including on a failed HTTP send. */
static esp_err_t send_json_string(httpd_req_t *req, char *json)
{
    if (!json) {
        (void)httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                  "JSON allocation failed");
        return ESP_ERR_NO_MEM;
    }
    set_common_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    return send_json_string(req, cJSON_PrintUnformatted(root));
}

static esp_err_t send_status_text(httpd_req_t *req, const char *status,
                                  const char *message)
{
    set_common_headers(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message);
}

static bool request_ipv4(httpd_req_t *req, bool peer,
                         webrtc_http_ipv4_t *address)
{
    if (req == NULL || address == NULL) {
        return false;
    }
    int socket = httpd_req_to_sockfd(req);
    if (socket < 0) {
        return false;
    }
    struct sockaddr_storage endpoint = { 0 };
    socklen_t endpoint_size = sizeof(endpoint);
    int result = peer ?
        getpeername(socket, (struct sockaddr *)&endpoint, &endpoint_size) :
        getsockname(socket, (struct sockaddr *)&endpoint, &endpoint_size);
    if (result != 0) {
        return false;
    }
    if (endpoint.ss_family == AF_INET &&
        endpoint_size >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *ipv4 =
            (const struct sockaddr_in *)&endpoint;
        if (ipv4->sin_addr.s_addr == htonl(INADDR_ANY)) {
            return false;
        }
        uint32_t host_order = ntohl(ipv4->sin_addr.s_addr);
        address->octet[0] = (uint8_t)(host_order >> 24U);
        address->octet[1] = (uint8_t)(host_order >> 16U);
        address->octet[2] = (uint8_t)(host_order >> 8U);
        address->octet[3] = (uint8_t)host_order;
        return true;
    }
    if (endpoint.ss_family == AF_INET6 &&
        endpoint_size >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *ipv6 =
            (const struct sockaddr_in6 *)&endpoint;
        const uint8_t *bytes = ipv6->sin6_addr.s6_addr;
        static const uint8_t mapped_prefix[12] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff,
        };
        if (memcmp(bytes, mapped_prefix, sizeof(mapped_prefix)) == 0) {
            memcpy(address->octet, bytes + sizeof(mapped_prefix),
                   sizeof(address->octet));
            return true;
        }
    }
    return false;
}

static bool ethernet_ipv4(webrtc_http_ipv4_t *address)
{
    if (address == NULL) {
        return false;
    }
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    esp_netif_ip_info_t info = { 0 };
    if (netif == NULL || esp_netif_get_ip_info(netif, &info) != ESP_OK ||
        info.ip.addr == 0U || info.ip.addr == IPADDR_NONE) {
        return false;
    }
    uint32_t host_order = ntohl(info.ip.addr);
    address->octet[0] = (uint8_t)(host_order >> 24U);
    address->octet[1] = (uint8_t)(host_order >> 16U);
    address->octet[2] = (uint8_t)(host_order >> 8U);
    address->octet[3] = (uint8_t)host_order;
    return true;
}

static bool enforce_session_owner(httpd_req_t *req, uint32_t session_id,
                                  esp_err_t *response_result)
{
    *response_result = ESP_OK;
    webrtc_http_ipv4_t peer;
    const webrtc_http_ipv4_t *peer_ptr =
        request_ipv4(req, true, &peer) ? &peer : NULL;
    webrtc_http_owner_result_t result =
        webrtc_http_candidate_policy_authorize(
            candidate_policy(req), session_id, peer_ptr);
    if (result == WEBRTC_HTTP_OWNER_ALLOW) {
        return true;
    }
    if (result == WEBRTC_HTTP_OWNER_PEER_REQUIRED) {
        *response_result = send_status_text(
            req, "403 Forbidden", "session owner address unavailable");
        return false;
    }
    *response_result = send_status_text(
        req, "403 Forbidden", "session owner mismatch");
    return false;
}

static esp_err_t page_handler(httpd_req_t *req)
{
    if (jetkvm_access_check_host(req) != ESP_OK) return jetkvm_access_rejection_result(req);
    size_t path_length = strcspn(req->uri, "?");
    if (web_app_is_ready() && path_length == 1 && req->uri[0] == '/') {
        return web_app_serve_index(req);
    }
    set_common_headers(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    size_t size = (size_t)(acceptance_page_html_end - acceptance_page_html_start);
    return httpd_resp_send(req, (const char *)acceptance_page_html_start, size);
}

static esp_err_t session_handler(httpd_req_t *req)
{
    bool sender_playout_delay = true;
    char query[96];
    char value[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "senderPlayoutDelay", value,
                              sizeof(value)) == ESP_OK &&
        (strcmp(value, "0") == 0 || strcmp(value, "off") == 0)) {
        sender_playout_delay = false;
    }
    uint32_t session_id = 0;
    uint32_t generation = 0;
    webrtc_http_ipv4_t peer;
    webrtc_http_ipv4_t device;
    bool have_peer = request_ipv4(req, true, &peer);
    bool have_device = request_ipv4(req, false, &device);
    if (!have_device) {
        have_device = ethernet_ipv4(&device);
    }
    esp_err_t err = webrtc_transport_session_create(
        jetkvm_access_request_epoch(req), sender_playout_delay, &session_id, &generation);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "transport is not running");
    }
    webrtc_http_candidate_policy_begin(
        candidate_policy(req), session_id, have_peer ? &peer : NULL,
        have_device ? &device : NULL);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        webrtc_transport_session_close(session_id);
        webrtc_http_candidate_policy_clear(candidate_policy(req), session_id);
        webrtc_transport_signal_record_drop();
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    char id[9];
    snprintf(id, sizeof(id), "%08" PRIx32, session_id);
    bool complete = cJSON_AddStringToObject(root, "session", id) != NULL &&
        cJSON_AddNumberToObject(root, "generation", generation) != NULL &&
        cJSON_AddStringToObject(
            root, "policy", "single-client-replaces-previous") != NULL &&
        cJSON_AddBoolToObject(root, "senderPlayoutDelayRequested",
                             sender_playout_delay) != NULL;
    char client_address[16] = "unavailable";
    char device_address[16] = "unavailable";
    if (complete && have_peer) {
        complete = webrtc_http_ipv4_format(
                       &peer, client_address, sizeof(client_address)) &&
            cJSON_AddStringToObject(
                root, "clientAddress", client_address) != NULL;
    }
    if (complete && have_device) {
        complete = webrtc_http_ipv4_format(
                       &device, device_address, sizeof(device_address)) &&
            cJSON_AddStringToObject(
                root, "deviceAddress", device_address) != NULL;
    }
    if (complete && candidate_policy(req)->rewrite_enabled) {
        complete = cJSON_AddStringToObject(
                       root, "candidatePolicy",
                       WEBRTC_HTTP_CANDIDATE_POLICY_NAME) != NULL;
    }
    ESP_LOGI(TAG,
             "HTTP ICE session=%08" PRIx32 " client=%s device=%s rewrite=%d",
             session_id, client_address, device_address,
             candidate_policy(req)->rewrite_enabled);
    if (!complete) {
        cJSON_Delete(root);
        webrtc_transport_session_close(session_id);
        webrtc_http_candidate_policy_clear(candidate_policy(req), session_id);
        webrtc_transport_signal_record_drop();
        return httpd_resp_send_err(
            req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    err = send_json(req, root);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        webrtc_transport_session_close(session_id);
        webrtc_http_candidate_policy_clear(candidate_policy(req), session_id);
        webrtc_transport_signal_record_drop();
    }
    return err;
}

static esp_err_t query_session_id(httpd_req_t *req, uint32_t *session_id)
{
    char query[96];
    char value[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "session", value, sizeof(value)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 16);
    if (!value[0] || !end || *end || parsed > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    *session_id = (uint32_t)parsed;
    return ESP_OK;
}

static esp_err_t events_handler(httpd_req_t *req)
{
    uint32_t session_id = 0;
    if (query_session_id(req, &session_id) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid session");
    }
    esp_err_t owner_response = ESP_OK;
    if (!enforce_session_owner(req, session_id, &owner_response)) {
        return owner_response;
    }
    webrtc_signal_event_t event = { 0 };
    uint32_t current_generation = 0;
    esp_err_t err = webrtc_transport_session_poll(session_id, &event,
                                                   &current_generation);
    if (err == ESP_ERR_NOT_FOUND) {
        webrtc_http_candidate_policy_clear(candidate_policy(req), session_id);
        httpd_resp_set_status(req, "410 Gone");
        return httpd_resp_sendstr(req, "{\"error\":\"session replaced or expired\"}");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "signaling poll failed");
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        webrtc_transport_signal_record_drop();
        webrtc_transport_signal_event_release(&event);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    const char *signal = event.data;
    char *rewritten = NULL;
    webrtc_signal_type_t type = event.type;
    webrtc_http_candidate_policy_t device_policy;
    if ((type == WEBRTC_SIGNAL_SDP || type == WEBRTC_SIGNAL_CANDIDATE) &&
        webrtc_http_candidate_policy_for_device(candidate_policy(req), &device_policy)) {
        rewritten = jetkvm_memory_policy_control_alloc(SIGNAL_BODY_MAX + 1U);
        webrtc_http_device_signal_t result = !rewritten ? WEBRTC_HTTP_DEVICE_SIGNAL_FAIL :
            webrtc_http_candidate_policy_rewrite_device_signal(&device_policy, session_id,
                type == WEBRTC_SIGNAL_SDP, signal, event.size, rewritten, SIGNAL_BODY_MAX + 1U);
        if (result == WEBRTC_HTTP_DEVICE_SIGNAL_FAIL) {
            cJSON_Delete(root);
            jetkvm_memory_policy_control_free(rewritten);
            webrtc_transport_signal_event_release(&event);
            webrtc_transport_signal_record_drop();
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                      "tunnel candidate rewrite failed");
        }
        /* A candidate that the policy does not admit is discarded; the poll
         * answers idle and signaling goes on. */
        if (result == WEBRTC_HTTP_DEVICE_SIGNAL_SKIP) type = WEBRTC_SIGNAL_NONE;
        signal = rewritten;
    }
    bool complete = cJSON_AddNumberToObject(
        root, "generation", type == WEBRTC_SIGNAL_NONE ?
            current_generation : event.generation) != NULL;
    switch (type) {
    case WEBRTC_SIGNAL_SDP:
        complete = complete &&
            cJSON_AddStringToObject(root, "type", "offer") != NULL &&
            cJSON_AddStringToObject(
                root, "sdp", signal ? signal : "") != NULL;
        break;
    case WEBRTC_SIGNAL_CANDIDATE:
        complete = complete &&
            cJSON_AddStringToObject(root, "type", "candidate") != NULL &&
            cJSON_AddStringToObject(
                root, "candidate", signal ? signal : "") != NULL;
        break;
    case WEBRTC_SIGNAL_BYE:
        complete = complete &&
            cJSON_AddStringToObject(root, "type", "bye") != NULL;
        break;
    default:
        complete = complete &&
            cJSON_AddStringToObject(root, "type", "idle") != NULL;
        break;
    }
    err = complete ? send_json(req, root) : ESP_ERR_NO_MEM;
    cJSON_Delete(root);
    jetkvm_memory_policy_control_free(rewritten);
    webrtc_transport_signal_event_release(&event);
    if (err != ESP_OK) {
        webrtc_transport_signal_record_drop();
        if (!complete) {
            return httpd_resp_send_err(
                req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        }
    }
    return err;
}

static esp_err_t receive_body(httpd_req_t *req, char **body)
{
    if (req->content_len <= 0 || req->content_len > SIGNAL_BODY_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *buffer = jetkvm_memory_policy_control_alloc(
        (size_t)req->content_len + 1U);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }
    int64_t deadline = esp_timer_get_time() + 10000000;
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        if (esp_timer_get_time() >= deadline) {
            jetkvm_memory_policy_control_free(buffer);
            return ESP_ERR_TIMEOUT;
        }
        int ret = httpd_req_recv(req, buffer + received,
                                 (size_t)req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT && esp_timer_get_time() < deadline) continue;
        if (ret <= 0) {
            jetkvm_memory_policy_control_free(buffer);
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    buffer[received] = '\0';
    *body = buffer;
    return ESP_OK;
}

static bool json_session_id(cJSON *root, uint32_t *session_id)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "session");
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(item->valuestring, &end, 16);
    if (!item->valuestring[0] || !end || *end || parsed > UINT32_MAX) {
        return false;
    }
    *session_id = (uint32_t)parsed;
    return true;
}

static esp_err_t signal_handler(httpd_req_t *req)
{
    char *body = NULL;
    esp_err_t err = receive_body(req, &body);
    if (err != ESP_OK) {
        httpd_resp_set_hdr(req, "Connection", "close");
        if (err == ESP_ERR_NO_MEM) {
            webrtc_transport_signal_record_drop();
            httpd_resp_set_status(req, "503 Service Unavailable");
            (void)httpd_resp_sendstr(req, "signaling memory unavailable");
        } else {
            (void)httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid signaling body");
        }
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    jetkvm_memory_policy_control_free(body);
    if (!root) {
        webrtc_transport_signal_record_drop();
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    }

    uint32_t session_id = 0;
    cJSON *generation_item = cJSON_GetObjectItemCaseSensitive(root, "generation");
    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!json_session_id(root, &session_id) || !cJSON_IsNumber(generation_item) ||
        generation_item->valuedouble < 0 || generation_item->valuedouble > UINT32_MAX ||
        (double)(uint32_t)generation_item->valuedouble != generation_item->valuedouble ||
        !cJSON_IsString(type_item) || !type_item->valuestring) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "session, generation, and type are required");
    }
    uint32_t generation = (uint32_t)generation_item->valuedouble;
    esp_err_t owner_response = ESP_OK;
    if (!enforce_session_owner(req, session_id, &owner_response)) {
        cJSON_Delete(root);
        return owner_response;
    }
    webrtc_signal_type_t type = WEBRTC_SIGNAL_NONE;
    cJSON *payload = NULL;
    if (strcmp(type_item->valuestring, "answer") == 0) {
        type = WEBRTC_SIGNAL_SDP;
        payload = cJSON_GetObjectItemCaseSensitive(root, "sdp");
    } else if (strcmp(type_item->valuestring, "candidate") == 0) {
        type = WEBRTC_SIGNAL_CANDIDATE;
        payload = cJSON_GetObjectItemCaseSensitive(root, "candidate");
    } else if (strcmp(type_item->valuestring, "bye") == 0) {
        type = WEBRTC_SIGNAL_BYE;
    } else {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown signaling type");
    }
    if (type != WEBRTC_SIGNAL_BYE && (!cJSON_IsString(payload) || !payload->valuestring)) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing signaling payload");
    }
    const char *data = payload && payload->valuestring ? payload->valuestring : NULL;
    size_t size = data ? strlen(data) : 0;
    char rewritten_candidate[WEBRTC_HTTP_CANDIDATE_REWRITTEN_MAX];
    if (type == WEBRTC_SIGNAL_CANDIDATE) {
        webrtc_http_candidate_result_t rewrite =
            webrtc_http_candidate_policy_rewrite(
                candidate_policy(req), session_id, data, size,
                rewritten_candidate, sizeof(rewritten_candidate));
        if (rewrite != WEBRTC_HTTP_CANDIDATE_OK) {
            cJSON_Delete(root);
            switch (rewrite) {
            case WEBRTC_HTTP_CANDIDATE_POLICY_UNAVAILABLE:
                return send_status_text(
                    req, "409 Conflict",
                    "HTTP peer candidate rewrite policy unavailable");
            case WEBRTC_HTTP_CANDIDATE_TOO_LONG:
                return send_status_text(req, "413 Payload Too Large",
                                        "ICE candidate exceeds 1024 bytes");
            case WEBRTC_HTTP_CANDIDATE_LIMIT_REACHED:
                return send_status_text(req, "429 Too Many Requests",
                                        "ICE candidate limit reached");
            case WEBRTC_HTTP_CANDIDATE_INVALID:
                return httpd_resp_send_err(
                    req, HTTPD_400_BAD_REQUEST,
                    "ICE candidate must be component-1 UDP host IPv4 or mDNS");
            default:
                return httpd_resp_send_err(
                    req, HTTPD_500_INTERNAL_SERVER_ERROR,
                    "ICE candidate rewrite failed");
            }
        }
        data = rewritten_candidate;
        size = strlen(rewritten_candidate);
    }
    if (!jetkvm_access_lock_epoch(jetkvm_access_request_epoch(req))) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Authentication expired");
    }
    err = webrtc_transport_session_submit(session_id, generation, type, data, size);
    jetkvm_access_unlock();
    cJSON_Delete(root);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "stale signaling generation");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "failed to enqueue signaling message");
    }
    if (type == WEBRTC_SIGNAL_BYE) {
        webrtc_http_candidate_policy_clear(candidate_policy(req), session_id);
    }
    set_common_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* Building and printing the full stats takes about 150 ms of CPU, 35 ms with
 * ?bitrate=1. This server's task (priority 5) is on core 0, where
 * configure_http() puts it, and would keep the OS Services task (priority 4,
 * core 0) from reading USB all that time. The stats are diagnostics: they
 * are built below it. */
#define STATS_BUILD_PRIORITY 3

/* The timing log (jetkvm_timing_log.h) for timing=<action>: "start" starts
 * an empty log first, "stop" stops the log first, so the answer is the
 * stopped log, which has no entries; each other value only reads. The stop
 * is before the read so that the log's memory is free also when there is
 * none for the answer. NULL with *error set when there is no memory. */
static char *render_timing_log(const char *action, esp_err_t *error)
{
    *error = strcmp(action, "start") == 0 ? jetkvm_timing_log_start() : ESP_OK;
    if (*error != ESP_OK) {
        return NULL;
    }
    if (strcmp(action, "stop") == 0) {
        jetkvm_timing_log_stop();
    }
    char *json = jetkvm_timing_log_json();
    if (json == NULL) {
        *error = ESP_ERR_NO_MEM;
    }
    return json;
}

/* Sends the timing log's string and frees it. It is from the system heap:
 * send_json_string() frees to cJSON's own heap. */
static esp_err_t send_timing_log(httpd_req_t *req, char *json)
{
    set_common_headers(req);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

/* The stats the query asks for, printed; NULL with *error set when they
 * could not be built. */
static char *render_stats(const char *query, bool has_query, esp_err_t *error)
{
    char value[8];
    bool detailed = has_query &&
        httpd_query_key_value(query, "details", value, sizeof(value)) == ESP_OK &&
        strcmp(value, "1") == 0;
    bool bitrate = has_query &&
        httpd_query_key_value(query, "bitrate", value, sizeof(value)) == ESP_OK &&
        strcmp(value, "1") == 0;
    cJSON *root = NULL;
    *error = bitrate ? webrtc_stats_json_create_bitrate(&root) :
             detailed ? webrtc_stats_json_create_detailed(&root) :
                        webrtc_stats_json_create(&root);
    if (*error != ESP_OK) {
        return NULL;
    }
    if (bitrate && httpd_query_key_value(query, "resetVideoIntervals", value,
                                         sizeof(value)) == ESP_OK && strcmp(value, "1") == 0) {
        webrtc_transport_reset_video_intervals();
    }
    char *json = cJSON_PrintUnformatted(root);
    /* The access guard holds its authority mutex through this handler.
     * Release the large stats tree before the response can trigger a new RPC,
     * so the peer task does not wait on post-response cleanup. */
    cJSON_Delete(root);
    return json;
}

static esp_err_t stats_handler(httpd_req_t *req)
{
    char query[64];
    const bool has_query = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK;
    char timing[8];
    const bool timing_log = has_query &&
        httpd_query_key_value(query, "timing", timing, sizeof(timing)) == ESP_OK;
    vTaskPrioritySet(NULL, STATS_BUILD_PRIORITY);
    esp_err_t error = ESP_OK;
    char *json = timing_log ? render_timing_log(timing, &error) :
                              render_stats(query, has_query, &error);
    /* The configured priority, not the one read before: a task waiting on
     * the access guard's mutex may have lent this task its own. */
    vTaskPrioritySet(NULL, JETKVM_HTTP_SERVER_TASK_PRIORITY);
    if (error != ESP_OK) {
        const char *message = error == ESP_ERR_NO_MEM ?
            "no memory" : "stats unavailable";
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   message);
    }
    return timing_log ? send_timing_log(req, json) : send_json_string(req, json);
}


static esp_err_t register_uri(httpd_handle_t server, const char *uri, httpd_method_t method,
                              esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t config = {.uri = uri, .method = method, .handler = handler};
    return strcmp(uri, "/") == 0 ? httpd_register_uri_handler(server, &config) :
        jetkvm_access_register(server, &config);
}

static esp_err_t register_routes(httpd_handle_t server, bool secure)
{
    esp_err_t err;
    if ((err = jetkvm_access_attach(server, secure)) != ESP_OK ||
        (err = register_uri(server, "/", HTTP_GET, page_handler)) != ESP_OK ||
        (err = register_uri(server, "/webrtc", HTTP_GET, page_handler)) != ESP_OK ||
        (err = register_uri(server, "/webrtc/session", HTTP_POST, session_handler)) != ESP_OK ||
        (err = register_uri(server, "/webrtc/events", HTTP_GET, events_handler)) != ESP_OK ||
        (err = register_uri(server, "/webrtc/signal", HTTP_POST, signal_handler)) != ESP_OK ||
        (err = register_uri(server, "/webrtc/stats", HTTP_GET, stats_handler)) != ESP_OK ||
        (err = register_uri(server, "/diagnostics", HTTP_GET, diagnostics_handler)) != ESP_OK ||
        (err = webrtc_websocket_start(server)) != ESP_OK ||
        (err = p4_device_api_register_handlers(server)) != ESP_OK ||
        (err = jetkvm_virtual_media_http_register(server)) != ESP_OK ||
        (web_app_is_ready() && (err = web_app_register_handlers(server)) != ESP_OK) ||
        (web_app_is_ready() && (err = web_app_register_spa_fallback(server)) != ESP_OK)) return err;
    return ESP_OK;
}

/* Small JSON replies leave the last segment stuck behind Nagle until the
 * browser's next request on that connection, so Chromium never reports the
 * fetch finished and a page waiting for network idle hangs.  Upstream's Go
 * server writes with TCP_NODELAY set by the runtime. */
static esp_err_t http_session_opened(httpd_handle_t server, int fd)
{
    (void)server;
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return ESP_OK;
}

static void configure_http(httpd_config_t *config)
{
    config->task_priority = JETKVM_HTTP_SERVER_TASK_PRIORITY;
    /* Core 0: during video the peer and encoder tasks keep core 1 busy, and
     * a handler there waits for them. A task without a core stays on the
     * core on which it first uses the FPU, and the stats handler prints
     * doubles, so each boot would choose this task's core. */
    config->core_id = 0;
    /* The stack must stay in internal RAM: handlers persist settings and
     * passwords to NVS, and a flash write runs with the cache disabled,
     * which asserts unless the calling task's stack is internal.  DMA
     * selects HP RAM: LP (RTC) RAM is internal too, but the CPU reaches it
     * over a slow bus; with the stack there, building /webrtc/stats took
     * 134 ms instead of 7 ms. */
    config->task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    /* Access includes the public /device/setup route on both listeners. */
    config->max_uri_handlers = 30;
    /* A browser page load runs six asset connections in parallel next to
     * the signaling WebSocket; six slots made every load purge one, and the
     * long-lived WebSocket lost the LRU tie. */
    config->max_open_sockets = 8;
    config->lru_purge_enable = true;
    config->recv_wait_timeout = 5;
    config->send_wait_timeout = 5;
    config->uri_match_fn = httpd_uri_match_wildcard;
    config->open_fn = http_session_opened;
}

typedef struct { httpd_handle_t server; SemaphoreHandle_t done; bool quiesce_uploads; } cleanup_t;
static void cleanup_routes(void *argument)
{
    cleanup_t *cleanup = argument;
    if (cleanup->quiesce_uploads) {
        (void)httpd_unregister_uri_handler(cleanup->server, "/storage/upload", HTTP_POST);
        xSemaphoreGive(cleanup->done);
        return;
    }
    (void)jetkvm_virtual_media_http_unregister(cleanup->server);
    (void)p4_device_api_unregister_handlers(cleanup->server);
    if (web_app_is_ready()) (void)web_app_unregister_handlers(cleanup->server);
    xSemaphoreGive(cleanup->done);
}
static esp_err_t stop_listener(httpd_handle_t *listener, bool secure)
{
    httpd_handle_t server = *listener;
    if (!server) return ESP_OK;
    webrtc_websocket_stop(server);
    StaticSemaphore_t storage;
    cleanup_t cleanup = {.server = server, .done = xSemaphoreCreateBinaryStatic(&storage), .quiesce_uploads = true};
    /* Route tables and web-pack registration belong to the HTTPD task. */
    while (httpd_queue_work(server, cleanup_routes, &cleanup) != ESP_OK) vTaskDelay(1);
    xSemaphoreTake(cleanup.done, portMAX_DELAY);
    jetkvm_virtual_media_http_drain(server);
    /* The reader may use the local lease timer until it has exited. */
    cleanup.quiesce_uploads = false;
    while (httpd_queue_work(server, cleanup_routes, &cleanup) != ESP_OK) vTaskDelay(1);
    xSemaphoreTake(cleanup.done, portMAX_DELAY);
    vSemaphoreDelete(cleanup.done);
    esp_err_t err = secure ? httpd_ssl_stop(server) : httpd_stop(server);
    if (err != ESP_OK) return err;
    jetkvm_access_detach(server);
    webrtc_websocket_forget(server);
    *listener = NULL;
    return ESP_OK;
}
#if CONFIG_WEBRTC_TRANSPORT_HTTPS
static esp_err_t apply_tls(const jetkvm_tls_config_t *tls)
{
    if (s_secure_server) {
        esp_err_t err = stop_listener(&s_secure_server, true);
        if (err != ESP_OK) return err;
    }
    if (!strcmp(tls->mode, "disabled")) return ESP_OK;
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
    configure_http(&config.httpd);
    config.httpd.stack_size = 10240;
    config.httpd.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    config.servercert = (const uint8_t *)tls->certificate;
    config.servercert_len = strlen(tls->certificate) + 1;
    config.prvtkey_pem = (const uint8_t *)tls->private_key;
    config.prvtkey_len = strlen(tls->private_key) + 1;
    config.tls_handshake_timeout_ms = 5000;
    esp_err_t err = httpd_ssl_start(&s_secure_server, &config);
    if (err == ESP_OK) err = register_routes(s_secure_server, true);
    if (err != ESP_OK && s_secure_server) {
        esp_err_t stopped = stop_listener(&s_secure_server, true);
        if (stopped != ESP_OK) return stopped;
    }
    return err;
}
#endif

esp_err_t webrtc_transport_http_start(void)
{
    if (s_server) return ESP_OK;
    esp_err_t err = jetkvm_access_init(webrtc_transport_revoke_local);
#if CONFIG_WEBRTC_TRANSPORT_HTTPS
    if (err == ESP_OK) err = jetkvm_tls_init(apply_tls);
#endif
    if (err != ESP_OK) return err;
    if (web_app_init() != ESP_OK) ESP_LOGW(TAG, "Web pack unavailable; serving diagnostic page");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    configure_http(&config);
    config.stack_size = 8192;
    config.server_port = CONFIG_WEBRTC_TRANSPORT_HTTP_PORT;
    err = httpd_start(&s_server, &config);
    if (err == ESP_OK) err = register_routes(s_server, false);
    if (err != ESP_OK) {
        webrtc_transport_http_stop();
        return err;
    }
#if CONFIG_WEBRTC_TRANSPORT_HTTPS
    err = jetkvm_tls_start();
    if (err != ESP_OK) {
        /* Keep authenticated HTTP available to repair or disable a valid TLS
         * configuration whose listener could not start (for example, OOM). */
        ESP_LOGW(TAG, "HTTPS startup failed (%s); HTTP recovery remains available",
                 esp_err_to_name(err));
    }
#endif
    return ESP_OK;
}

void webrtc_transport_http_stop(void)
{
#if CONFIG_WEBRTC_TRANSPORT_HTTPS
    jetkvm_tls_stop();
#endif
    /* A failed control-socket send leaves HTTPD running. Retain its handle and
     * retry before freeing shared web-pack and transport state. */
    while (stop_listener(&s_secure_server, true) != ESP_OK) vTaskDelay(1);
    while (stop_listener(&s_server, false) != ESP_OK) vTaskDelay(1);
    web_app_deinit();
}

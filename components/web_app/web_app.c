/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "web_app.h"
#include "jetkvm_access.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "web_app_pack.h"

#define WEB_APP_PATH_MAX 512
#define WEB_APP_RANGE_HEADER_MAX 96

#if defined(WEB_APP_EMBEDDED_PACK)
/* Linked into the application image by target_add_binary_data(): the pack
 * rides every OTA artifact as one atomic unit with the firmware, so there is
 * no web partition to read, map, or rewrite. */
extern const uint8_t web_app_pack_start[] asm("_binary_web_app_pack_bin_start");
extern const uint8_t web_app_pack_end[] asm("_binary_web_app_pack_bin_end");
#endif

typedef struct {
    const void *metadata;
    size_t metadata_size;
    web_app_pack_view_t pack;
    httpd_handle_t servers[2];
    bool spa_fallback_registered[2];
} web_app_context_t;

static const char *TAG = "web_app";
static web_app_context_t s_web_app;

static bool path_has_dot_segment(const char *path, size_t path_length)
{
    for (size_t i = 0; i < path_length;) {
        while (i < path_length && path[i] == '/') {
            ++i;
        }
        size_t start = i;
        while (i < path_length && path[i] != '/') {
            ++i;
        }
        size_t length = i - start;
        if ((length == 1 && path[start] == '.') ||
            (length == 2 && path[start] == '.' && path[start + 1] == '.')) {
            return true;
        }
    }
    return false;
}

static bool request_accepts_html(httpd_req_t *req)
{
    size_t length = httpd_req_get_hdr_value_len(req, "Accept");
    if (length == 0 || length >= 192) {
        return false;
    }
    char accept[192];
    if (httpd_req_get_hdr_value_str(req, "Accept", accept, sizeof(accept)) !=
        ESP_OK) {
        return false;
    }
    return strstr(accept, "text/html") != NULL;
}

static esp_err_t set_asset_headers(httpd_req_t *req,
                                   const web_app_pack_entry_t *entry)
{
    const char *encoding = web_app_content_encoding(entry->encoding);
    esp_err_t err = httpd_resp_set_type(req, web_app_content_type(entry->mime));
    if (err == ESP_OK && encoding) {
        err = httpd_resp_set_hdr(req, "Content-Encoding", encoding);
    }
    if (err == ESP_OK) {
        err = httpd_resp_set_hdr(req, "Cache-Control",
                                 web_app_cache_control(entry->cache));
    }
    if (err == ESP_OK) {
        err = httpd_resp_set_hdr(
            req, "ETag", web_app_pack_entry_etag(&s_web_app.pack, entry));
    }
    if (err == ESP_OK) {
        err = httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
    }
    if (err == ESP_OK) {
        err = httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    }
    if (err == ESP_OK) {
        err = httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    }
    return err;
}

static esp_err_t send_asset_body(httpd_req_t *req, const char *body, size_t size)
{
    /* Idle asset sockets retain lwIP mailboxes and, for HTTPS, TLS state.
     * Release both before WebRTC reserves its DTLS connection memory. */
    esp_err_t err = httpd_resp_set_hdr(req, "Connection", "close");
    if (err == ESP_OK) err = httpd_resp_send(req, body, size);
    /* The response is already sent. Returning an error makes HTTPD close
     * synchronously without adding another response. A queued close could
     * run after the browser's WebSocket offer has already arrived. */
    return err == ESP_OK ? ESP_FAIL : err;
}

static esp_err_t send_asset(httpd_req_t *req,
                            const web_app_pack_entry_t *entry)
{
    esp_err_t err = set_asset_headers(req, entry);
    if (err != ESP_OK) {
        return err;
    }

    web_app_byte_range_t range = {
        .first = 0,
        .length = entry->stored_size,
    };
    size_t range_header_length = httpd_req_get_hdr_value_len(req, "Range");
    web_app_range_result_t range_result = WEB_APP_RANGE_NONE;
    if (range_header_length > 0) {
        if (range_header_length >= WEB_APP_RANGE_HEADER_MAX) {
            range_result = WEB_APP_RANGE_INVALID;
        } else {
            char range_header[WEB_APP_RANGE_HEADER_MAX];
            err = httpd_req_get_hdr_value_str(req, "Range", range_header,
                                               sizeof(range_header));
            if (err != ESP_OK) {
                return err;
            }
            range_result = web_app_parse_http_range(
                range_header, entry->stored_size, &range);
        }
    }

    char content_range[80];
    if (range_result == WEB_APP_RANGE_INVALID) {
        snprintf(content_range, sizeof(content_range), "bytes */%" PRIu32,
                 entry->stored_size);
        httpd_resp_set_status(req, "416 Range Not Satisfiable");
        httpd_resp_set_hdr(req, "Content-Range", content_range);
        return send_asset_body(req, NULL, 0);
    }
    if (range_result == WEB_APP_RANGE_VALID) {
        size_t last = range.first + range.length - 1;
        snprintf(content_range, sizeof(content_range),
                 "bytes %zu-%zu/%" PRIu32, range.first, last,
                 entry->stored_size);
        httpd_resp_set_status(req, "206 Partial Content");
        httpd_resp_set_hdr(req, "Content-Range", content_range);
    }

    if (range.length == 0) {
        return send_asset_body(req, NULL, 0);
    }
    /* The whole pack is already XIP-addressable as part of the app image;
     * serving is a plain pointer read, no flash mapping per request. */
    const uint8_t *body =
        (const uint8_t *)s_web_app.metadata + entry->data_offset + range.first;
    if ((size_t)(entry->data_offset + range.first) + range.length >
        s_web_app.metadata_size) {
        ESP_LOGE(TAG, "asset range escapes the embedded pack");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "asset mapping failed");
    }
    return send_asset_body(req, (const char *)body, range.length);
}

static esp_err_t static_handler(httpd_req_t *req)
{
    if (jetkvm_access_check_host(req) != ESP_OK) return jetkvm_access_rejection_result(req);
    if (!s_web_app.metadata) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "web app pack is not initialized");
    }
    size_t path_length = strcspn(req->uri, "?");
    if (path_length == 0 || path_length >= WEB_APP_PATH_MAX ||
        path_has_dot_segment(req->uri, path_length)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid static asset path");
    }

    const web_app_pack_entry_t *entry = web_app_pack_find(
        &s_web_app.pack, req->uri, path_length);
    if (!entry &&
        ((path_length == sizeof("/static") - 1 &&
          memcmp(req->uri, "/static", path_length) == 0) ||
         (path_length == sizeof("/static/") - 1 &&
          memcmp(req->uri, "/static/", path_length) == 0))) {
        static const char index_path[] = "/static/index.html";
        entry = web_app_pack_find(&s_web_app.pack, index_path,
                                  sizeof(index_path) - 1);
    }
    if (!entry) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                                   "static asset not found");
    }
    return send_asset(req, entry);
}

esp_err_t web_app_serve_index(httpd_req_t *req)
{
    if (!req || !s_web_app.metadata) {
        return ESP_ERR_INVALID_STATE;
    }
    static const char index_path[] = "/static/index.html";
    const web_app_pack_entry_t *entry = web_app_pack_find(
        &s_web_app.pack, index_path, sizeof(index_path) - 1);
    if (!entry) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "web app index is missing");
    }
    return send_asset(req, entry);
}

static esp_err_t spa_fallback_handler(httpd_req_t *req)
{
    if (jetkvm_access_check_host(req) != ESP_OK) return jetkvm_access_rejection_result(req);
    size_t path_length = strcspn(req->uri, "?");
    bool is_root = path_length == 1 && req->uri[0] == '/';
    if (is_root || request_accepts_html(req)) {
        return web_app_serve_index(req);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
}

esp_err_t web_app_init(void)
{
    if (s_web_app.metadata) {
        return ESP_OK;
    }
#if defined(WEB_APP_EMBEDDED_PACK)
    size_t pack_size = (size_t)(web_app_pack_end - web_app_pack_start);
    web_app_pack_view_t pack;
    web_app_pack_error_t pack_error = web_app_pack_validate(
        web_app_pack_start, pack_size, pack_size, &pack);
    if (pack_error != WEB_APP_PACK_OK) {
        ESP_LOGE(TAG, "embedded web pack is invalid: %s",
                 web_app_pack_error_name(pack_error));
        return ESP_ERR_INVALID_CRC;
    }
    s_web_app.metadata = web_app_pack_start;
    s_web_app.metadata_size = pack_size;
    s_web_app.pack = pack;
    ESP_LOGI(TAG, "ready: %" PRIu32 " assets, %" PRIu32
                  " bytes embedded in the app image",
             pack.header->entry_count, pack.header->pack_size);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "no embedded web pack in this build");
    return ESP_ERR_NOT_FOUND;
#endif
}

static int server_index(httpd_handle_t server)
{
    for (int i = 0; i < 2; ++i) if (s_web_app.servers[i] == server) return i;
    return -1;
}

esp_err_t web_app_register_handlers(httpd_handle_t server)
{
    if (!server || !s_web_app.metadata) {
        return ESP_ERR_INVALID_STATE;
    }
    if (server_index(server) >= 0) return ESP_OK;
    int slot = server_index(NULL);
    if (slot < 0) return ESP_ERR_NO_MEM;
    httpd_uri_t static_root = {
        .uri = "/static",
        .method = HTTP_GET,
        .handler = static_handler,
    };
    httpd_uri_t static_wildcard = {
        .uri = "/static/*",
        .method = HTTP_GET,
        .handler = static_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &static_root);
    if (err != ESP_OK) {
        return err;
    }
    err = httpd_register_uri_handler(server, &static_wildcard);
    if (err != ESP_OK) {
        httpd_unregister_uri_handler(server, "/static", HTTP_GET);
        return err;
    }
    s_web_app.servers[slot] = server;
    return ESP_OK;
}

esp_err_t web_app_register_spa_fallback(httpd_handle_t server)
{
    if (!server || !s_web_app.metadata) {
        return ESP_ERR_INVALID_STATE;
    }
    int slot = server_index(server);
    if (slot < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_web_app.spa_fallback_registered[slot]) {
        return ESP_OK;
    }
    httpd_uri_t fallback = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = spa_fallback_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &fallback);
    if (err == ESP_OK) {
        s_web_app.spa_fallback_registered[slot] = true;
    }
    return err;
}

esp_err_t web_app_unregister_handlers(httpd_handle_t server)
{
    if (!server) return ESP_ERR_INVALID_ARG;
    int slot = server_index(server);
    if (slot < 0) return ESP_OK;
    esp_err_t first = ESP_OK;
    if (s_web_app.spa_fallback_registered[slot]) {
        first = httpd_unregister_uri_handler(server, "/*", HTTP_GET);
        s_web_app.spa_fallback_registered[slot] = false;
    }
    esp_err_t wildcard = httpd_unregister_uri_handler(
        server, "/static/*", HTTP_GET);
    esp_err_t second = httpd_unregister_uri_handler(
        server, "/static", HTTP_GET);
    s_web_app.servers[slot] = NULL;
    return first != ESP_OK ? first : wildcard != ESP_OK ? wildcard : second;
}

void web_app_deinit(void)
{
    if (s_web_app.servers[0] || s_web_app.servers[1]) {
        ESP_LOGE(TAG, "stop/unregister the HTTP handlers before deinit");
        return;
    }
    memset(&s_web_app, 0, sizeof(s_web_app));
}

bool web_app_is_ready(void)
{
    return s_web_app.metadata != NULL;
}

esp_err_t web_app_manifest_crc32(uint32_t *out_crc32)
{
    if (!out_crc32 || !s_web_app.metadata) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_crc32 = s_web_app.pack.header->manifest_crc32;
    return ESP_OK;
}

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* The part of the ESP-IDF HTTP client that storage_download.c uses. The test
 * (test_storage_download_task.c) implements the functions as a fake server. */
typedef struct fake_http_client *esp_http_client_handle_t;

typedef enum { HTTP_EVENT_ON_HEADER = 1 } esp_http_client_event_id_t;
typedef enum { HTTP_METHOD_GET = 0 } esp_http_client_method_t;

typedef struct {
    esp_http_client_event_id_t event_id;
    char *header_key;
    char *header_value;
    void *user_data;
} esp_http_client_event_t;

typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *event);

typedef struct {
    const char *url;
    esp_http_client_method_t method;
    int timeout_ms;
    http_event_handle_cb event_handler;
    void *user_data;
    esp_err_t (*crt_bundle_attach)(void *conf);
    bool disable_auto_redirect;
    int buffer_size;
    int buffer_size_tx;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key,
                                     const char *value);
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t client, const char *key);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client);
esp_err_t esp_http_client_flush_response(esp_http_client_handle_t client, int *len);
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

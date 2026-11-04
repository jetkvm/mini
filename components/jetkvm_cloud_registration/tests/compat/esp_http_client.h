#pragma once
#include <stdbool.h>
#include "esp_err.h"
#define HTTP_EVENT_ON_DATA 1
#define HTTP_METHOD_POST 1
#define HTTP_METHOD_DELETE 2
typedef struct { int event_id; void *user_data; int data_len; void *data; } esp_http_client_event_t;
typedef struct {
    const char *url;
    int method;
    int timeout_ms;
    bool disable_auto_redirect;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    void *user_data;
    esp_err_t (*crt_bundle_attach)(void *);
    bool skip_cert_common_name_check;
    const char *common_name;
} esp_http_client_config_t;
typedef void *esp_http_client_handle_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int len);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

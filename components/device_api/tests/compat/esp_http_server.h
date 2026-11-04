#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef void *httpd_handle_t;

typedef enum {
    HTTP_GET = 0,
    HTTP_POST = 1,
} httpd_method_t;

typedef struct httpd_req {
    httpd_handle_t handle;
    int method;
    size_t content_len;
    void *user_ctx;
    void *aux;
} httpd_req_t;

typedef struct httpd_uri {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *request);
    void *user_ctx;
} httpd_uri_t;

enum {
    HTTPD_500_INTERNAL_SERVER_ERROR = 500,
};

esp_err_t httpd_register_uri_handler(httpd_handle_t server,
                                     const httpd_uri_t *uri);
esp_err_t httpd_unregister_uri_handler(httpd_handle_t server,
                                       const char *uri,
                                       httpd_method_t method);
int httpd_req_recv(httpd_req_t *request, char *buffer, size_t length);
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *name,
                             const char *value);
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status);
esp_err_t httpd_resp_sendstr(httpd_req_t *request, const char *body);
esp_err_t httpd_resp_send_err(httpd_req_t *request, int code,
                              const char *message);

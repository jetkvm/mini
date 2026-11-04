#pragma once

#include "esp_http_server.h"

/* Register before the SPA fallback. Stop the pump before stopping HTTPD;
 * HTTPD then frees each socket's context and closes its owned peer session. */
esp_err_t webrtc_websocket_start(httpd_handle_t server);
void webrtc_websocket_stop(httpd_handle_t server);

void webrtc_websocket_forget(httpd_handle_t server);

/* SPDX-License-Identifier: MIT */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t jetkvm_virtual_media_http_register(httpd_handle_t server);
esp_err_t jetkvm_virtual_media_http_unregister(httpd_handle_t server);

void jetkvm_virtual_media_http_revoke_local(void);

void jetkvm_virtual_media_http_drain(httpd_handle_t server);

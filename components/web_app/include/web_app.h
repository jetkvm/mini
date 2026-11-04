/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Validate the web asset pack embedded in the application image.
 *
 * The pack is linked into the firmware binary (target_add_binary_data), so
 * app and UI travel as one atomic unit through every OTA update and
 * rollback.  It stays XIP-addressable for the process lifetime; asset
 * bodies are served as plain pointer reads with no flash mapping.
 */
esp_err_t web_app_init(void);

/**
 * Register GET handlers for the exact and wildcard static paths.
 *
 * The HTTP server must have been started with
 * config.uri_match_fn = httpd_uri_match_wildcard. Missing static assets always
 * remain 404 responses rather than receiving the SPA document. Asset responses
 * close their connections to preserve memory for WebRTC on both HTTP and HTTPS.
 */
esp_err_t web_app_register_handlers(httpd_handle_t server);

/** Serve /static/index.html for a caller-owned route such as GET /. */
esp_err_t web_app_serve_index(httpd_req_t *req);

/**
 * Register the final catch-all GET handler used for BrowserRouter navigation.
 *
 * Register all explicit WebRTC, device API, and other handlers first.
 * The fallback serves index.html only for GET / or requests accepting
 * text/html; other unknown requests receive 404.
 */
esp_err_t web_app_register_spa_fallback(httpd_handle_t server);

/** Unregister handlers previously added by web_app_register_handlers(). */
esp_err_t web_app_unregister_handlers(httpd_handle_t server);

/** Clear the serving state. Call after stopping the server. */
void web_app_deinit(void);

bool web_app_is_ready(void);

/**
 * Report the embedded pack's manifest CRC32.
 *
 * The CRC covers the sorted path table and every per-asset SHA-256 ETag, so
 * it uniquely identifies the exact pack content.  OTA reports it as the web
 * ("system") component version, and the release server derives the same
 * value from the candidate image's pack header.
 */
esp_err_t web_app_manifest_crc32(uint32_t *out_crc32);

#ifdef __cplusplus
}
#endif

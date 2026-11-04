/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_cloud_registration.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "jetkvm_memory_policy.h"

/* The RPC runs on the WebRTC peer task; the timeout bounds the stall. */
#define PUBLIC_IP_TIMEOUT_MS 4000

/* The RPC task is the only caller, so the cache needs no lock. */
static char s_ip[46];
static int64_t s_seen_at;
static char s_error[96];

static const char *lookup_public_ip(const char *cloud_url)
{
    /* As Go: the cloud URL's scheme and host, without its path. */
    const char *host = cloud_url ? strstr(cloud_url, "://") : NULL;
    if (host == NULL) {
        return "invalid cloud URL";
    }
    int base = (int)(host + 3 - cloud_url) + (int)strcspn(host + 3, "/");
    char url[192];
    if (snprintf(url, sizeof(url), "%.*s/cdn-cgi/trace", base, cloud_url) >=
        (int)sizeof(url)) {
        return "cloud URL too long";
    }
    jetkvm_memory_policy_reservation_t tls = {0};
    if (!jetkvm_memory_policy_reserve_tls("public-ip", &tls)) {
        return "insufficient memory for TLS";
    }
    const esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = PUBLIC_IP_TIMEOUT_MS,
        .disable_auto_redirect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    char body[1024];
    int status = 0;
    int length = -1;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = client ? esp_http_client_open(client, 0) : ESP_ERR_NO_MEM;
    if (err == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        status = esp_http_client_get_status_code(client);
        length = esp_http_client_read_response(client, body, sizeof(body) - 1);
    }
    esp_http_client_cleanup(client);
    jetkvm_memory_policy_release(&tls);
    if (err != ESP_OK || length < 0) {
        snprintf(s_error, sizeof(s_error), "error sending request: %s",
                 esp_err_to_name(err != ESP_OK ? err : ESP_FAIL));
        return s_error;
    }
    if (status != 200) {
        snprintf(s_error, sizeof(s_error), "unexpected status code: %d", status);
        return s_error;
    }
    body[length] = '\0';
    char ip[sizeof(s_ip)];
    int64_t seen_at = 0;
    if (!jetkvm_cloud_public_ip_parse(body, ip, sizeof(ip), &seen_at)) {
        return "no IP address found";
    }
    memcpy(s_ip, ip, sizeof(s_ip));
    s_seen_at = seen_at;
    return NULL;
}

const char *jetkvm_cloud_public_ip_get(bool refresh, const char *cloud_url,
                                       char *ip, size_t ip_capacity,
                                       int64_t *last_updated, void *ctx)
{
    (void)ctx;
    const char *error = refresh ? lookup_public_ip(cloud_url) : NULL;
    if (error == NULL) {
        snprintf(ip, ip_capacity, "%s", s_ip);
        *last_updated = s_seen_at;
    }
    return error;
}

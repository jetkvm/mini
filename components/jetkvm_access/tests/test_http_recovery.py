#!/usr/bin/env python3
"""Keep authenticated HTTP available when optional HTTPS startup fails."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = (root / "components/webrtc_transport/webrtc_http.c").read_text()
start = source[source.index("esp_err_t webrtc_transport_http_start(void)"):
               source.index("void webrtc_transport_http_stop(void)")]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef int esp_err_t;
typedef struct { unsigned stack_size, server_port; } httpd_config_t;
#define ESP_OK 0
#define CONFIG_WEBRTC_TRANSPORT_HTTPS 1
#define ESP_LOGW(...) ((void)0)
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
#define CONFIG_WEBRTC_TRANSPORT_HTTP_PORT 80
static void *s_server;
static int access_error, init_error, http_error, routes_error, tls_error, pack_error;
static unsigned tls_calls, stops;
static bool authenticated_routes;
static void webrtc_transport_revoke_local(void) {}
static esp_err_t apply_tls(const void *config) { (void)config; return ESP_OK; }
static int jetkvm_access_init(void (*revoke)(void)) {
    assert(revoke == webrtc_transport_revoke_local); return access_error;
}
static int jetkvm_tls_init(int (*apply)(const void *)) {
    assert(apply == apply_tls); return init_error;
}
static int web_app_init(void) { return pack_error; }
static void configure_http(httpd_config_t *config) { (void)config; }
static int httpd_start(void **server, const httpd_config_t *config) {
    assert(!access_error && !init_error && config->server_port == 80);
    if (!http_error) *server = (void *)1;
    return http_error;
}
static int register_routes(void *server, bool secure) {
    assert(server == s_server && !secure);
    if (!routes_error) authenticated_routes = true;
    return routes_error;
}
static int jetkvm_tls_start(void) {
    assert(s_server && authenticated_routes); ++tls_calls; return tls_error;
}
static void webrtc_transport_http_stop(void) {
    s_server = NULL; authenticated_routes = false; ++stops;
}
START
static void reset(void) {
    s_server = NULL; authenticated_routes = false;
    access_error = init_error = http_error = routes_error = tls_error = pack_error = 0;
    tls_calls = stops = 0;
}
int main(void) {
    reset(); assert(webrtc_transport_http_start() == ESP_OK);
    assert(s_server && authenticated_routes && tls_calls == 1 && !stops);
    assert(webrtc_transport_http_start() == ESP_OK && tls_calls == 1);
    /* Simulate HTTPS allocation and route-registration failures separately. */
    for (int failure = 1; failure <= 2; ++failure) {
        reset(); tls_error = failure;
        assert(webrtc_transport_http_start() == ESP_OK);
        assert(s_server && authenticated_routes && tls_calls == 1 && !stops);
    }
    reset(); access_error = 3; assert(webrtc_transport_http_start() == 3);
    assert(!s_server && !authenticated_routes && !tls_calls);
    reset(); init_error = 4; assert(webrtc_transport_http_start() == 4);
    assert(!s_server && !authenticated_routes && !tls_calls);
    reset(); http_error = 5; assert(webrtc_transport_http_start() == 5);
    assert(!s_server && !authenticated_routes && !tls_calls && stops == 1);
    reset(); routes_error = 6; assert(webrtc_transport_http_start() == 6);
    assert(!s_server && !authenticated_routes && !tls_calls && stops == 1);
    reset(); pack_error = 7; assert(webrtc_transport_http_start() == ESP_OK);
    assert(s_server && authenticated_routes && tls_calls == 1 && !stops);
    puts("HTTP recovery after HTTPS startup failure and required-service failures: OK");
}
'''.replace("START", start)
with tempfile.TemporaryDirectory() as directory:
    c = Path(directory) / "recovery.c"
    binary = Path(directory) / "recovery"
    c.write_text(harness)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(c), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

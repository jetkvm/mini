#!/usr/bin/env python3
"""Run the actual HTTP stats handler with real cJSON and tracked ownership,
and check that it builds the stats below the OS Services task's priority."""
from pathlib import Path
import subprocess
import tempfile

component = Path(__file__).resolve().parents[1]
repo = component.parents[1]
source = (component / 'webrtc_http.c').read_text()
helpers = source[source.index('static void set_common_headers('):
                 source.index('\nstatic esp_err_t send_status_text(')]
# Everything between the signaling handler and register_uri: the stats
# handler and what it calls.
signal_end = 'return httpd_resp_sendstr(req, "{\\"ok\\":true}");\n}\n'
handler = source[source.index(signal_end) + len(signal_end):
                 source.index('\nstatic esp_err_t register_uri(')]

stubs = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
typedef struct { const char *query; } httpd_req_t;
typedef unsigned UBaseType_t;
/* The HTTP server's task priority, and the OS Services task's. */
#define SERVER_PRIORITY 5U
#define JETKVM_HTTP_SERVER_TASK_PRIORITY SERVER_PRIORITY
#define OS_SERVICES_PRIORITY 4U
static UBaseType_t task_priority = SERVER_PRIORITY;
void vTaskPrioritySet(void *task, UBaseType_t priority) { assert(!task); task_priority = priority; }
static void *allocations[256];
static size_t live_count, tree_count;
static unsigned send_calls, error_calls, header_calls;
static bool fail_allocations, borrowed_tree, detailed_called, bitrate_called;
static esp_err_t send_result, create_result;
static cJSON *tree;
static const char *expected_error;
static const char expected_body[] =
    "{\"modeValid\":true,\"sequence\":7,\"nested\":{\"frame\":30}}";

static void *tracked_malloc(size_t size)
{
    if (fail_allocations) return NULL;
    void *ptr = malloc(size); assert(ptr);
    for (size_t i = 0; i < 256; ++i) {
        if (!allocations[i]) { allocations[i] = ptr; ++live_count; return ptr; }
    }
    assert(!"allocation tracker exhausted"); return NULL;
}
static void tracked_free(void *ptr)
{
    if (!ptr) return;
    for (size_t i = 0; i < 256; ++i) {
        if (allocations[i] == ptr) {
            allocations[i] = NULL; --live_count; free(ptr); return;
        }
    }
    assert(!"unknown pointer or double free");
}
static void assert_tree_ownership(bool has_string)
{
    /* No request bytes may become visible while stats still owns its AST. */
    assert(live_count == (borrowed_tree ? tree_count : 0) + (has_string ? 1 : 0));
}
static esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *key, const char *value)
{
    (void)req; assert_tree_ownership(true); ++header_calls;
    assert((strcmp(key, "Cache-Control") == 0 && strcmp(value, "no-store, max-age=0") == 0) ||
           (strcmp(key, "X-Content-Type-Options") == 0 && strcmp(value, "nosniff") == 0));
    return ESP_OK;
}
static esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *value)
{
    (void)req; assert_tree_ownership(true); assert(strcmp(value, "application/json") == 0);
    return ESP_OK;
}
static esp_err_t httpd_resp_sendstr(httpd_req_t *req, const char *json)
{
    (void)req; assert_tree_ownership(true); ++send_calls;
    assert(strcmp(json, expected_body) == 0);
    assert(header_calls == 2);
    return send_result;
}
static esp_err_t httpd_resp_send_err(httpd_req_t *req, int status, const char *message)
{
    (void)req; assert_tree_ownership(false); ++error_calls;
    assert(status == HTTPD_500_INTERNAL_SERVER_ERROR);
    assert(expected_error && strcmp(message, expected_error) == 0);
    return ESP_FAIL;
}
static esp_err_t httpd_req_get_url_query_str(httpd_req_t *req, char *out, size_t size)
{
    if (!req->query) return ESP_FAIL;
    assert(strlen(req->query) + 1 <= size); strcpy(out, req->query); return ESP_OK;
}
static esp_err_t httpd_query_key_value(const char *query, const char *key, char *out, size_t size)
{
    const size_t key_len = strlen(key);
    for (const char *at = query; at; at = strchr(at, '&') ? strchr(at, '&') + 1 : NULL) {
        if (strncmp(at, key, key_len) != 0 || at[key_len] != '=') continue;
        const size_t len = strcspn(at + key_len + 1, "&");
        assert(len + 1 <= size); memcpy(out, at + key_len + 1, len); out[len] = 0; return ESP_OK;
    }
    return ESP_FAIL;
}
static unsigned interval_resets;
static void webrtc_transport_reset_video_intervals(void) { ++interval_resets; }
static esp_err_t make_stats(cJSON **out, bool detailed)
{
    /* Building the stats must not keep the OS Services task off its core. */
    assert(task_priority < OS_SERVICES_PRIORITY);
    detailed_called = detailed;
    if (create_result != ESP_OK) return create_result;
    bool fail_render = fail_allocations;
    fail_allocations = false;
    tree = cJSON_Parse(expected_body); assert(tree);
    tree_count = live_count;
    fail_allocations = fail_render;
    *out = tree; return ESP_OK;
}
static esp_err_t webrtc_stats_json_create(cJSON **out) { return make_stats(out, false); }
static esp_err_t webrtc_stats_json_create_detailed(cJSON **out) { return make_stats(out, true); }
static esp_err_t webrtc_stats_json_create_bitrate(cJSON **out) { bitrate_called = true; return make_stats(out, false); }
static void reset(void)
{
    assert(live_count == 0);
    /* Every request, failed or not, gives the server its priority back. */
    assert(task_priority == SERVER_PRIORITY);
    tree_count = 0; send_calls = error_calls = header_calls = interval_resets = 0;
    fail_allocations = borrowed_tree = detailed_called = bitrate_called = false;
    send_result = create_result = ESP_OK; tree = NULL; expected_error = NULL;
}
'''
checks = r'''
int main(void)
{
    cJSON_Hooks hooks = { .malloc_fn = tracked_malloc, .free_fn = tracked_free };
    cJSON_InitHooks(&hooks);
    httpd_req_t req = {0};

    reset(); assert(stats_handler(&req) == ESP_OK);
    assert(task_priority == SERVER_PRIORITY);

    /* A priority a mutex waiter lent the task is not kept as its own. */
    reset(); task_priority = 6U; assert(stats_handler(&req) == ESP_OK);
    assert(task_priority == SERVER_PRIORITY);
    assert(send_calls == 1 && !error_calls && !detailed_called && !bitrate_called && !live_count);

    reset(); req.query = "details=1";
    assert(stats_handler(&req) == ESP_OK);
    assert(detailed_called && send_calls == 1 && !live_count);

    reset(); req.query = "bitrate=1";
    assert(stats_handler(&req) == ESP_OK);
    assert(bitrate_called && !detailed_called && send_calls == 1 && !live_count);
    assert(!interval_resets);

    reset(); req.query = "bitrate=1&resetVideoIntervals=1";
    assert(stats_handler(&req) == ESP_OK);
    assert(bitrate_called && interval_resets == 1 && send_calls == 1 && !live_count);

    reset(); req.query = "details=1&resetVideoIntervals=1";
    assert(stats_handler(&req) == ESP_OK);
    assert(detailed_called && !interval_resets);

    reset(); req.query = "details=0"; send_result = ESP_FAIL;
    assert(stats_handler(&req) == ESP_FAIL);
    assert(!detailed_called && send_calls == 1 && !error_calls && !live_count);

    reset(); fail_allocations = true; expected_error = "JSON allocation failed";
    assert(stats_handler(&req) == ESP_ERR_NO_MEM);
    assert(!send_calls && error_calls == 1 && !live_count && !header_calls);

    reset(); create_result = ESP_ERR_NO_MEM; expected_error = "no memory";
    assert(stats_handler(&req) == ESP_FAIL);
    assert(!send_calls && error_calls == 1 && !live_count);

    reset(); create_result = ESP_FAIL; expected_error = "stats unavailable";
    assert(stats_handler(&req) == ESP_FAIL);
    assert(!send_calls && error_calls == 1 && !live_count);

    /* The shared helper still borrows other handlers' trees, both on success
     * and send/allocation failure; only the serialized string is consumed. */
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        reset(); task_priority = 0U;
        assert(make_stats(&tree, false) == ESP_OK); borrowed_tree = true;
        task_priority = SERVER_PRIORITY;
        send_result = scenario == 1 ? ESP_FAIL : ESP_OK;
        fail_allocations = scenario == 2;
        expected_error = "JSON allocation failed";
        assert(send_json(&req, tree) == (scenario == 2 ? ESP_ERR_NO_MEM : send_result));
        assert(live_count == tree_count && cJSON_IsObject(tree));
        assert(send_calls == (scenario == 2 ? 0U : 1U));
        assert(error_calls == (scenario == 2 ? 1U : 0U));
        cJSON_Delete(tree); assert(!live_count);
    }
    puts("stats response AST/string lifetime host tests: OK");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='webrtc-stats-response-') as directory:
    temp = Path(directory)
    (temp / 'test.c').write_text(stubs + helpers + handler + checks)
    cjson = repo / 'managed_components/espressif__cjson/cJSON'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-I', str(cjson), str(temp / 'test.c'), str(cjson / 'cJSON.c'),
                    '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test')], check=True)

/* Update-queue tests with the real JSON-RPC dispatcher, linked into the media
 * test. An update check opens a TLS connection to the update server and waits
 * for its answer, so it runs on its own worker and never on the peer task,
 * which also sends the video. The other update methods return at once and
 * stay on the peer task. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "jetkvm_control.h"
#include "jetkvm_memory_policy.h"
#include "rpc_worker.h"

void run_worker(const char *name); /* test_media.c */

static const rpc_worker_origin_t origin = {
    .session_id = 7, .generation = 9, .stream_id = 11, .access_epoch = 5,
};

/* The update service. A check call stands for the request to the server. */
static unsigned check_calls;
static char checked_system[JETKVM_CONTROL_OTA_VERSION_CAPACITY];
/* The server's URLs at their full length, each octet one that JSON escapes
 * as \u00XX: six times as long once rendered. */
static bool escaped_urls;

extern size_t control_arena_blocks, control_arena_fail_size; /* control_arena.c */

static void remote_status(jetkvm_control_ota_status_t *status)
{
    memset(status, 0, sizeof(*status));
    if (escaped_urls) {
        memset(status->remote_app_url, 0x01, sizeof(status->remote_app_url) - 1U);
        memset(status->remote_system_url, 0x01, sizeof(status->remote_system_url) - 1U);
    }
    snprintf(status->local_system_version, sizeof(status->local_system_version), "0.5.13");
    snprintf(status->remote_system_version, sizeof(status->remote_system_version), "0.5.14");
    status->system_update_available = true;
}

static bool service_status(jetkvm_control_ota_status_t *status, void *ctx)
{
    (void)ctx;
    ++check_calls;
    remote_status(status);
    return true;
}

static bool service_check(const char *app_constraint, const char *system_constraint,
                          bool include_prerelease, jetkvm_control_ota_status_t *status,
                          void *ctx)
{
    (void)app_constraint; (void)include_prerelease; (void)ctx;
    ++check_calls;
    snprintf(checked_system, sizeof(checked_system), "%s", system_constraint);
    remote_status(status);
    return true;
}

static bool service_dev_channel(void *ctx)
{
    (void)ctx;
    return true;
}

static const jetkvm_ota_context_t service = {
    .get_update_status = service_status,
    .check_update = service_check,
    .get_dev_channel = service_dev_channel,
};

static const char status_request[] =
    "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"getUpdateStatus\"}";
static const char second_status[] =
    "{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"getUpdateStatus\"}";
static const char components_request[] =
    "{\"jsonrpc\":\"2.0\",\"id\":33,\"method\":\"checkUpdateComponents\","
    "\"params\":{\"params\":{\"components\":{\"system\":\"0.5.14\"}},"
    "\"includePreRelease\":false}}";
static const char second_components[] =
    "{\"jsonrpc\":\"2.0\",\"id\":34,\"method\":\"checkUpdateComponents\","
    "\"params\":{\"params\":{\"components\":{\"system\":\"0.5.14\"}},"
    "\"includePreRelease\":false}}";
static const char dev_channel_request[] =
    "{\"jsonrpc\":\"2.0\",\"id\":35,\"method\":\"getDevChannelState\"}";
static const char emulation[] =
    "{\"jsonrpc\":\"2.0\",\"id\":36,\"method\":\"setUsbEmulationState\",\"params\":{\"enabled\":false}}";

static bool defer(const char *request)
{
    return rpc_worker_defer((const uint8_t *)request, strlen(request), &origin);
}

/* The reply with no worker: the update service called by the dispatcher. */
static void direct_reply(const char *request, char *json, size_t capacity)
{
    const jetkvm_control_context_t context = {0};
    size_t length = 0;
    assert(jetkvm_control_handle_rpc_with_context(&context, (const uint8_t *)request,
        strlen(request), json, capacity, &length) == JETKVM_CONTROL_RESPONSE_READY);
    json[length] = '\0';
}

/* What the peer task sends when rpc_worker_defer() refuses a request. */
static void peer_reply(const char *request, char *json, size_t capacity)
{
    jetkvm_control_context_t context = {0};
    rpc_worker_peer_fallbacks(&context);
    size_t length = 0;
    assert(jetkvm_control_handle_rpc_with_context(&context, (const uint8_t *)request,
        strlen(request), json, capacity, &length) == JETKVM_CONTROL_RESPONSE_READY);
    json[length] = '\0';
}

static void expect_worker_reply(const char *request)
{
    char expected[1024];
    rpc_worker_reply_t reply;
    check_calls = 0;
    assert(defer(request));
    assert(check_calls == 0 && !rpc_worker_take_reply(&reply));
    run_worker("jk_update_rpc");
    assert(check_calls == 1);
    assert(rpc_worker_take_reply(&reply));
    assert(reply.session_id == 7 && reply.generation == 9 && reply.stream_id == 11);
    direct_reply(request, expected, sizeof(expected));
    assert(check_calls == 2);
    assert(reply.length == strlen(expected) && !memcmp(reply.json, expected, reply.length));
    assert(strstr(expected, "\"systemUpdateAvailable\":true"));
}

static void test_runs_on_worker(void)
{
    expect_worker_reply(status_request);
    checked_system[0] = '\0';
    expect_worker_reply(components_request);
    assert(!strcmp(checked_system, "0.5.14"));
    puts("getUpdateStatus and checkUpdateComponents run on the update worker, same JSON as inline: OK");
}

static void test_busy_queue_does_not_block_peer(void)
{
    char json[512], expected[512];
    rpc_worker_reply_t reply;
    check_calls = 0;
    assert(defer(status_request));
    /* A second check while the first waits for the server: the peer task
     * answers at once and does not make a second request. */
    assert(!defer(second_status));
    peer_reply(second_status, json, sizeof(json));
    assert(!defer(second_components));
    peer_reply(second_components, expected, sizeof(expected));
    assert(check_calls == 0);
    assert(!strcmp(json, "{\"jsonrpc\":\"2.0\",\"id\":32,\"error\":{\"code\":-32603,"
                         "\"message\":\"Internal error\",\"data\":"
                         "\"error checking for updates: another update check is active\"}}"));
    assert(strstr(expected, "\"failed to check update: another update check is active\""));
    /* The peer task still reads the dev channel from the service. */
    assert(!defer(dev_channel_request));
    peer_reply(dev_channel_request, json, sizeof(json));
    direct_reply(dev_channel_request, expected, sizeof(expected));
    assert(!strcmp(json, expected) && strstr(json, "\"result\":true"));
    /* The control queue is free: a USB emulation change does not wait for
     * the update server either. */
    assert(defer(emulation));
    run_worker("jk_control");
    assert(rpc_worker_take_reply(&reply) && strstr(reply.json, "\"id\":36"));
    assert(!rpc_worker_take_reply(&reply));
    run_worker("jk_update_rpc");
    assert(check_calls == 1);
    assert(rpc_worker_take_reply(&reply) && strstr(reply.json, "\"id\":31"));
    puts("a busy update queue answers on the peer without a request: OK");
}

static void test_long_reply(void)
{
    static char expected[8192];
    rpc_worker_reply_t reply;
    escaped_urls = true;
    check_calls = 0;
    assert(defer(status_request));
    run_worker("jk_update_rpc");
    assert(rpc_worker_take_reply(&reply) && reply.large);
    direct_reply(status_request, expected, sizeof(expected));
    assert(check_calls == 2 && strlen(expected) > sizeof(reply.json));
    assert(reply.length == strlen(expected) && !memcmp(reply.large, expected, reply.length));
    rpc_worker_release_reply(&reply);
    escaped_urls = false;
    assert(control_arena_blocks == 0);
    puts("an escaped status longer than the inline reply is sent whole: OK");
}

static void test_no_memory_for_reply(void)
{
    rpc_worker_reply_t reply;
    check_calls = 0;
    control_arena_fail_size = JETKVM_MEMORY_RPC_SCRATCH_BYTES;
    assert(defer(status_request));
    run_worker("jk_update_rpc");
    control_arena_fail_size = 0;
    assert(check_calls == 0);
    assert(rpc_worker_take_reply(&reply) && !reply.large);
    assert(strstr(reply.json, "\"error checking for updates: out of memory\""));
    assert(control_arena_blocks == 0);
    puts("without a reply buffer the update worker answers without a request: OK");
}

void test_update_queue(void);
void test_update_queue(void)
{
    jetkvm_control_set_default_ota_context(&service);
    test_runs_on_worker();
    test_busy_queue_does_not_block_peer();
    test_long_reply();
    test_no_memory_for_reply();
    jetkvm_control_set_default_ota_context(NULL);
}

/* MQTT-queue tests with the real JSON-RPC dispatcher, linked into the media
 * test. testMqttConnection waits for a broker for up to five seconds, so it
 * runs on its own worker and never on the peer task, which also sends the
 * video. The other MQTT methods return at once and stay on the peer task. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "jetkvm_control.h"
#include "rpc_worker.h"

void run_worker(const char *name); /* test_media.c */

static const rpc_worker_origin_t origin = {
    .session_id = 7, .generation = 9, .stream_id = 11, .access_epoch = 5,
};

/* The MQTT service. A test call stands for the wait for the broker. */
static unsigned test_calls;
static char tested_broker[JETKVM_CONTROL_MQTT_BROKER_CAPACITY];

static bool service_test(const jetkvm_control_mqtt_settings_t *settings,
                         jetkvm_control_mqtt_test_result_t *result, void *ctx)
{
    (void)ctx;
    ++test_calls;
    snprintf(tested_broker, sizeof(tested_broker), "%s", settings->broker);
    memset(result, 0, sizeof(*result));
    snprintf(result->error, sizeof(result->error), "bad user name or password");
    return true;
}

static bool service_settings(jetkvm_control_mqtt_settings_t *settings, void *ctx)
{
    (void)ctx;
    memset(settings, 0, sizeof(*settings));
    snprintf(settings->broker, sizeof(settings->broker), "broker.example");
    settings->port = 1883;
    snprintf(settings->base_topic, sizeof(settings->base_topic), "jetkvm");
    return true;
}

static const jetkvm_mqtt_context_t service = {
    .get_settings = service_settings,
    .test_connection = service_test,
};

#define SETTINGS                                                              \
    "{\"enabled\":false,\"broker\":\"192.0.2.7\",\"port\":1883,"             \
    "\"username\":\"wrong\",\"password\":\"secret\",\"base_topic\":\"jetkvm\"," \
    "\"use_tls\":false,\"tls_insecure\":false,"                              \
    "\"enable_ha_discovery\":true,\"enable_actions\":true,\"debounce_ms\":0}"
static const char test_request[] =
    "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"testMqttConnection\","
    "\"params\":{\"settings\":" SETTINGS "}}";
static const char second_test[] =
    "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"testMqttConnection\","
    "\"params\":{\"settings\":" SETTINGS "}}";
static const char settings_request[] =
    "{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"getMqttSettings\"}";
static const char emulation[] =
    "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"setUsbEmulationState\",\"params\":{\"enabled\":false}}";

static bool defer(const char *request)
{
    return rpc_worker_defer((const uint8_t *)request, strlen(request), &origin);
}

/* The reply with no worker: the MQTT service called by the dispatcher. */
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

static void test_runs_on_worker(void)
{
    char expected[512];
    rpc_worker_reply_t reply;
    test_calls = 0;
    assert(defer(test_request));
    assert(test_calls == 0 && !rpc_worker_take_reply(&reply));
    run_worker("jk_mqtt_rpc");
    assert(test_calls == 1 && !strcmp(tested_broker, "192.0.2.7"));
    assert(rpc_worker_take_reply(&reply));
    assert(reply.session_id == 7 && reply.generation == 9 && reply.stream_id == 11);
    direct_reply(test_request, expected, sizeof(expected));
    assert(test_calls == 2);
    assert(reply.length == strlen(expected) && !memcmp(reply.json, expected, reply.length));
    assert(strstr(expected, "\"bad user name or password\""));
    puts("testMqttConnection runs on the MQTT worker, same JSON as inline: OK");
}

static void test_busy_queue_does_not_block_peer(void)
{
    char json[512], expected[512];
    rpc_worker_reply_t reply;
    test_calls = 0;
    assert(defer(test_request));
    /* A second test while the first waits for its broker: the peer task
     * answers at once and does not start a second wait. */
    assert(!defer(second_test));
    peer_reply(second_test, json, sizeof(json));
    assert(test_calls == 0);
    assert(!strcmp(json, "{\"jsonrpc\":\"2.0\",\"id\":22,\"result\":{\"success\":false,"
                         "\"error\":\"another MQTT connection test is active\"}}"));
    /* The peer task still reads the MQTT settings from the service. */
    assert(!defer(settings_request));
    peer_reply(settings_request, json, sizeof(json));
    direct_reply(settings_request, expected, sizeof(expected));
    assert(!strcmp(json, expected) && strstr(json, "\"broker.example\""));
    /* The control queue is free: a USB emulation change does not wait for
     * the broker either. */
    assert(defer(emulation));
    run_worker("jk_control");
    assert(rpc_worker_take_reply(&reply) && strstr(reply.json, "\"id\":24"));
    assert(!rpc_worker_take_reply(&reply));
    run_worker("jk_mqtt_rpc");
    assert(test_calls == 1);
    assert(rpc_worker_take_reply(&reply) && strstr(reply.json, "\"id\":21"));
    puts("a busy MQTT queue answers on the peer without waiting: OK");
}

void test_mqtt_queue(void);
void test_mqtt_queue(void)
{
    jetkvm_control_set_default_mqtt_context(&service);
    test_runs_on_worker();
    test_busy_queue_does_not_block_peer();
    jetkvm_control_set_default_mqtt_context(NULL);
}

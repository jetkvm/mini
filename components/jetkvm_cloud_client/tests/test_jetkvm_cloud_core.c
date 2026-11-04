#include "jetkvm_cloud_core.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_url_mapping(void)
{
    char output[JETKVM_CLOUD_CORE_MAX_URL_SIZE + 1U];
    bool tls = false;
    assert(jetkvm_cloud_core_make_ws_url(
               "https://api.jetkvm.com", output, sizeof(output), &tls) ==
           JETKVM_CLOUD_CORE_OK);
    assert(tls);
    assert(strcmp(output, "wss://api.jetkvm.com") == 0);

    assert(jetkvm_cloud_core_make_ws_url(
               "http://127.0.0.1:8080/custom?test=1", output,
               sizeof(output), &tls) == JETKVM_CLOUD_CORE_OK);
    assert(!tls);
    assert(strcmp(output, "ws://127.0.0.1:8080/custom?test=1") == 0);

    char tiny[8] = "guard";
    assert(jetkvm_cloud_core_make_ws_url(
               "https://api.jetkvm.com", tiny, sizeof(tiny), &tls) ==
           JETKVM_CLOUD_CORE_OUTPUT_TOO_SMALL);
    assert(jetkvm_cloud_core_make_ws_url(
               "wss://api.jetkvm.com", output, sizeof(output), &tls) ==
           JETKVM_CLOUD_CORE_INVALID_URL);
    assert(jetkvm_cloud_core_make_ws_url(
               "https://user@api.jetkvm.com", output, sizeof(output), &tls) ==
           JETKVM_CLOUD_CORE_INVALID_URL);
    assert(jetkvm_cloud_core_make_ws_url(
               "https://api.jetkvm.com/#fragment", output,
               sizeof(output), &tls) == JETKVM_CLOUD_CORE_INVALID_URL);
    assert(jetkvm_cloud_core_make_ws_url(
               "https://api.jetkvm.com\r\nInjected: yes", output,
               sizeof(output), &tls) == JETKVM_CLOUD_CORE_INVALID_URL);
}

static void test_headers(void)
{
    char output[JETKVM_CLOUD_CORE_MAX_HEADER_SIZE];
    size_t output_length = 0;
    assert(jetkvm_cloud_core_make_headers(
               "p4-001122334455", "0.5.8-dev202609282126",
               "jetkvm-mini-wireless", "0123456789abcdef", output,
               sizeof(output), &output_length) == JETKVM_CLOUD_CORE_OK);
    const char *expected =
        "X-Device-ID: p4-001122334455\r\n"
        "X-App-Version: 0.5.8-dev202609282126-mini\r\n"
        "X-Device-SKU: jetkvm-mini-wireless\r\n"
        "Authorization: Bearer 0123456789abcdef\r\n";
    assert(output_length == strlen(expected));
    assert(strcmp(output, expected) == 0);

    /* Every value at its limit, with the version suffix, still fits. */
    char device_id[JETKVM_CLOUD_CORE_MAX_DEVICE_ID_SIZE + 1U];
    char version[JETKVM_CLOUD_CORE_MAX_APP_VERSION_SIZE + 1U];
    char sku[JETKVM_CLOUD_CORE_MAX_SKU_SIZE + 1U];
    char token[JETKVM_CLOUD_CORE_MAX_TOKEN_SIZE + 1U];
    memset(device_id, 'd', sizeof(device_id) - 1U);
    device_id[sizeof(device_id) - 1U] = '\0';
    memset(version, 'v', sizeof(version) - 1U);
    version[sizeof(version) - 1U] = '\0';
    memset(sku, 's', sizeof(sku) - 1U);
    sku[sizeof(sku) - 1U] = '\0';
    memset(token, 't', sizeof(token) - 1U);
    token[sizeof(token) - 1U] = '\0';
    assert(jetkvm_cloud_core_make_headers(
               device_id, version, sku, token, output, sizeof(output),
               &output_length) == JETKVM_CLOUD_CORE_OK);
    assert(strstr(output, JETKVM_CLOUD_CORE_APP_VERSION_SUFFIX "\r\n"
                          "X-Device-SKU: ") != NULL);

    assert(jetkvm_cloud_core_make_headers(
               "device\r\nInjected: yes", "version", "jetkvm-mini-wireless",
               "token", output, sizeof(output), &output_length) ==
           JETKVM_CLOUD_CORE_INVALID_HEADER_VALUE);
    assert(output_length == 0);
    assert(jetkvm_cloud_core_make_headers(
               "device", "", "jetkvm-mini-wireless", "token", output,
               sizeof(output), &output_length) ==
           JETKVM_CLOUD_CORE_INVALID_HEADER_VALUE);
    assert(jetkvm_cloud_core_make_headers(
               "device", "version", "", "token", output,
               sizeof(output), &output_length) ==
           JETKVM_CLOUD_CORE_INVALID_HEADER_VALUE);
    assert(jetkvm_cloud_core_make_headers(
               "device", "version", "sku with space", "token", output,
               sizeof(output), &output_length) ==
           JETKVM_CLOUD_CORE_INVALID_HEADER_VALUE);
    assert(jetkvm_cloud_core_make_headers(
               "device", "version", "jetkvm-mini-wireless",
               "token with space", output, sizeof(output),
               &output_length) ==
           JETKVM_CLOUD_CORE_INVALID_HEADER_VALUE);
}

static void test_single_frame_chunking(void)
{
    uint8_t storage[64];
    jetkvm_cloud_reassembler_t state;
    jetkvm_cloud_reassembler_init(&state, storage, sizeof(storage));
    assert(jetkvm_cloud_reassembler_push(
               &state, JETKVM_CLOUD_WS_OPCODE_TEXT, true, 11, 0,
               (const uint8_t *)"hello ", 6) ==
           JETKVM_CLOUD_REASSEMBLY_INCOMPLETE);
    assert(jetkvm_cloud_reassembler_push(
               &state, JETKVM_CLOUD_WS_OPCODE_TEXT, true, 11, 6,
               (const uint8_t *)"world", 5) ==
           JETKVM_CLOUD_REASSEMBLY_COMPLETE);
    assert(jetkvm_cloud_reassembler_message_size(&state) == 11);
    assert(strcmp((const char *)jetkvm_cloud_reassembler_message_data(&state),
                  "hello world") == 0);

    /* Completed messages must be explicitly consumed/reset before reuse. */
    assert(jetkvm_cloud_reassembler_push(
               &state, JETKVM_CLOUD_WS_OPCODE_TEXT, true, 1, 0,
               (const uint8_t *)"x", 1) ==
           JETKVM_CLOUD_REASSEMBLY_PROTOCOL_ERROR);
    assert(jetkvm_cloud_reassembler_message_data(&state) == NULL);
}

static void test_fragmented_message_and_control(void)
{
    uint8_t storage[64];
    jetkvm_cloud_reassembler_t state;
    jetkvm_cloud_reassembler_init(&state, storage, sizeof(storage));
    assert(jetkvm_cloud_reassembler_push(
               &state, JETKVM_CLOUD_WS_OPCODE_TEXT, false, 5, 0,
               (const uint8_t *)"first", 5) ==
           JETKVM_CLOUD_REASSEMBLY_INCOMPLETE);
    assert(jetkvm_cloud_reassembler_push(
               &state, JETKVM_CLOUD_WS_OPCODE_PING, true, 0, 0,
               NULL, 0) == JETKVM_CLOUD_REASSEMBLY_CONTROL);
    assert(jetkvm_cloud_reassembler_push(
               &state, JETKVM_CLOUD_WS_OPCODE_CONTINUATION, false, 1, 0,
               (const uint8_t *)"-", 1) ==
           JETKVM_CLOUD_REASSEMBLY_INCOMPLETE);
    assert(jetkvm_cloud_reassembler_push(
               &state, JETKVM_CLOUD_WS_OPCODE_CONTINUATION, true, 4, 0,
               (const uint8_t *)"last", 4) ==
           JETKVM_CLOUD_REASSEMBLY_COMPLETE);
    assert(strcmp((const char *)jetkvm_cloud_reassembler_message_data(&state),
                  "first-last") == 0);
}

static void test_reassembly_rejections(void)
{
    uint8_t storage[32];
    jetkvm_cloud_reassembler_t state;
    jetkvm_cloud_reassembler_init(&state, storage, sizeof(storage));

    assert(jetkvm_cloud_reassembler_push(
               &state, JETKVM_CLOUD_WS_OPCODE_CONTINUATION, true, 1, 0,
               (const uint8_t *)"x", 1) ==
           JETKVM_CLOUD_REASSEMBLY_PROTOCOL_ERROR);
    assert(jetkvm_cloud_reassembler_push(
               &state, JETKVM_CLOUD_WS_OPCODE_BINARY, true, 1, 0,
               (const uint8_t *)"x", 1) ==
           JETKVM_CLOUD_REASSEMBLY_PROTOCOL_ERROR);

    assert(jetkvm_cloud_reassembler_push(
               &state, JETKVM_CLOUD_WS_OPCODE_TEXT, true, 4, 0,
               (const uint8_t *)"ab", 2) ==
           JETKVM_CLOUD_REASSEMBLY_INCOMPLETE);
    assert(jetkvm_cloud_reassembler_push(
               &state, JETKVM_CLOUD_WS_OPCODE_TEXT, true, 4, 3,
               (const uint8_t *)"d", 1) ==
           JETKVM_CLOUD_REASSEMBLY_PROTOCOL_ERROR);

    assert(jetkvm_cloud_reassembler_push(
               &state, JETKVM_CLOUD_WS_OPCODE_TEXT, true,
               JETKVM_CLOUD_CORE_MAX_MESSAGE_SIZE + 1U, 0,
               NULL, 0) == JETKVM_CLOUD_REASSEMBLY_TOO_LARGE);

    assert(jetkvm_cloud_reassembler_push(
               &state, JETKVM_CLOUD_WS_OPCODE_TEXT, true, 0, 0,
               NULL, 0) == JETKVM_CLOUD_REASSEMBLY_COMPLETE);
    assert(jetkvm_cloud_reassembler_message_size(&state) == 0);
    assert(jetkvm_cloud_reassembler_message_data(&state)[0] == '\0');
}

static void test_reassembly_exact_size_limit(void)
{
    const size_t message_size = JETKVM_CLOUD_CORE_MAX_MESSAGE_SIZE;
    uint8_t *storage = malloc(message_size + 1U);
    uint8_t *payload = malloc(message_size);
    assert(storage != NULL);
    assert(payload != NULL);
    memset(payload, 'x', message_size);

    jetkvm_cloud_reassembler_t state;
    jetkvm_cloud_reassembler_init(&state, storage, message_size + 1U);
    assert(jetkvm_cloud_reassembler_push(
               &state, JETKVM_CLOUD_WS_OPCODE_TEXT, true, message_size, 0,
               payload, message_size) == JETKVM_CLOUD_REASSEMBLY_COMPLETE);
    assert(jetkvm_cloud_reassembler_message_size(&state) == message_size);
    assert(jetkvm_cloud_reassembler_message_data(&state)[message_size] ==
           '\0');

    free(payload);
    free(storage);
}

static void test_epoch_binding(void)
{
    jetkvm_cloud_epoch_state_t state = { 0 };
    uint32_t first = jetkvm_cloud_epoch_connected(&state);
    assert(first != 0);
    assert(jetkvm_cloud_epoch_is_current_connection(&state, first));
    assert(jetkvm_cloud_epoch_bind_session(&state, first, 10, 20));
    assert(jetkvm_cloud_epoch_matches_session(&state, first, 10, 20));

    jetkvm_cloud_epoch_disconnected(&state, first + 1U);
    assert(jetkvm_cloud_epoch_is_current_connection(&state, first));
    jetkvm_cloud_epoch_disconnected(&state, first);
    assert(!jetkvm_cloud_epoch_matches_session(&state, first, 10, 20));
    assert(jetkvm_cloud_epoch_replace_generation(&state, 10, 20, 21));
    assert(state.generation == 21);
    assert(!jetkvm_cloud_epoch_replace_generation(&state, 10, 20, 22));

    uint32_t second = jetkvm_cloud_epoch_connected(&state);
    assert(second != first);
    assert(!jetkvm_cloud_epoch_matches_session(&state, second, 10, 20));
    assert(jetkvm_cloud_epoch_bind_session(&state, second, 30, 40));
    jetkvm_cloud_epoch_clear_session(&state, 10, 20);
    assert(jetkvm_cloud_epoch_matches_session(&state, second, 30, 40));
    jetkvm_cloud_epoch_clear_session(&state, 30, 40);
    assert(!state.session_active);

    state.connection_epoch = UINT32_MAX;
    assert(jetkvm_cloud_epoch_connected(&state) == 1U);
}

static void test_authenticated_signaling_policy(void)
{
    assert(!jetkvm_cloud_core_authenticated_step_requires_reconnect(false,
                                                                     false));
    assert(!jetkvm_cloud_core_authenticated_step_requires_reconnect(true,
                                                                     true));
    assert(jetkvm_cloud_core_authenticated_step_requires_reconnect(true,
                                                                    false));
}

/* Drives the retry state like the client worker: a tick every 250 ms, an
 * attempt when due, and a 5 s delay when the attempt fails. Returns the
 * number of attempts in [0, end_ms) and the time of the first one. */
static unsigned simulate_attempts(int64_t network_up_ms, int64_t dns_ready_ms,
                                  int64_t end_ms, int64_t *first_ms,
                                  int64_t *connected_ms)
{
    jetkvm_cloud_retry_t retry = { 0 };
    unsigned attempts = 0;
    *first_ms = -1;
    *connected_ms = -1;
    jetkvm_cloud_retry_request(&retry, 0, 0);
    for (int64_t now_ms = 0; now_ms < end_ms; now_ms += 250) {
        int64_t now_us = now_ms * 1000;
        if (!jetkvm_cloud_retry_due(&retry, now_us, now_ms >= network_up_ms)) {
            continue;
        }
        jetkvm_cloud_retry_started(&retry);
        attempts++;
        if (*first_ms < 0) {
            *first_ms = now_ms;
        }
        if (now_ms >= dns_ready_ms) {
            *connected_ms = now_ms;
            break;
        }
        jetkvm_cloud_retry_request(&retry, now_us, 5000);
    }
    return attempts;
}

static void test_reconnect_schedule(void)
{
    int64_t first_ms = 0;
    int64_t connected_ms = 0;

    /* Boot on the Wi-Fi board: no address for 10 s, DNS with the address.
     * No attempt before the network, and the first one connects. */
    assert(simulate_attempts(10000, 10000, 60000, &first_ms,
                             &connected_ms) == 1);
    assert(first_ms == 10000 && connected_ms == 10000);

    /* Address at 10 s, DNS only at 12.2 s: one failed attempt, then the next
     * one 5 s later connects. */
    assert(simulate_attempts(10000, 12200, 60000, &first_ms,
                             &connected_ms) == 2);
    assert(first_ms == 10000 && connected_ms == 15000);

    /* A cloud outage of a minute with the network up: at most one attempt
     * every 5 s, as upstream, instead of one every ~140 ms. */
    assert(simulate_attempts(0, 61000, 60000, &first_ms, &connected_ms) == 12);
    assert(connected_ms == -1);

    /* No network at all: no attempt, so no TLS ticket either. */
    assert(simulate_attempts(120000, 0, 60000, &first_ms, &connected_ms) == 0);

    /* The deadline is inclusive and wide enough for the largest delay. */
    jetkvm_cloud_retry_t retry = { 0 };
    assert(!jetkvm_cloud_retry_due(&retry, 0, true));
    jetkvm_cloud_retry_request(&retry, 1000, 3600000U);
    assert(!jetkvm_cloud_retry_due(&retry, 3600000999LL, true));
    assert(jetkvm_cloud_retry_due(&retry, 3600001000LL, true));
    assert(!jetkvm_cloud_retry_due(&retry, 3600001000LL, false));
    jetkvm_cloud_retry_started(&retry);
    assert(!jetkvm_cloud_retry_due(&retry, 3600001000LL, true));
}

int main(void)
{
    test_url_mapping();
    test_headers();
    test_single_frame_chunking();
    test_fragmented_message_and_control();
    test_reassembly_rejections();
    test_reassembly_exact_size_limit();
    test_epoch_binding();
    test_authenticated_signaling_policy();
    test_reconnect_schedule();
    puts("JetKVM cloud client core tests: OK");
    return 0;
}

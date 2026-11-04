#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpc_events.h"

static void test_usb_bus_state_mapping(void)
{
    assert(webrtc_rpc_usb_state_from_bus(false, false, false, false) ==
           WEBRTC_RPC_USB_NOT_ATTACHED);
    assert(webrtc_rpc_usb_state_from_bus(true, false, false, false) ==
           WEBRTC_RPC_USB_NOT_ATTACHED);
    assert(webrtc_rpc_usb_state_from_bus(true, true, false, false) ==
           WEBRTC_RPC_USB_ATTACHED);
    assert(webrtc_rpc_usb_state_from_bus(true, true, true, false) ==
           WEBRTC_RPC_USB_CONFIGURED);
    assert(webrtc_rpc_usb_state_from_bus(true, true, true, true) ==
           WEBRTC_RPC_USB_SUSPENDED);
    /* A detached snapshot wins over stale mounted/suspended flags. */
    assert(webrtc_rpc_usb_state_from_bus(true, false, true, true) ==
           WEBRTC_RPC_USB_NOT_ATTACHED);
}

static void test_hid_diff(void)
{
    webrtc_rpc_hid_state_t previous = {0};
    webrtc_rpc_hid_state_t current = {0};
    assert(webrtc_rpc_event_hid_diff(NULL, &current, false) ==
           WEBRTC_RPC_EVENT_HID_MASK);
    assert(webrtc_rpc_event_hid_diff(&previous, &current, true) == 0);

    current.usb = WEBRTC_RPC_USB_CONFIGURED;
    assert(webrtc_rpc_event_hid_diff(&previous, &current, true) ==
           WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_USB_STATE));
    previous = current;
    current.keyboard_leds = 0x03;
    assert(webrtc_rpc_event_hid_diff(&previous, &current, true) ==
           WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_KEYBOARD_LEDS));
    previous = current;
    current.modifier = 0x02;
    assert(webrtc_rpc_event_hid_diff(&previous, &current, true) ==
           (WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_KEYBOARD_LEDS) |
            WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_KEYS_DOWN)));
    previous = current;
    current.keys[0] = 4;
    assert(webrtc_rpc_event_hid_diff(&previous, &current, true) ==
           WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_KEYS_DOWN));
    /* A press and its release between two polls: the keys are as before,
     * but the host accepted two reports. */
    previous = current;
    current.keyboard_revision += 2U;
    assert(webrtc_rpc_event_hid_diff(&previous, &current, true) ==
           WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_KEYS_DOWN));
}

static void expect_render(webrtc_rpc_event_kind_t kind,
                          const webrtc_rpc_event_snapshot_t *snapshot,
                          const char *expected)
{
    char output[384];
    size_t output_size = 0;
    assert(webrtc_rpc_event_render(kind, snapshot, output, sizeof(output),
                                   &output_size));
    assert(output_size == strlen(expected));
    assert(strcmp(output, expected) == 0);
    char short_output[8];
    assert(!webrtc_rpc_event_render(kind, snapshot, short_output,
                                    sizeof(short_output), &output_size));
}

static void test_render(void)
{
    webrtc_rpc_event_snapshot_t snapshot = {
        .video = {
            .ready = true,
            .streaming = WEBRTC_RPC_VIDEO_ACTIVE,
            .width = 1920,
            .height = 1080,
            .fps = 30,
        },
        .hid = {
            .usb = WEBRTC_RPC_USB_CONFIGURED,
            .keyboard_leds = 0x0b,
            .modifier = 0x02,
            .keys = {4, 5, 0, 0, 0, 0},
        },
    };
    expect_render(
        WEBRTC_RPC_EVENT_VIDEO_INPUT, &snapshot,
        "{\"jsonrpc\":\"2.0\",\"method\":\"videoInputState\","
        "\"params\":{\"ready\":true,\"streaming\":1,"
        "\"width\":1920,\"height\":1080,\"fps\":30}}");
    /* An upload to the TF card holds video off. */
    snapshot.video.upload_pauses = true;
    expect_render(
        WEBRTC_RPC_EVENT_VIDEO_INPUT, &snapshot,
        "{\"jsonrpc\":\"2.0\",\"method\":\"videoInputState\","
        "\"params\":{\"ready\":true,\"streaming\":1,"
        "\"uploadPausesVideo\":true,"
        "\"width\":1920,\"height\":1080,\"fps\":30}}");
    snapshot.video.ready = false;
    expect_render(
        WEBRTC_RPC_EVENT_VIDEO_INPUT, &snapshot,
        "{\"jsonrpc\":\"2.0\",\"method\":\"videoInputState\","
        "\"params\":{\"ready\":false,\"streaming\":1,"
        "\"error\":\"no_signal\",\"uploadPausesVideo\":true,"
        "\"width\":0,\"height\":0,\"fps\":0}}");
    snapshot.video.ready = true;
    snapshot.video.upload_pauses = false;
    expect_render(
        WEBRTC_RPC_EVENT_USB_STATE, &snapshot,
        "{\"jsonrpc\":\"2.0\",\"method\":\"usbState\","
        "\"params\":\"configured\"}");
    expect_render(
        WEBRTC_RPC_EVENT_KEYBOARD_LEDS, &snapshot,
        "{\"jsonrpc\":\"2.0\",\"method\":\"keyboardLedState\","
        "\"params\":{\"num_lock\":true,\"caps_lock\":true,"
        "\"scroll_lock\":false,\"compose\":true,\"kana\":false,"
        "\"shift\":true}}");
    expect_render(
        WEBRTC_RPC_EVENT_KEYS_DOWN, &snapshot,
        "{\"jsonrpc\":\"2.0\",\"method\":\"keysDownState\","
        "\"params\":{\"modifier\":2,\"keys\":[4,5,0,0,0,0]}}");

    snapshot.video = (webrtc_rpc_video_state_t) {0};
    expect_render(
        WEBRTC_RPC_EVENT_VIDEO_INPUT, &snapshot,
        "{\"jsonrpc\":\"2.0\",\"method\":\"videoInputState\","
        "\"params\":{\"ready\":false,\"streaming\":0,"
        "\"error\":\"no_signal\",\"width\":0,\"height\":0,\"fps\":0}}");
    /* OS Services is moving the display between sources: no error. */
    snapshot.video.switching = true;
    expect_render(
        WEBRTC_RPC_EVENT_VIDEO_INPUT, &snapshot,
        "{\"jsonrpc\":\"2.0\",\"method\":\"videoInputState\","
        "\"params\":{\"ready\":false,\"streaming\":0,"
        "\"width\":0,\"height\":0,\"fps\":0}}");
}

static void test_hidrpc_render(void)
{
    const webrtc_rpc_hid_state_t hid = {
        .keyboard_leds = 0x03,
        .modifier = 0x22,
        .keys = {4, 5, 0, 0, 0, 0},
    };
    uint8_t output[8] = {0};
    size_t output_size = 0;
    assert(webrtc_hidrpc_state_render(
        WEBRTC_HIDRPC_STATE_KEYBOARD_LEDS, &hid, false, false,
        output, sizeof(output), &output_size));
    const uint8_t expected_leds[] = {0x32, 0x43};
    assert(output_size == sizeof(expected_leds));
    assert(memcmp(output, expected_leds, sizeof(expected_leds)) == 0);

    assert(webrtc_hidrpc_state_render(
        WEBRTC_HIDRPC_STATE_KEYS_DOWN, &hid, false, false,
        output, sizeof(output), &output_size));
    const uint8_t expected_keys[] = {0x33, 0x22, 4, 5, 0, 0, 0, 0};
    assert(output_size == sizeof(expected_keys));
    assert(memcmp(output, expected_keys, sizeof(expected_keys)) == 0);

    assert(webrtc_hidrpc_state_render(
        WEBRTC_HIDRPC_STATE_KEYBOARD_MACRO, &hid, true, true,
        output, sizeof(output), &output_size));
    const uint8_t expected_macro[] = {0x34, 1, 1};
    assert(output_size == sizeof(expected_macro));
    assert(memcmp(output, expected_macro, sizeof(expected_macro)) == 0);
    assert(!webrtc_hidrpc_state_render(
        WEBRTC_HIDRPC_STATE_KEYS_DOWN, &hid, false, false,
        output, sizeof(output) - 1U, &output_size));
}

/* Members to add, as jetkvm_control_render_guest_video_fields() writes them:
 * NUL-terminated when they fit, nothing promised in output when not. */
static const char *members;
static bool members_fail;
static bool render_members(char *output, size_t capacity, size_t *length)
{
    size_t size = strlen(members);
    memset(output, 'X', capacity);
    *length = 0;
    if (members_fail || size + 1U > capacity) {
        return false;
    }
    memcpy(output, members, size + 1U);
    *length = size;
    return true;
}

/* Claims all of its window, leaving no room for the braces. */
static bool render_overlong(char *output, size_t capacity, size_t *length)
{
    memset(output, ',', capacity);
    *length = capacity;
    return true;
}

/* Fails, but leaves a length behind. */
static bool render_fail_with_length(char *output, size_t capacity, size_t *length)
{
    (void)output;
    *length = capacity - 1U;
    return false;
}

static void test_add_params(void)
{
    static const char event[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"videoInputState\","
        "\"params\":{\"ready\":false,\"streaming\":0,"
        "\"error\":\"no_signal\",\"width\":0,\"height\":0,\"fps\":0}}";
    static const char extended[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"videoInputState\","
        "\"params\":{\"ready\":false,\"streaming\":0,"
        "\"error\":\"no_signal\",\"width\":0,\"height\":0,\"fps\":0,"
        "\"source\":\"guest\",\"osServices\":{\"state\":\"connected\"}}}";
    const webrtc_rpc_event_snapshot_t snapshot = {0};
    char output[1280];
    size_t size = 0;

    members = ",\"source\":\"guest\",\"osServices\":{\"state\":\"connected\"}";
    members_fail = false;
    assert(webrtc_rpc_event_render(WEBRTC_RPC_EVENT_VIDEO_INPUT, &snapshot,
                                   output, sizeof(output), &size));
    assert(webrtc_rpc_event_add_params(output, sizeof(output), &size,
                                       render_members));
    assert(size == strlen(extended) && strcmp(output, extended) == 0);

    /* No members leave the event byte for byte as it was. */
    members = "";
    assert(webrtc_rpc_event_render(WEBRTC_RPC_EVENT_VIDEO_INPUT, &snapshot,
                                   output, sizeof(output), &size));
    assert(webrtc_rpc_event_add_params(output, sizeof(output), &size,
                                       render_members));
    assert(size == strlen(event) && strcmp(output, event) == 0);

    /* The extended event and its NUL fill the buffer exactly; one byte less
     * and the event goes out as it was. */
    members = ",\"source\":\"guest\",\"osServices\":{\"state\":\"connected\"}";
    for (size_t capacity = sizeof(extended) - 1U; capacity <= sizeof(extended);
         ++capacity) {
        memset(output, 0, sizeof(output));
        memcpy(output, event, sizeof(event));
        size = strlen(event);
        bool fits = capacity == sizeof(extended);
        assert(webrtc_rpc_event_add_params(output, capacity, &size,
                                           render_members) == fits);
        assert(strcmp(output, fits ? extended : event) == 0);
        assert(size == strlen(output));
        assert(output[capacity] == '\0'); /* nothing written past capacity */
    }

    members_fail = true;
    memcpy(output, event, sizeof(event));
    size = strlen(event);
    assert(!webrtc_rpc_event_add_params(output, sizeof(output), &size,
                                        render_members));
    assert(size == strlen(event) && strcmp(output, event) == 0);

    /* Only a rendered notification, ending in two braces, takes members. */
    members_fail = false;
    memcpy(output, "{\"params\":[]}", 14);
    size = 13;
    assert(!webrtc_rpc_event_add_params(output, sizeof(output), &size,
                                        render_members));
    assert(size == 13 && strcmp(output, "{\"params\":[]}") == 0);

    /* A renderer that breaks its contract cannot push the braces past the
     * buffer, a heap block with five bytes to spare (ASan checks). */
    static const webrtc_rpc_event_members_fn broken[] = {
        render_overlong, render_fail_with_length,
    };
    for (size_t i = 0; i < sizeof(broken) / sizeof(broken[0]); ++i) {
        char *exact = malloc(sizeof(event) + 4U);
        assert(exact);
        memcpy(exact, event, sizeof(event));
        size = strlen(event);
        assert(!webrtc_rpc_event_add_params(exact, sizeof(event) + 4U, &size,
                                            broken[i]));
        assert(size == strlen(event) && strcmp(exact, event) == 0);
        free(exact);
    }
    /* No room even for the NUL: refused before anything is written. */
    char *full = malloc(strlen(event));
    assert(full);
    memcpy(full, event, strlen(event));
    size = strlen(event);
    assert(!webrtc_rpc_event_add_params(full, strlen(event), &size,
                                        render_members));
    assert(size == strlen(event) && memcmp(full, event, size) == 0);
    free(full);
}

int main(void)
{
    test_usb_bus_state_mapping();
    test_hid_diff();
    test_render();
    test_hidrpc_render();
    test_add_params();
    puts("rpc event tests: OK");
    return 0;
}

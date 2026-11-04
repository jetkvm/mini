/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEBRTC_RPC_EVENT_KEY_COUNT 6U

typedef enum {
    WEBRTC_RPC_EVENT_VIDEO_INPUT = 0,
    WEBRTC_RPC_EVENT_USB_STATE,
    WEBRTC_RPC_EVENT_KEYBOARD_LEDS,
    WEBRTC_RPC_EVENT_KEYS_DOWN,
    /* Sent once per rpc channel; rendered by jetkvm_control, not by
     * webrtc_rpc_event_render. */
    WEBRTC_RPC_EVENT_LOCAL_VERSION,
    WEBRTC_RPC_EVENT_DEVICE_CAPABILITIES,
    WEBRTC_RPC_EVENT_COUNT,
} webrtc_rpc_event_kind_t;

#define WEBRTC_RPC_EVENT_MASK(kind) (1UL << (unsigned)(kind))
#define WEBRTC_RPC_EVENT_HID_MASK                                          \
    (WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_USB_STATE) |                  \
     WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_KEYBOARD_LEDS) |              \
     WEBRTC_RPC_EVENT_MASK(WEBRTC_RPC_EVENT_KEYS_DOWN))

typedef enum {
    WEBRTC_RPC_VIDEO_INACTIVE = 0,
    WEBRTC_RPC_VIDEO_ACTIVE,
    WEBRTC_RPC_VIDEO_STOPPING,
} webrtc_rpc_video_streaming_t;

typedef enum {
    WEBRTC_RPC_USB_NOT_ATTACHED = 0,
    WEBRTC_RPC_USB_ATTACHED,
    WEBRTC_RPC_USB_ADDRESSED,
    WEBRTC_RPC_USB_CONFIGURED,
    WEBRTC_RPC_USB_SUSPENDED,
} webrtc_rpc_usb_state_t;

/** Map a P4 TinyUSB bus snapshot to the JSON-RPC usbState vocabulary. */
webrtc_rpc_usb_state_t webrtc_rpc_usb_state_from_bus(
    bool initialized, bool connected, bool mounted, bool suspended);

typedef struct {
    bool ready;
    /* Not ready while the source changes: no error. */
    bool switching;
    webrtc_rpc_video_streaming_t streaming;
    /* An upload to the TF card holds video off (uploadPausesVideo). */
    bool upload_pauses;
    uint16_t width;
    uint16_t height;
    uint16_t fps;
} webrtc_rpc_video_state_t;

typedef struct {
    webrtc_rpc_usb_state_t usb;
    uint8_t keyboard_leds;
    uint8_t modifier;
    uint8_t keys[WEBRTC_RPC_EVENT_KEY_COUNT];
    /* Counts the keyboard reports the host accepted. A press and its release
     * between two polls leave the keys as they were; the count still moves,
     * so keysDownState still goes out. */
    uint32_t keyboard_revision;
} webrtc_rpc_hid_state_t;

typedef struct {
    webrtc_rpc_video_state_t video;
    webrtc_rpc_hid_state_t hid;
} webrtc_rpc_event_snapshot_t;

typedef enum {
    WEBRTC_HIDRPC_STATE_KEYBOARD_LEDS = 0,
    WEBRTC_HIDRPC_STATE_KEYS_DOWN,
    WEBRTC_HIDRPC_STATE_KEYBOARD_MACRO,
    WEBRTC_HIDRPC_STATE_COUNT,
} webrtc_hidrpc_state_kind_t;

#define WEBRTC_HIDRPC_STATE_MASK(kind) (1UL << (unsigned)(kind))
#define WEBRTC_HIDRPC_STATE_ALL_MASK                                  \
    ((1UL << (unsigned)WEBRTC_HIDRPC_STATE_COUNT) - 1UL)

/** Return the coalescible HID notification bits changed by the new snapshot. */
uint32_t webrtc_rpc_event_hid_diff(
    const webrtc_rpc_hid_state_t *previous,
    const webrtc_rpc_hid_state_t *current,
    bool previous_valid);

/** Render one canonical JetKVM JSON-RPC 2.0 server notification. */
bool webrtc_rpc_event_render(
    webrtc_rpc_event_kind_t kind,
    const webrtc_rpc_event_snapshot_t *snapshot,
    char *output, size_t output_capacity, size_t *output_size);

/* Writes members, each with a leading comma, at output; false when they do
 * not fit capacity with their NUL. */
typedef bool (*webrtc_rpc_event_members_fn)(char *output, size_t capacity,
                                            size_t *length);

/**
 * Add members to the params object of a rendered notification (output_size
 * bytes, ending "}}"), in place. Returns false, with the notification as it
 * was, when render fails or the members do not fit output_capacity.
 */
bool webrtc_rpc_event_add_params(
    char *output, size_t output_capacity, size_t *output_size,
    webrtc_rpc_event_members_fn render);

/** Render one JetKVM HIDRPC v1 server-to-browser state message. */
bool webrtc_hidrpc_state_render(
    webrtc_hidrpc_state_kind_t kind,
    const webrtc_rpc_hid_state_t *hid,
    bool macro_active, bool macro_is_paste,
    uint8_t *output, size_t output_capacity, size_t *output_size);

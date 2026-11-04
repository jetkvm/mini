/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JETKVM_HIDRPC_VERSION 0x01U
#define JETKVM_HIDRPC_TYPE_HANDSHAKE 0x01U
#define JETKVM_HIDRPC_TYPE_KEYBOARD_REPORT 0x02U
#define JETKVM_HIDRPC_TYPE_ABS_MOUSE_REPORT 0x03U
#define JETKVM_HIDRPC_TYPE_WHEEL_REPORT 0x04U
#define JETKVM_HIDRPC_TYPE_KEYPRESS_REPORT 0x05U
#define JETKVM_HIDRPC_TYPE_REL_MOUSE_REPORT 0x06U
#define JETKVM_HIDRPC_TYPE_KEYBOARD_MACRO_REPORT 0x07U
#define JETKVM_HIDRPC_TYPE_CANCEL_KEYBOARD_MACRO_REPORT 0x08U
#define JETKVM_HIDRPC_TYPE_KEYPRESS_KEEPALIVE_REPORT 0x09U
#define JETKVM_HIDRPC_TYPE_KEYBOARD_LED_STATE 0x32U
#define JETKVM_HIDRPC_TYPE_KEYS_DOWN_STATE 0x33U
#define JETKVM_HIDRPC_TYPE_KEYBOARD_MACRO_STATE 0x34U

#define JETKVM_HIDRPC_KEY_COUNT 6U
#define JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE 9U
/*
 * P4 keeps macro delivery allocation-free.  This is also a protocol parsing
 * limit: a larger message is rejected before an event view reaches the
 * WebRTC callback, so downstream users can always make a fixed-size copy.
 */
#define JETKVM_HIDRPC_MACRO_MAX_STEPS 128U
#define JETKVM_CONTROL_KEYBOARD_LAYOUT_CAPACITY 32U
#define JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY 256U
#define JETKVM_VIRTUAL_MEDIA_UPLOAD_ID_CAPACITY 48U
#define JETKVM_VIRTUAL_MEDIA_URL_CAPACITY 2048U
#define JETKVM_VIRTUAL_MEDIA_REASON_CAPACITY 192U
#define JETKVM_VIRTUAL_MEDIA_MAX_LISTED_FILES 64U
#define JETKVM_CONTROL_WOL_MAX_DEVICES 16U
#define JETKVM_CONTROL_WOL_NAME_CAPACITY 64U
#define JETKVM_CONTROL_WOL_MAC_CAPACITY 18U
#define JETKVM_CONTROL_WOL_BROADCAST_CAPACITY 16U
#define JETKVM_CONTROL_KEYBOARD_MACRO_MAX_COUNT 25U
#define JETKVM_CONTROL_KEYBOARD_MACRO_MAX_STEPS 10U
#define JETKVM_CONTROL_KEYBOARD_MACRO_MAX_KEYS 10U
#define JETKVM_CONTROL_KEYBOARD_MACRO_MAX_MODIFIERS 8U
#define JETKVM_CONTROL_KEYBOARD_MACRO_KEY_NAME_COUNT 230U
#define JETKVM_CONTROL_KEYBOARD_MACRO_ID_CAPACITY 32U
/* The UI limits names to 50 UTF-16 code units. 151 bytes cover the largest
 * valid all-BMP UTF-8 name plus its terminator. */
#define JETKVM_CONTROL_KEYBOARD_MACRO_NAME_CAPACITY 151U
#define JETKVM_CONTROL_JIGGLER_CRON_CAPACITY 64U
#define JETKVM_CONTROL_JIGGLER_TIMEZONE_CAPACITY 64U
#define JETKVM_CONTROL_JIGGLER_DEFAULT_CRON "0 * * * * *"
#define JETKVM_CONTROL_JIGGLER_DEFAULT_TIMEZONE "UTC"
#define JETKVM_CONTROL_VIDEO_LOG_STATUS_CAPACITY 2048U

typedef enum {
    JETKVM_CONTROL_NO_RESPONSE = 0,
    JETKVM_CONTROL_RESPONSE_READY = 1,
    JETKVM_CONTROL_EVENT_HANDLED = 2,
    JETKVM_CONTROL_OUTPUT_TOO_SMALL = -1,
    JETKVM_CONTROL_INVALID_ARGUMENT = -2,
    JETKVM_CONTROL_MALFORMED_INPUT = -3,
} jetkvm_control_result_t;

typedef enum {
    JETKVM_HIDRPC_EVENT_KEYBOARD_REPORT =
        JETKVM_HIDRPC_TYPE_KEYBOARD_REPORT,
    JETKVM_HIDRPC_EVENT_ABS_MOUSE_REPORT =
        JETKVM_HIDRPC_TYPE_ABS_MOUSE_REPORT,
    JETKVM_HIDRPC_EVENT_WHEEL_REPORT =
        JETKVM_HIDRPC_TYPE_WHEEL_REPORT,
    JETKVM_HIDRPC_EVENT_KEYPRESS_REPORT =
        JETKVM_HIDRPC_TYPE_KEYPRESS_REPORT,
    JETKVM_HIDRPC_EVENT_REL_MOUSE_REPORT =
        JETKVM_HIDRPC_TYPE_REL_MOUSE_REPORT,
    JETKVM_HIDRPC_EVENT_KEYBOARD_MACRO_REPORT =
        JETKVM_HIDRPC_TYPE_KEYBOARD_MACRO_REPORT,
    JETKVM_HIDRPC_EVENT_CANCEL_KEYBOARD_MACRO_REPORT =
        JETKVM_HIDRPC_TYPE_CANCEL_KEYBOARD_MACRO_REPORT,
    JETKVM_HIDRPC_EVENT_KEYPRESS_KEEPALIVE_REPORT =
        JETKVM_HIDRPC_TYPE_KEYPRESS_KEEPALIVE_REPORT,
} jetkvm_hidrpc_event_type_t;

typedef struct {
    uint8_t modifier;
    uint8_t keys[JETKVM_HIDRPC_KEY_COUNT];
} jetkvm_hidrpc_keyboard_report_t;

typedef struct {
    int32_t x;
    int32_t y;
    uint8_t buttons;
} jetkvm_hidrpc_abs_mouse_report_t;

typedef struct {
    uint8_t key;
    bool pressed;
} jetkvm_hidrpc_keypress_report_t;

typedef struct {
    int8_t dx;
    int8_t dy;
    uint8_t buttons;
} jetkvm_hidrpc_rel_mouse_report_t;

typedef struct {
    int8_t wheel_y;
    int8_t wheel_x;
} jetkvm_hidrpc_wheel_report_t;

typedef struct {
    uint8_t modifier;
    uint8_t keys[JETKVM_HIDRPC_KEY_COUNT];
    uint16_t delay_ms;
} jetkvm_hidrpc_macro_step_t;

typedef struct {
    bool is_paste;
    uint32_t step_count;
    const uint8_t *encoded_steps;
    size_t encoded_steps_size;
} jetkvm_hidrpc_keyboard_macro_report_t;

typedef struct {
    jetkvm_hidrpc_event_type_t type;
    union {
        jetkvm_hidrpc_keyboard_report_t keyboard;
        jetkvm_hidrpc_abs_mouse_report_t abs_mouse;
        jetkvm_hidrpc_keypress_report_t keypress;
        jetkvm_hidrpc_rel_mouse_report_t rel_mouse;
        jetkvm_hidrpc_wheel_report_t wheel;
        jetkvm_hidrpc_keyboard_macro_report_t macro;
    } data;
} jetkvm_hidrpc_event_t;

/**
 * Consume a validated HIDRPC input event.
 *
 * The event and any encoded_steps view are valid only for the duration of the
 * callback. A queueing USB implementation must copy data it retains.
 */
typedef void (*jetkvm_hidrpc_event_fn)(const jetkvm_hidrpc_event_t *event,
                                       void *ctx);

typedef struct {
    jetkvm_hidrpc_event_fn on_event;
    void *ctx;
} jetkvm_hidrpc_context_t;

typedef struct {
    bool num_lock;
    bool caps_lock;
    bool scroll_lock;
    bool compose;
    bool kana;
    bool shift;
} jetkvm_keyboard_led_state_t;

/** Return false when the USB keyboard state is temporarily unavailable. */
typedef bool (*jetkvm_get_keyboard_led_state_fn)(
    jetkvm_keyboard_led_state_t *state, void *ctx);

/** Return false when the current key-down state is temporarily unavailable. */
typedef bool (*jetkvm_get_key_down_state_fn)(
    jetkvm_hidrpc_keyboard_report_t *state, void *ctx);

typedef enum {
    JETKVM_USB_STATE_CONFIGURED = 0,
    JETKVM_USB_STATE_ATTACHED,
    JETKVM_USB_STATE_ADDRESSED,
    JETKVM_USB_STATE_SUSPENDED,
    JETKVM_USB_STATE_NOT_ATTACHED,
} jetkvm_usb_state_t;

/** Return false when the USB device state is temporarily unavailable. */
typedef bool (*jetkvm_get_usb_state_fn)(jetkvm_usb_state_t *state, void *ctx);

#define JETKVM_USB_DESCRIPTOR_STRING_CAPACITY 128U

/**
 * One self-contained view of the USB gadget configuration exposed on the
 * target-host port.  The identifiers describe the device descriptor; the
 * class flags describe interfaces actually compiled into the active gadget;
 * emulation_enabled reports whether the gadget controller is currently
 * installed/bound.  It deliberately does not describe the independent USB
 * Serial/JTAG debug port.
 */
typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    char serial_number[JETKVM_USB_DESCRIPTOR_STRING_CAPACITY];
    char manufacturer[JETKVM_USB_DESCRIPTOR_STRING_CAPACITY];
    char product[JETKVM_USB_DESCRIPTOR_STRING_CAPACITY];
    bool absolute_mouse;
    bool relative_mouse;
    bool keyboard;
    bool mass_storage;
    bool serial_console;
    bool audio;
    bool emulation_enabled;
} jetkvm_usb_configuration_snapshot_t;

/** Copy one coherent, caller-owned USB configuration snapshot. */
typedef bool (*jetkvm_get_usb_configuration_fn)(
    jetkvm_usb_configuration_snapshot_t *snapshot, void *ctx);

#define JETKVM_USB_DESCRIPTOR_CONFIG_STRING_MAX_LENGTH 31U
#define JETKVM_USB_DESCRIPTOR_CONFIG_STRING_CAPACITY                         \
    (JETKVM_USB_DESCRIPTOR_CONFIG_STRING_MAX_LENGTH + 1U)

/**
 * Validated USB device-descriptor values supplied by setUsbConfig. All five
 * JSON fields are required; descriptor strings may be empty.
 */
typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    char serial_number[JETKVM_USB_DESCRIPTOR_CONFIG_STRING_CAPACITY];
    char manufacturer[JETKVM_USB_DESCRIPTOR_CONFIG_STRING_CAPACITY];
    char product[JETKVM_USB_DESCRIPTOR_CONFIG_STRING_CAPACITY];
} jetkvm_usb_descriptor_config_t;

/**
 * Complete desired USB interface set supplied by setUsbDevices. Omitted JSON
 * bool fields are false, matching Go's encoding/json struct binding.
 */
typedef struct {
    bool absolute_mouse;
    bool relative_mouse;
    bool keyboard;
    bool mass_storage;
    bool serial_console;
    bool audio;
} jetkvm_usb_devices_t;

/** Device names accepted by the JetKVM-compatible setUsbDeviceState RPC. */
typedef enum {
    JETKVM_USB_DEVICE_ABSOLUTE_MOUSE = 0,
    JETKVM_USB_DEVICE_RELATIVE_MOUSE,
    JETKVM_USB_DEVICE_KEYBOARD,
    JETKVM_USB_DEVICE_MASS_STORAGE,
    JETKVM_USB_DEVICE_SERIAL_CONSOLE,
    JETKVM_USB_DEVICE_AUDIO,
} jetkvm_usb_device_t;

/**
 * Result of a synchronous USB configuration transaction. INVALID and
 * UNSUPPORTED map to JSON-RPC -32602; operational failures map to -32603.
 */
typedef enum {
    JETKVM_USB_CONFIGURATION_CHANGE_COMPLETED = 0,
    JETKVM_USB_CONFIGURATION_CHANGE_INVALID,
    JETKVM_USB_CONFIGURATION_CHANGE_UNSUPPORTED,
    JETKVM_USB_CONFIGURATION_CHANGE_FAILED,
    JETKVM_USB_CONFIGURATION_CHANGE_UNAVAILABLE,
    JETKVM_USB_CONFIGURATION_CHANGE_BUSY,
} jetkvm_usb_configuration_change_result_t;

/**
 * Apply validated descriptor values. The callback must copy the request before
 * returning if it needs to retain it.
 */
typedef jetkvm_usb_configuration_change_result_t
    (*jetkvm_set_usb_descriptor_config_fn)(
        const jetkvm_usb_descriptor_config_t *config, void *ctx);

/** Apply one complete desired USB interface set. */
typedef jetkvm_usb_configuration_change_result_t
    (*jetkvm_set_usb_devices_fn)(const jetkvm_usb_devices_t *devices,
                                void *ctx);

/** Apply one JetKVM USB interface state change. */
typedef jetkvm_usb_configuration_change_result_t
    (*jetkvm_set_usb_device_state_fn)(jetkvm_usb_device_t device,
                                     bool enabled, void *ctx);

typedef enum {
    /** The lifecycle owner completed the requested emulation state. */
    JETKVM_USB_EMULATION_CHANGE_ACCEPTED = 0,
    /** The target USB lifecycle owner is not available. */
    JETKVM_USB_EMULATION_CHANGE_UNAVAILABLE,
    /* Another deferred control operation owns the worker; retry later. */
    JETKVM_USB_EMULATION_CHANGE_BUSY,
} jetkvm_usb_emulation_change_result_t;

/**
 * Complete a target-port emulation change through the USB owner. The transport
 * dispatches this callback on a worker. ACCEPTED means the device driver
 * transition completed, not that the host has finished enumeration.
 */
typedef jetkvm_usb_emulation_change_result_t
    (*jetkvm_set_usb_emulation_enabled_fn)(bool enabled, void *ctx);

typedef struct {
    jetkvm_get_usb_configuration_fn get;
    jetkvm_set_usb_descriptor_config_fn set_descriptor_config;
    jetkvm_set_usb_devices_fn set_devices;
    jetkvm_set_usb_device_state_fn set_device_state;
    jetkvm_set_usb_emulation_enabled_fn set_emulation_enabled;
    void *ctx;
} jetkvm_usb_configuration_context_t;

typedef enum {
    JETKVM_CONTROL_VIRTUAL_MEDIA_MODE_CDROM = 0,
    JETKVM_CONTROL_VIRTUAL_MEDIA_MODE_DISK,
} jetkvm_control_virtual_media_mode_t;

typedef enum {
    JETKVM_CONTROL_VIRTUAL_MEDIA_SOURCE_STORAGE = 0,
    JETKVM_CONTROL_VIRTUAL_MEDIA_SOURCE_HTTP,
    /* The installer disc in the firmware. */
    JETKVM_CONTROL_VIRTUAL_MEDIA_SOURCE_BUILTIN,
} jetkvm_control_virtual_media_source_t;

typedef struct {
    bool initialized;
    bool card_present;
    /* Advances each time a card leaves the slot. */
    uint32_t card_generation;
    bool filesystem_mounted;
    bool mounted;
    jetkvm_control_virtual_media_source_t source;
    jetkvm_control_virtual_media_mode_t mode;
    char filename[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
    char url[JETKVM_VIRTUAL_MEDIA_URL_CAPACITY];
    uint64_t size;
} jetkvm_control_virtual_media_state_t;

typedef struct {
    bool usable;
    uint64_t size;
    char reason[JETKVM_VIRTUAL_MEDIA_REASON_CAPACITY];
} jetkvm_control_virtual_media_url_info_t;

typedef struct {
    uint64_t bytes_used;
    uint64_t bytes_free;
} jetkvm_control_storage_space_t;

typedef struct {
    char filename[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
    uint64_t size;
    int64_t created_at;
} jetkvm_control_storage_file_t;

/**
 * Synchronous visitor used while a storage snapshot remains valid.  The
 * visitor must copy anything it needs before returning.
 */
typedef bool (*jetkvm_visit_storage_file_fn)(
    const jetkvm_control_storage_file_t *file, void *ctx);

typedef struct {
    size_t count;
    bool truncated;
    jetkvm_control_storage_file_t files[JETKVM_VIRTUAL_MEDIA_MAX_LISTED_FILES];
} jetkvm_control_storage_files_t;

typedef struct {
    uint64_t already_uploaded_bytes;
    char data_channel[JETKVM_VIRTUAL_MEDIA_UPLOAD_ID_CAPACITY];
} jetkvm_control_storage_upload_t;

typedef enum {
    JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED = 0,
    JETKVM_VIRTUAL_MEDIA_CHANGE_INVALID,
    JETKVM_VIRTUAL_MEDIA_CHANGE_NOT_FOUND,
    JETKVM_VIRTUAL_MEDIA_CHANGE_BUSY,
    JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE,
    JETKVM_VIRTUAL_MEDIA_CHANGE_URL_UNAVAILABLE, /* the image URL did not answer usably */
    JETKVM_VIRTUAL_MEDIA_CHANGE_FAILED,
    JETKVM_VIRTUAL_MEDIA_CHANGE_ALREADY_MOUNTED, /* another image is mounted */
} jetkvm_virtual_media_change_result_t;

typedef bool (*jetkvm_get_virtual_media_state_fn)(
    jetkvm_control_virtual_media_state_t *state, void *ctx);
typedef bool (*jetkvm_get_storage_space_fn)(
    jetkvm_control_storage_space_t *space, void *ctx);
typedef bool (*jetkvm_list_storage_files_fn)(
    jetkvm_visit_storage_file_fn visit, void *visit_ctx, void *ctx);
typedef jetkvm_virtual_media_change_result_t
    (*jetkvm_mount_storage_file_fn)(
        const char *filename, jetkvm_control_virtual_media_mode_t mode,
        void *ctx);
typedef bool (*jetkvm_check_mount_url_fn)(
    const char *url, jetkvm_control_virtual_media_url_info_t *info, void *ctx);
typedef jetkvm_virtual_media_change_result_t
    (*jetkvm_mount_http_fn)(const char *url,
                            jetkvm_control_virtual_media_mode_t mode,
                            void *ctx);
typedef jetkvm_virtual_media_change_result_t
    (*jetkvm_storage_filename_action_fn)(const char *filename, void *ctx);
typedef jetkvm_virtual_media_change_result_t
    (*jetkvm_unmount_virtual_media_fn)(void *ctx);
/* Mounts the firmware's installer disc as a CD-ROM; refuses with
 * JETKVM_VIRTUAL_MEDIA_CHANGE_ALREADY_MOUNTED while an image is mounted. */
typedef jetkvm_virtual_media_change_result_t
    (*jetkvm_mount_builtin_installer_fn)(void *ctx);
/* formatStorage: the card is erased, or the reason why it was not. */
typedef enum {
    JETKVM_CONTROL_STORAGE_FORMAT_COMPLETED = 0,
    JETKVM_CONTROL_STORAGE_FORMAT_NOT_READY,
    JETKVM_CONTROL_STORAGE_FORMAT_NO_CARD,
    JETKVM_CONTROL_STORAGE_FORMAT_IMAGE_MOUNTED,
    JETKVM_CONTROL_STORAGE_FORMAT_UPLOAD_ACTIVE,
    /* Another virtual media request is running. */
    JETKVM_CONTROL_STORAGE_FORMAT_BUSY,
    JETKVM_CONTROL_STORAGE_FORMAT_FAILED,
} jetkvm_control_storage_format_result_t;

/* On success `space` holds the space of the new filesystem. */
typedef jetkvm_control_storage_format_result_t
    (*jetkvm_format_storage_fn)(jetkvm_control_storage_space_t *space,
                                void *ctx);
typedef jetkvm_virtual_media_change_result_t
    (*jetkvm_start_storage_upload_fn)(
        const char *filename, uint64_t size, const char *sha256,
        jetkvm_control_storage_upload_t *upload, void *ctx);

typedef struct {
    jetkvm_get_virtual_media_state_fn get_state;
    jetkvm_get_storage_space_fn get_space;
    jetkvm_list_storage_files_fn list_files;
    jetkvm_mount_storage_file_fn mount_storage;
    jetkvm_check_mount_url_fn check_url;
    jetkvm_mount_http_fn mount_http;
    jetkvm_mount_builtin_installer_fn mount_builtin_installer;
    jetkvm_unmount_virtual_media_fn unmount;
    jetkvm_format_storage_fn format_storage;
    jetkvm_storage_filename_action_fn delete_file;
    jetkvm_start_storage_upload_fn start_upload;
    void *ctx;
} jetkvm_virtual_media_context_t;

/* downloadToStorage: one HTTP(S) GET streamed into a file on the TF card. */
#define JETKVM_CONTROL_STORAGE_DOWNLOAD_ERROR_CAPACITY 128U

typedef enum {
    JETKVM_CONTROL_STORAGE_DOWNLOAD_IDLE = 0,
    JETKVM_CONTROL_STORAGE_DOWNLOAD_DOWNLOADING,
    JETKVM_CONTROL_STORAGE_DOWNLOAD_DONE,
    JETKVM_CONTROL_STORAGE_DOWNLOAD_ERROR,
} jetkvm_control_storage_download_phase_t;

typedef struct {
    jetkvm_control_storage_download_phase_t phase;
    char filename[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
    uint64_t bytes;
    uint64_t total;
    bool reused;
    bool video_paused;
    char error[JETKVM_CONTROL_STORAGE_DOWNLOAD_ERROR_CAPACITY];
} jetkvm_control_storage_download_state_t;

typedef struct {
    /** Start a background download; false with a reason when refused. */
    bool (*start)(const char *url, const char *filename, char *error,
                  size_t error_capacity, void *ctx);
    bool (*get_state)(jetkvm_control_storage_download_state_t *state,
                      void *ctx);
    void *ctx;
} jetkvm_storage_download_context_t;

typedef enum {
    /** A remote-wakeup signal was accepted by the USB controller. */
    JETKVM_USB_WAKE_SUCCEEDED = 0,
    /** The link is not in a state that needs waking (JetKVM-compatible no-op). */
    JETKVM_USB_WAKE_NOOP,
    /** The USB driver or wake adapter was not initialized. */
    JETKVM_USB_WAKE_UNAVAILABLE,
    /** The host suspended the bus without enabling DEVICE_REMOTE_WAKEUP. */
    JETKVM_USB_WAKE_NOT_ENABLED,
    /** Another bounded wake operation is already in progress. */
    JETKVM_USB_WAKE_BUSY,
    /** The USB controller rejected or failed the wake request. */
    JETKVM_USB_WAKE_FAILED,
} jetkvm_usb_wake_result_t;

/**
 * Request host wake without coupling the JSON-RPC layer to a USB driver.
 *
 * Implementations must return NOOP for an absent or active (not suspended)
 * host, and must not report SUCCEEDED unless the controller accepted a real
 * remote-wakeup signal.
 */
typedef jetkvm_usb_wake_result_t (*jetkvm_wake_host_fn)(void *ctx);

/** Send one Ethernet Wake-on-LAN magic packet. */
typedef bool (*jetkvm_send_wol_fn)(const uint8_t mac[6],
                                   const char *broadcast_ip, void *ctx);

/** One bounded, JetKVM-compatible saved Wake-on-LAN target. */
typedef struct {
    char name[JETKVM_CONTROL_WOL_NAME_CAPACITY];
    char mac_address[JETKVM_CONTROL_WOL_MAC_CAPACITY];
    char broadcast_ip[JETKVM_CONTROL_WOL_BROADCAST_CAPACITY];
} jetkvm_control_wol_device_t;

typedef struct {
    size_t count;
    jetkvm_control_wol_device_t devices[JETKVM_CONTROL_WOL_MAX_DEVICES];
} jetkvm_control_wol_device_list_t;

/** Copy or atomically replace the persistent saved-target list. */
typedef bool (*jetkvm_get_wol_devices_fn)(
    jetkvm_control_wol_device_list_t *list, void *ctx);
typedef bool (*jetkvm_set_wol_devices_fn)(
    const jetkvm_control_wol_device_list_t *list, void *ctx);

typedef struct {
    jetkvm_get_wol_devices_fn get;
    jetkvm_set_wol_devices_fn set;
    void *ctx;
} jetkvm_wol_devices_context_t;

/** Compact representation of one saved keyboard macro step.
 *
 * Key and modifier identifiers index the canonical frontend name tables. This
 * keeps the worst-case 25x10x10 list small enough for transactional NVS while
 * preserving the exact names returned to the browser.
 */
typedef struct {
    uint8_t key_count;
    uint8_t key_ids[JETKVM_CONTROL_KEYBOARD_MACRO_MAX_KEYS];
    uint8_t modifier_count;
    uint8_t modifier_ids[JETKVM_CONTROL_KEYBOARD_MACRO_MAX_MODIFIERS];
    uint16_t delay_ms;
} jetkvm_control_keyboard_macro_step_t;

typedef struct {
    char id[JETKVM_CONTROL_KEYBOARD_MACRO_ID_CAPACITY];
    char name[JETKVM_CONTROL_KEYBOARD_MACRO_NAME_CAPACITY];
    uint8_t step_count;
    jetkvm_control_keyboard_macro_step_t
        steps[JETKVM_CONTROL_KEYBOARD_MACRO_MAX_STEPS];
    int32_t sort_order;
} jetkvm_control_keyboard_macro_t;

typedef struct {
    size_t count;
    jetkvm_control_keyboard_macro_t
        macros[JETKVM_CONTROL_KEYBOARD_MACRO_MAX_COUNT];
} jetkvm_control_keyboard_macro_list_t;

typedef bool (*jetkvm_get_keyboard_macros_fn)(
    jetkvm_control_keyboard_macro_list_t *list, void *ctx);
typedef bool (*jetkvm_set_keyboard_macros_fn)(
    const jetkvm_control_keyboard_macro_list_t *list, void *ctx);

typedef struct {
    jetkvm_get_keyboard_macros_fn get;
    jetkvm_set_keyboard_macros_fn set;
    void *ctx;
} jetkvm_keyboard_macros_context_t;

typedef struct {
    uint32_t inactivity_limit_seconds;
    uint32_t jitter_percentage;
    char schedule_cron_tab[JETKVM_CONTROL_JIGGLER_CRON_CAPACITY];
    char timezone[JETKVM_CONTROL_JIGGLER_TIMEZONE_CAPACITY];
} jetkvm_control_jiggler_config_t;

typedef bool (*jetkvm_get_jiggler_state_fn)(bool *enabled, void *ctx);
typedef bool (*jetkvm_set_jiggler_state_fn)(bool enabled, void *ctx);
typedef bool (*jetkvm_get_jiggler_config_fn)(
    jetkvm_control_jiggler_config_t *config, void *ctx);
typedef enum {
    JETKVM_JIGGLER_CONFIG_CHANGE_ACCEPTED = 0,
    JETKVM_JIGGLER_CONFIG_CHANGE_INVALID,
    JETKVM_JIGGLER_CONFIG_CHANGE_FAILED,
} jetkvm_jiggler_config_change_result_t;
typedef jetkvm_jiggler_config_change_result_t
    (*jetkvm_set_jiggler_config_fn)(
    const jetkvm_control_jiggler_config_t *config, void *ctx);

typedef struct {
    jetkvm_get_jiggler_state_fn get_state;
    jetkvm_set_jiggler_state_fn set_state;
    jetkvm_get_jiggler_config_fn get_config;
    jetkvm_set_jiggler_config_fn set_config;
    void *ctx;
} jetkvm_jiggler_context_t;

typedef enum {
    JETKVM_VIDEO_STREAM_INACTIVE = 0,
    JETKVM_VIDEO_STREAM_ACTIVE,
    JETKVM_VIDEO_STREAM_STOPPING,
} jetkvm_video_streaming_t;

typedef struct {
    bool ready;
    jetkvm_video_streaming_t streaming;
    /* An upload to the TF card holds video off; reported as
     * uploadPausesVideo, only while true. */
    bool upload_pauses_video;
    const char *error;
    uint16_t width;
    uint16_t height;
    double fps;
} jetkvm_video_state_t;

/**
 * Render one caller-owned, human-readable snapshot for getVideoLogStatus.
 *
 * The Go implementation returns the capture driver's status log as a string.
 * Platforms without V4L2 may provide equivalent native diagnostics. The
 * callback must NUL-terminate the output and return false if the supplied
 * capacity cannot hold the complete snapshot.
 */
typedef bool (*jetkvm_get_video_log_status_fn)(
    char *output, size_t output_capacity, void *ctx);

typedef enum {
    JETKVM_QUALITY_CHANGE_ACCEPTED = 0,
    JETKVM_QUALITY_CHANGE_INVALID,
    JETKVM_QUALITY_CHANGE_BUSY,
    JETKVM_QUALITY_CHANGE_UNAVAILABLE,
} jetkvm_quality_change_result_t;

/**
 * Queue a quality change without touching the encoder from the WebRTC task.
 *
 * ACCEPTED means that an owning video-control task has durably accepted the
 * command. Implementations must not synchronously restart or reconfigure the
 * encoder from this callback.
 */
typedef jetkvm_quality_change_result_t (*jetkvm_queue_quality_change_fn)(
    double factor, void *ctx);

typedef enum {
    JETKVM_VIDEO_CODEC_CHANGE_ACCEPTED = 0,
    JETKVM_VIDEO_CODEC_CHANGE_INVALID,
    JETKVM_VIDEO_CODEC_CHANGE_UNSUPPORTED,
    JETKVM_VIDEO_CODEC_CHANGE_FAILED,
} jetkvm_video_codec_change_result_t;

/** Persist a codec preference; P4 supports auto and h264 only. */
typedef jetkvm_video_codec_change_result_t
    (*jetkvm_set_video_codec_preference_fn)(const char *codec, void *ctx);

/** Pause only encoded video delivery while keeping RPC/HID connected. */
typedef bool (*jetkvm_set_video_stream_paused_fn)(bool paused, void *ctx);

#define JETKVM_CONTROL_EDID_HEX_CAPACITY 513U

typedef enum {
    JETKVM_EDID_CHANGE_ACCEPTED = 0,
    JETKVM_EDID_CHANGE_INVALID,
    JETKVM_EDID_CHANGE_BUSY,
    JETKVM_EDID_CHANGE_UNAVAILABLE,
    JETKVM_EDID_CHANGE_FAILED,
} jetkvm_edid_change_result_t;

/** Copy the active EDID as 128 or 256 bytes of uppercase hexadecimal. */
typedef bool (*jetkvm_get_edid_fn)(char *output, size_t output_capacity,
                                   void *ctx);

/** Apply and persist an EDID before returning ACCEPTED. */
typedef jetkvm_edid_change_result_t (*jetkvm_set_edid_fn)(const char *edid,
                                                          void *ctx);

/** Enumerate presets by index. NULL name marks the end; false reports failure. */
typedef bool (*jetkvm_get_edid_preset_fn)(size_t index, const char **name,
                                        char *edid, size_t edid_capacity,
                                        void *ctx);

typedef struct {
    jetkvm_get_edid_preset_fn get_preset;
    jetkvm_get_edid_fn get;
    jetkvm_set_edid_fn set;
    void *ctx;
} jetkvm_edid_context_t;

typedef enum {
    JETKVM_HOST_DISPLAY_IDLE_CHANGE_ACCEPTED = 0,
    JETKVM_HOST_DISPLAY_IDLE_CHANGE_UNAVAILABLE,
    JETKVM_HOST_DISPLAY_IDLE_CHANGE_FAILED,
} jetkvm_host_display_idle_change_result_t;

typedef bool (*jetkvm_get_host_display_idle_fn)(bool *enabled, void *ctx);
typedef jetkvm_host_display_idle_change_result_t
    (*jetkvm_set_host_display_idle_fn)(bool enabled, void *ctx);

typedef struct {
    jetkvm_get_host_display_idle_fn get;
    jetkvm_set_host_display_idle_fn set;
    void *ctx;
} jetkvm_host_display_idle_context_t;

typedef enum {
    JETKVM_VIDEO_SLEEP_CHANGE_ACCEPTED = 0,
    JETKVM_VIDEO_SLEEP_CHANGE_INVALID,
    JETKVM_VIDEO_SLEEP_CHANGE_UNAVAILABLE,
    JETKVM_VIDEO_SLEEP_CHANGE_FAILED,
} jetkvm_video_sleep_change_result_t;

/** Return the actual sleep state and the persisted inactivity duration. */
typedef bool (*jetkvm_get_video_sleep_mode_fn)(bool *enabled,
                                               int32_t *duration_seconds,
                                               void *ctx);
typedef jetkvm_video_sleep_change_result_t
    (*jetkvm_set_video_sleep_mode_fn)(int32_t duration_seconds, void *ctx);

typedef struct {
    jetkvm_get_video_sleep_mode_fn get;
    jetkvm_set_video_sleep_mode_fn set;
    void *ctx;
} jetkvm_video_sleep_context_t;

#define JETKVM_CONTROL_MQTT_BROKER_CAPACITY 256U
#define JETKVM_CONTROL_MQTT_USERNAME_CAPACITY 128U
#define JETKVM_CONTROL_MQTT_PASSWORD_CAPACITY 128U
#define JETKVM_CONTROL_MQTT_BASE_TOPIC_CAPACITY 128U
#define JETKVM_CONTROL_MQTT_ERROR_CAPACITY 192U

/** MQTT settings use the exact snake_case JSON shape exposed by Go. */
typedef struct {
    bool enabled;
    char broker[JETKVM_CONTROL_MQTT_BROKER_CAPACITY];
    int32_t port;
    char username[JETKVM_CONTROL_MQTT_USERNAME_CAPACITY];
    char password[JETKVM_CONTROL_MQTT_PASSWORD_CAPACITY];
    char base_topic[JETKVM_CONTROL_MQTT_BASE_TOPIC_CAPACITY];
    bool use_tls;
    bool tls_insecure;
    bool enable_ha_discovery;
    bool enable_actions;
    int32_t debounce_ms;
} jetkvm_control_mqtt_settings_t;

typedef struct {
    bool connected;
    char error[JETKVM_CONTROL_MQTT_ERROR_CAPACITY];
} jetkvm_control_mqtt_status_t;

typedef struct {
    bool success;
    char error[JETKVM_CONTROL_MQTT_ERROR_CAPACITY];
} jetkvm_control_mqtt_test_result_t;

typedef enum {
    JETKVM_MQTT_CONFIG_CHANGE_ACCEPTED = 0,
    JETKVM_MQTT_CONFIG_CHANGE_INVALID,
    JETKVM_MQTT_CONFIG_CHANGE_UNAVAILABLE,
    JETKVM_MQTT_CONFIG_CHANGE_FAILED,
} jetkvm_mqtt_config_change_result_t;

typedef bool (*jetkvm_get_mqtt_settings_fn)(
    jetkvm_control_mqtt_settings_t *settings, void *ctx);
typedef jetkvm_mqtt_config_change_result_t (*jetkvm_set_mqtt_settings_fn)(
    const jetkvm_control_mqtt_settings_t *settings,
    char *error, size_t error_capacity, void *ctx);
typedef bool (*jetkvm_get_mqtt_status_fn)(
    jetkvm_control_mqtt_status_t *status, void *ctx);
typedef bool (*jetkvm_test_mqtt_connection_fn)(
    const jetkvm_control_mqtt_settings_t *settings,
    jetkvm_control_mqtt_test_result_t *result, void *ctx);

typedef struct {
    jetkvm_get_mqtt_settings_fn get_settings;
    jetkvm_set_mqtt_settings_fn set_settings;
    jetkvm_get_mqtt_status_fn get_status;
    jetkvm_test_mqtt_connection_fn test_connection;
    void *ctx;
} jetkvm_mqtt_context_t;

#define JETKVM_CONTROL_OTA_VERSION_CAPACITY 32U
#define JETKVM_CONTROL_OTA_URL_CAPACITY 256U
#define JETKVM_CONTROL_OTA_HASH_CAPACITY 65U
#define JETKVM_CONTROL_OTA_ERROR_CAPACITY 232U
#define JETKVM_CONTROL_OTA_API_URL_CAPACITY 192U

/** Transport-neutral view of the Go getUpdateStatus result. */
typedef struct {
    char local_app_version[JETKVM_CONTROL_OTA_VERSION_CAPACITY];
    char local_system_version[JETKVM_CONTROL_OTA_VERSION_CAPACITY];
    char remote_app_version[JETKVM_CONTROL_OTA_VERSION_CAPACITY];
    char remote_app_url[JETKVM_CONTROL_OTA_URL_CAPACITY];
    char remote_app_hash[JETKVM_CONTROL_OTA_HASH_CAPACITY];
    char remote_system_version[JETKVM_CONTROL_OTA_VERSION_CAPACITY];
    char remote_system_url[JETKVM_CONTROL_OTA_URL_CAPACITY];
    char remote_system_hash[JETKVM_CONTROL_OTA_HASH_CAPACITY];
    bool app_update_available;
    bool system_update_available;
    bool will_disable_auto_update;
    char error[JETKVM_CONTROL_OTA_ERROR_CAPACITY];
} jetkvm_control_ota_status_t;

typedef bool (*jetkvm_ota_get_update_status_fn)(
    jetkvm_control_ota_status_t *status, void *ctx);
typedef bool (*jetkvm_ota_check_update_fn)(
    const char *app_constraint, const char *system_constraint,
    bool include_prerelease, jetkvm_control_ota_status_t *status, void *ctx);
typedef bool (*jetkvm_ota_try_update_fn)(
    const char *app_constraint, const char *system_constraint,
    bool include_prerelease, bool reset_config, char *error,
    size_t error_capacity, void *ctx);
typedef bool (*jetkvm_ota_get_auto_update_fn)(void *ctx);
typedef bool (*jetkvm_ota_set_auto_update_fn)(bool enabled, void *ctx);
typedef bool (*jetkvm_ota_get_dev_channel_fn)(void *ctx);
typedef bool (*jetkvm_ota_set_dev_channel_fn)(bool enabled, void *ctx);
typedef bool (*jetkvm_ota_is_update_pending_fn)(void *ctx);
typedef bool (*jetkvm_ota_get_update_api_url_fn)(char *url, size_t capacity,
                                                 void *ctx);
typedef bool (*jetkvm_ota_set_update_api_url_fn)(const char *url, char *error,
                                                 size_t error_capacity,
                                                 void *ctx);

typedef struct {
    jetkvm_ota_get_update_status_fn get_update_status;
    jetkvm_ota_check_update_fn check_update;
    jetkvm_ota_try_update_fn try_update;
    jetkvm_ota_get_auto_update_fn get_auto_update;
    jetkvm_ota_set_auto_update_fn set_auto_update;
    /** Dev channel: include prerelease versions in update checks. */
    jetkvm_ota_get_dev_channel_fn get_dev_channel;
    jetkvm_ota_set_dev_channel_fn set_dev_channel;
    jetkvm_ota_is_update_pending_fn is_update_pending;
    jetkvm_ota_get_update_api_url_fn get_update_api_url;
    jetkvm_ota_set_update_api_url_fn set_update_api_url;
    void *ctx;
} jetkvm_ota_context_t;

typedef enum {
    JETKVM_CLOUD_CONFIG_CHANGE_ACCEPTED = 0,
    JETKVM_CLOUD_CONFIG_CHANGE_INVALID,
    JETKVM_CLOUD_CONFIG_CHANGE_FAILED,
} jetkvm_cloud_config_change_result_t;

/** Persist cloud URLs before returning ACCEPTED. No secret is carried here. */
typedef jetkvm_cloud_config_change_result_t (*jetkvm_set_cloud_urls_fn)(
    const char *cloud_url, const char *cloud_app_url, void *ctx);

typedef enum {
    JETKVM_KEYBOARD_LAYOUT_CHANGE_ACCEPTED = 0,
    JETKVM_KEYBOARD_LAYOUT_CHANGE_INVALID,
    JETKVM_KEYBOARD_LAYOUT_CHANGE_FAILED,
} jetkvm_keyboard_layout_change_result_t;

/** Copy a canonical layout tag into the supplied caller-owned buffer. */
typedef bool (*jetkvm_get_keyboard_layout_fn)(
    char *output, size_t output_capacity, void *ctx);

/** Persist the layout before returning ACCEPTED. */
typedef jetkvm_keyboard_layout_change_result_t
    (*jetkvm_set_keyboard_layout_fn)(const char *layout, void *ctx);

typedef struct {
    jetkvm_get_keyboard_layout_fn get;
    jetkvm_set_keyboard_layout_fn set;
    void *ctx;
} jetkvm_keyboard_layout_context_t;

typedef enum {
    /** A reboot owner atomically reserved its one pending request slot. */
    JETKVM_REBOOT_REQUEST_ACCEPTED = 0,
    /** A previously accepted reboot has not executed yet. */
    JETKVM_REBOOT_REQUEST_BUSY,
    /** The reboot worker/transport boundary is not ready. */
    JETKVM_REBOOT_REQUEST_UNAVAILABLE,
} jetkvm_reboot_request_result_t;

/**
 * Reserve a reboot request without sleeping or restarting in the RPC/peer
 * callback.  ACCEPTED transfers execution to an asynchronous owner.
 */
typedef jetkvm_reboot_request_result_t (*jetkvm_request_reboot_fn)(
    bool force, void *ctx);

typedef enum {
    JETKVM_WIFI_BAND_AUTO = 0,
    JETKVM_WIFI_BAND_2G = 1,
    JETKVM_WIFI_BAND_5G = 2,
} jetkvm_wifi_band_t;

#define JETKVM_WIFI_MAX_NETWORKS 8U
#define JETKVM_WIFI_MAX_SCAN_RESULTS 32U
#define JETKVM_WIFI_SSID_CAPACITY 33U
#define JETKVM_WIFI_PASSWORD_CAPACITY 65U
#define JETKVM_WIFI_ERROR_CAPACITY 64U

typedef struct {
    char ssid[JETKVM_WIFI_SSID_CAPACITY];
    bool connected;
    bool secure; /* saved with a password */
} jetkvm_wifi_saved_network_t;

typedef struct {
    bool available, connected, applying, apply_failed;
    /** BLE provisioning is running because no saved network connected. */
    bool provisioning;
    /** A connect request is about to leave the current network. */
    bool switching;
    /** The saved network the static IPv4 settings apply to ("" for DHCP). */
    char static_ssid[JETKVM_WIFI_SSID_CAPACITY];
    jetkvm_wifi_band_t band;
    uint8_t channel;
    int8_t rssi;
    char ssid[JETKVM_WIFI_SSID_CAPACITY];
    /** Why the last connect-now request failed; empty after a success.
     *  `applying` covers both a band change and a connect-now request. */
    char connect_error[JETKVM_WIFI_ERROR_CAPACITY];
    uint8_t network_count;
    jetkvm_wifi_saved_network_t networks[JETKVM_WIFI_MAX_NETWORKS];
} jetkvm_wifi_settings_t;

typedef struct {
    char ssid[JETKVM_WIFI_SSID_CAPACITY];
    int8_t rssi;
    uint8_t channel;
    bool secure, saved;
} jetkvm_wifi_scan_entry_t;

/** Cached results of the last scan, one entry per SSID, strongest first. */
typedef struct {
    bool scanning;
    uint8_t count;
    jetkvm_wifi_scan_entry_t entries[JETKVM_WIFI_MAX_SCAN_RESULTS];
} jetkvm_wifi_scan_t;

typedef struct {
    char ssid[JETKVM_WIFI_SSID_CAPACITY];
    char password[JETKVM_WIFI_PASSWORD_CAPACITY];
    /** Switch to the network now; the device returns to the previous one on failure. */
    bool connect;
} jetkvm_wifi_network_request_t;

#define JETKVM_NETWORK_HOSTNAME_CAPACITY 64U
#define JETKVM_NETWORK_MAC_CAPACITY 18U
#define JETKVM_NETWORK_IPV4_CAPACITY 16U
#define JETKVM_NETWORK_TIMESTAMP_CAPACITY 32U
#define JETKVM_TIME_SYNC_MAX_SERVERS 3U
#define JETKVM_TIME_SYNC_SERVER_CAPACITY 64U

typedef struct {
    char hostname[JETKVM_NETWORK_HOSTNAME_CAPACITY];
    bool dhcp;
    char address[16];
    char netmask[16];
    char gateway[16];
    char dns[3][16];
    /* Time sync follows the upstream fields the shared form submits. Appended
     * after the original persisted layout so older blobs still load. */
    bool time_sync_custom;
    bool time_sync_disable_fallback;
    char time_sync_ntp_servers[JETKVM_TIME_SYNC_MAX_SERVERS][JETKVM_TIME_SYNC_SERVER_CAPACITY];
} jetkvm_network_settings_t;

/** The MAC address setting (getMacAddressSettings). */
typedef struct {
    /** A valid factory MAC is in eFuse and the network interface follows the setting. */
    bool available;
    /** The saved choice: the Espressif MAC instead of the factory MAC ("Default"). */
    bool espressif;
    char default_mac[JETKVM_NETWORK_MAC_CAPACITY];
    char espressif_mac[JETKVM_NETWORK_MAC_CAPACITY];
} jetkvm_mac_address_settings_t;

/** Snapshot of the active Ethernet or Wi-Fi interface. */
typedef struct {
    char hostname[JETKVM_NETWORK_HOSTNAME_CAPACITY];
    char interface_name[8];
    char mac_address[JETKVM_NETWORK_MAC_CAPACITY];
    char ipv4_address[JETKVM_NETWORK_IPV4_CAPACITY];
    char netmask[JETKVM_NETWORK_IPV4_CAPACITY];
    char gateway[JETKVM_NETWORK_IPV4_CAPACITY];
    char dns_main[JETKVM_NETWORK_IPV4_CAPACITY];
    char dns_backup[JETKVM_NETWORK_IPV4_CAPACITY];
    char dns_fallback[JETKVM_NETWORK_IPV4_CAPACITY];
    char last_updated[JETKVM_NETWORK_TIMESTAMP_CAPACITY];
    bool up;
    bool online;
    bool ipv4_ready;
    bool dhcp;
    bool lease_valid;
    char lease_server[16];
    char lease_expiry[JETKVM_NETWORK_TIMESTAMP_CAPACITY];
    unsigned lease_seconds;
} jetkvm_network_state_t;

typedef struct {
    uint32_t max_brightness, dim_after, off_after;
} jetkvm_backlight_settings_t;
typedef struct {
    bool (*get_backlight)(jetkvm_backlight_settings_t *settings, void *ctx);
    const char *(*set_backlight)(const jetkvm_backlight_settings_t *settings, void *ctx);
    bool (*get_rotation)(uint16_t *rotation, void *ctx);
    const char *(*set_rotation)(uint16_t rotation, void *ctx);
    /* Side buttons drive the local display and wake it. */
    bool buttons;
    void *ctx;
} jetkvm_local_display_context_t;
void jetkvm_control_set_default_local_display_context(const jetkvm_local_display_context_t *context);

/* USB debugging (components/jetkvm_usb_debugging). get gives the stored
 * setting and whether the USB Serial/JTAG port is connected now; it returns
 * false when the setting cannot be read. set stores the setting and returns
 * NULL, or the reason it could not. Off and on take effect at the next
 * restart. A registered context adds the "usb_debugging" capability. */
typedef struct {
    bool (*get)(bool *enabled, bool *active, void *ctx);
    const char *(*set)(bool enabled, void *ctx);
    void *ctx;
} jetkvm_usb_debugging_context_t;
void jetkvm_control_set_default_usb_debugging_context(const jetkvm_usb_debugging_context_t *context);

/* Certificates are public; get never exposes private key material. */
typedef struct {
    bool (*get)(char *mode, size_t mode_capacity, char *certificate, size_t capacity, void *ctx);
    bool (*set)(const char *mode, const char *certificate, const char *private_key,
                char *error, size_t capacity, void *ctx);
    void *ctx;
} jetkvm_tls_context_t;
void jetkvm_control_set_default_tls_context(const jetkvm_tls_context_t *context);

/* Other names for the device that its web server answers to: get fills up
 * to `capacity` names and returns their count; set takes the whole list and
 * returns NULL, or why it took none. */
#define JETKVM_EXTRA_HOSTNAMES_MAX 8U
#define JETKVM_EXTRA_HOSTNAME_CAPACITY 128U
typedef struct {
    size_t (*get)(char (*names)[JETKVM_EXTRA_HOSTNAME_CAPACITY], size_t capacity, void *ctx);
    const char *(*set)(const char *const *names, size_t count, void *ctx);
    void *ctx;
} jetkvm_extra_hostnames_context_t;
void jetkvm_control_set_default_extra_hostnames_context(const jetkvm_extra_hostnames_context_t *context);

#define JETKVM_TAILSCALE_CONTROL_URL_CAPACITY 128U
#define JETKVM_TAILSCALE_AUTH_KEY_CAPACITY 128U
typedef struct {
    bool enabled;
    char control_url[JETKVM_TAILSCALE_CONTROL_URL_CAPACITY];
    char auth_key[JETKVM_TAILSCALE_AUTH_KEY_CAPACITY];
} jetkvm_tailscale_config_t;
typedef struct {
    bool installed, enabled, running, auth_key_set, logged_in;
    char backend_state[32], auth_url[512], health[256];
    char control_url[JETKVM_TAILSCALE_CONTROL_URL_CAPACITY];
    char hostname[64], dns_name[128], ipv4[16];
} jetkvm_tailscale_status_t;
typedef struct {
    bool (*get_status)(jetkvm_tailscale_status_t *status, void *ctx);
    bool (*get_config)(jetkvm_tailscale_config_t *config, void *ctx);
    const char *(*set_config)(const jetkvm_tailscale_config_t *config, void *ctx);
    /* Stops Tailscale and erases the node identity and the auth key, so that
     * the next enable starts a new login. */
    const char *(*logout)(void *ctx);
    void *ctx;
} jetkvm_tailscale_context_t;
void jetkvm_control_set_default_tailscale_context(const jetkvm_tailscale_context_t *context);
bool jetkvm_control_render_network_notification(const jetkvm_network_state_t *state,
    char *output, size_t capacity, size_t *length);
/** The localVersion and deviceCapabilities events, sent once each time the
 *  rpc channel opens; params match the getLocalVersion and
 *  getDeviceCapabilities results. */
bool jetkvm_control_render_local_version_notification(
    const char *app_version, const char *system_version, const char *sku,
    char *output, size_t capacity, size_t *length);

/* cJSON's header is not on every includer's path. The OS Services callbacks
 * only pass its node type through, so they name it here. */
typedef struct cJSON cJSON;

/* The longest text an OS Services clipboard request carries. */
#define JETKVM_GUEST_CLIPBOARD_MAX_TEXT 65528U

/* OS Services clipboard through the OS Services sender. Each call blocks
 * until the sender answers or about 2.5 seconds pass, so only the RPC
 * worker installs them. Each returns NULL with *status set ("ok",
 * "unsupported", "empty", "denied", "failed") and, for get, the text in
 * text[0..*length) and *truncated; or the reason the request got no
 * answer, which becomes the JSON-RPC error's data.
 *
 * get's capacity is JETKVM_GUEST_CLIPBOARD_MAX_TEXT + 1; set's text is
 * valid UTF-8 of at most JETKVM_GUEST_CLIPBOARD_MAX_TEXT bytes. They run on
 * the worker's 8 KiB PSRAM stack: no flash writes and no large locals. The
 * reason is copied, so it need only outlive the call. */
typedef struct {
    const char *(*get)(char *text, size_t capacity, size_t *length,
                       bool *truncated, const char **status, void *ctx);
    const char *(*set)(const char *text, size_t length, const char **status,
                       void *ctx);
    void *ctx;
} jetkvm_guest_clipboard_context_t;

/* OS Services file drops through the OS Services sender. drop offers the
 * files of a guestFileDrop: an array the dispatcher has checked, of objects
 * with a string "name" and a whole "size" from 0 to 2^53. It waits until the
 * sender's file.drop is written (about 2.5 seconds at most), so only the RPC
 * worker installs it, and it runs on the worker's 8 KiB PSRAM stack. It adds
 * "channel" (the data channel the browser opens next) and "transfers" (one
 * ID per file) to result and returns NULL, or returns why no drop started;
 * the reason is copied, so it need only outlive the call. */
typedef struct {
    const char *(*drop)(const cJSON *files, cJSON *result, void *ctx);
    void *ctx;
} jetkvm_guest_file_context_t;

/* OS Services settings and state from the OS Services component. */
typedef struct {
    /* Adds the settings' fields to object. */
    bool (*get_settings)(cJSON *object, void *ctx);
    /* Lays request (the "settings" object) over the current settings,
     * saves and applies them, and adds the applied settings to applied.
     * Returns NULL, or why the settings were refused. Writes flash: call
     * only on an internal-RAM stack (the peer task's inline dispatch is). */
    const char *(*set_settings)(const cJSON *request, cJSON *applied, void *ctx);
    /* Adds "source", "osServices" and any "guestClipboard",
     * "guestTerminal", "guestFile", "guestUpdate" fields to a video state
     * object. The peer task calls it for getVideoState and for every
     * videoInputState event, so it must not block; the event, these fields
     * included, has 1280 bytes. */
    bool (*add_video_state)(cJSON *object, void *ctx);
    /* guestUpdateStart: installs the newest OS Services release on the
     * connected sender. Returns NULL, or why it cannot start. Does not
     * block. */
    const char *(*update_start)(void *ctx);
    void *ctx;
} jetkvm_os_services_context_t;

/** Snapshot and non-blocking command boundary used by the JSON-RPC layer. */
typedef struct {
    jetkvm_video_state_t video;
    const char *app_version;
    const char *system_version;
    /** Hardware SKU, returned by getLocalVersion; the UI builds the cloud UI
     *  address from it after a reboot. */
    const char *sku;
    const char *device_id;
    /** Used by getNetworkState only when `snapshot_network` is NULL. */
    jetkvm_network_state_t network;
    /** Fills the network state on demand. Set by hosts whose snapshot is
     *  expensive (a lock plus an lwIP callback for the DHCP lease) so that
     *  only getNetworkState pays for it, not every RPC frame. */
    void (*snapshot_network)(jetkvm_network_state_t *state);
    jetkvm_network_settings_t network_settings;
    jetkvm_tailscale_context_t tailscale;
    bool (*get_wifi_settings)(jetkvm_wifi_settings_t *settings, void *ctx);
    const char *(*set_wifi_band)(jetkvm_wifi_band_t band, void *ctx);
    /** Copy cached scan results; `refresh` starts a new scan in the background. */
    bool (*scan_wifi_networks)(bool refresh, jetkvm_wifi_scan_t *scan, void *ctx);
    /** `connecting` reports whether the device will switch to the network to
     *  test it (a connect request, or a new password for the current one). */
    const char *(*save_wifi_network)(const jetkvm_wifi_network_request_t *request,
                                     bool *connecting, void *ctx);
    const char *(*remove_wifi_network)(const char *ssid, void *ctx);
    /** Join a saved network now; the device returns to a saved network on failure. */
    const char *(*connect_wifi_network)(const char *ssid, void *ctx);
    bool (*get_network_settings)(jetkvm_network_settings_t *settings, void *ctx);
    const char *(*set_network_settings)(const jetkvm_network_settings_t *settings, void *ctx);
    const char *(*renew_dhcp_lease)(void *ctx);
    /** The MAC address setting; it takes effect at the next boot. */
    bool (*get_mac_address_settings)(jetkvm_mac_address_settings_t *settings, void *ctx);
    const char *(*set_mac_address_settings)(bool espressif, void *ctx);
    /** Copies the last public IPv4 address seen through `cloud_url` (empty
     *  before the first lookup), looking it up first when `refresh`. Returns
     *  an error message, or NULL. */
    const char *(*get_public_ip)(bool refresh, const char *cloud_url,
                                 char *ip, size_t ip_capacity,
                                 int64_t *last_updated, void *ctx);
    bool cloud_registered;
    const char *cloud_url;
    const char *cloud_app_url;
    double stream_quality_factor;
    jetkvm_get_video_log_status_fn get_video_log_status;
    void *video_log_status_ctx;
    jetkvm_queue_quality_change_fn queue_quality_change;
    void *queue_quality_change_ctx;
    const char *video_codec_preference;
    jetkvm_set_video_codec_preference_fn set_video_codec_preference;
    void *set_video_codec_preference_ctx;
    jetkvm_set_video_stream_paused_fn set_video_stream_paused;
    void *set_video_stream_paused_ctx;
    jetkvm_edid_context_t edid;
    jetkvm_host_display_idle_context_t host_display_idle;
    jetkvm_video_sleep_context_t video_sleep;
    jetkvm_mqtt_context_t mqtt;
    jetkvm_ota_context_t ota;
    jetkvm_tls_context_t tls;
    /** Runs on a worker; NULL means unavailable on this dispatch path. */
    const char *(*factory_reset)(void *ctx);
    void *factory_reset_ctx;
    const char *(*deregister_device)(void *ctx);
    void *deregister_device_ctx;
    jetkvm_set_cloud_urls_fn set_cloud_urls;
    void *set_cloud_urls_ctx;
    jetkvm_keyboard_layout_context_t keyboard_layout;
    jetkvm_request_reboot_fn request_reboot;
    void *request_reboot_ctx;
    jetkvm_hidrpc_context_t hidrpc;
    jetkvm_get_keyboard_led_state_fn get_keyboard_led_state;
    jetkvm_get_key_down_state_fn get_key_down_state;
    jetkvm_get_usb_state_fn get_usb_state;
    jetkvm_usb_configuration_context_t usb_configuration;
    jetkvm_virtual_media_context_t virtual_media;
    jetkvm_storage_download_context_t storage_download;
    jetkvm_wake_host_fn wake_host;
    jetkvm_send_wol_fn send_wol;
    jetkvm_wol_devices_context_t wol_devices;
    jetkvm_keyboard_macros_context_t keyboard_macros;
    jetkvm_jiggler_context_t jiggler;
    /** Per request: the RPC worker installs the default clipboard context;
     *  the peer task's inline dispatch leaves it zeroed or installs
     *  callbacks that only report why the worker refused the request. */
    jetkvm_guest_clipboard_context_t guest_clipboard;
    /** Per request, as guest_clipboard. */
    jetkvm_guest_file_context_t guest_file;
    void *network_ctx;
    void *hid_state_ctx;
} jetkvm_control_context_t;

/** `context` supplies the providers the capability list is derived from;
 *  NULL reports only what the defaults provide. */
bool jetkvm_control_render_capabilities_notification(
    const jetkvm_control_context_t *context,
    char *output, size_t capacity, size_t *length);

/**
 * Install process-wide keyboard-layout callbacks before RPC service startup.
 *
 * A per-request `jetkvm_control_context_t.keyboard_layout` takes precedence.
 * This default lets application composition wire persistent settings without
 * coupling the control protocol to a concrete storage component.  Do not
 * replace it while requests are in flight.
 */
void jetkvm_control_set_default_keyboard_layout_context(
    const jetkvm_keyboard_layout_context_t *context);

/** Install application-owned EDID callbacks before the RPC service starts. */
void jetkvm_control_set_default_edid_context(
    const jetkvm_edid_context_t *context);

/** Install persistent host-display idle callbacks before RPC service startup. */
void jetkvm_control_set_default_host_display_idle_context(
    const jetkvm_host_display_idle_context_t *context);

typedef struct {
    bool (*get_enabled)(bool *enabled, void *ctx);
    const char *(*set_enabled)(bool enabled, void *ctx);
    void *ctx;
} jetkvm_audio_context_t;
void jetkvm_control_set_default_audio_context(const jetkvm_audio_context_t *context);

/** Install HDMI power-saving callbacks before RPC service startup. */
void jetkvm_control_set_default_video_sleep_context(
    const jetkvm_video_sleep_context_t *context);

/** Install the persistent MQTT service callbacks before RPC startup. */
void jetkvm_control_set_default_mqtt_context(
    const jetkvm_mqtt_context_t *context);
/* The default MQTT context, for the RPC worker's peer fallbacks, which
 * replace only the connection test. Zeroed when none was set. */
jetkvm_mqtt_context_t jetkvm_control_default_mqtt_context(void);

/** Install the OTA update service callbacks before RPC startup. */
void jetkvm_control_set_default_ota_context(
    const jetkvm_ota_context_t *context);
/* The default OTA context, for the RPC worker's peer fallbacks, which
 * replace only the update checks. Zeroed when none was set. */
jetkvm_ota_context_t jetkvm_control_default_ota_context(void);

/** Install the TF-card download callbacks before RPC startup. */
void jetkvm_control_set_default_storage_download_context(
    const jetkvm_storage_download_context_t *context);

/** Render the storageDownloadState event; its params are what
 *  getStorageDownloadState returns. */
bool jetkvm_control_render_storage_download_event(
    const jetkvm_control_storage_download_state_t *state, char *output,
    size_t capacity, size_t *length);

/** Install the OS Services callbacks before RPC startup.
 *  getOsServicesSettings, setOsServicesSettings, getVideoState, the
 *  "os_services" capability and the videoInputState event use them; a
 *  reason a callback returns is copied, so it need only outlive the call. */
void jetkvm_control_set_default_os_services_context(
    const jetkvm_os_services_context_t *context);
/** Install the OS Services clipboard callbacks before RPC startup. */
void jetkvm_control_set_default_guest_clipboard_context(
    const jetkvm_guest_clipboard_context_t *context);
/* The default clipboard context, for the RPC worker to install in its
 * per-request context. Zeroed when none was set. */
jetkvm_guest_clipboard_context_t jetkvm_control_default_guest_clipboard_context(void);
/** Install the OS Services file drop callback before RPC startup. */
void jetkvm_control_set_default_guest_file_context(
    const jetkvm_guest_file_context_t *context);
/* The default file drop context, for the RPC worker to install in its
 * per-request context. Zeroed when none was set. */
jetkvm_guest_file_context_t jetkvm_control_default_guest_file_context(void);

/* The OS Services fields of videoInputState as JSON object members with a
 * leading comma (`,"source":"physical","osServices":{...}`), to place
 * before the closing brace of the event's params; *length is 0 when no OS
 * Services context is set. Returns false when they do not fit. */
bool jetkvm_control_render_guest_video_fields(char *output, size_t capacity,
                                             size_t *length);

/** Handle the JetKVM HIDRPC v1 handshake without dispatching input events. */
jetkvm_control_result_t jetkvm_control_handle_hidrpc(
    const uint8_t *input, size_t input_size,
    uint8_t *output, size_t output_capacity, size_t *output_size);

/**
 * Validate and dispatch the JetKVM HIDRPC v1 input message.
 *
 * Handshake produces RESPONSE_READY. A validated input report delivered to
 * on_event produces EVENT_HANDLED and no wire response. Unknown message types
 * produce NO_RESPONSE; known types with a non-canonical length or boolean byte
 * produce MALFORMED_INPUT.
 */
jetkvm_control_result_t jetkvm_control_handle_hidrpc_with_context(
    const jetkvm_hidrpc_context_t *context,
    const uint8_t *input, size_t input_size,
    uint8_t *output, size_t output_capacity, size_t *output_size);

/** Decode one step from a validated keyboard-macro event view. */
bool jetkvm_hidrpc_macro_step_at(
    const jetkvm_hidrpc_keyboard_macro_report_t *macro, uint32_t index,
    jetkvm_hidrpc_macro_step_t *step);

/**
 * Handle the video-stage subset of the JetKVM JSON-RPC 2.0 contract.
 *
 * Parse/invalid-request/method-not-found responses use the standard JSON-RPC
 * error codes. Notifications (requests without id) intentionally produce no
 * response. HID input/state methods use only the callbacks supplied in the
 * context and never call USB hardware directly.
 */
jetkvm_control_result_t jetkvm_control_handle_rpc_with_context(
    const jetkvm_control_context_t *context,
    const uint8_t *input, size_t input_size,
    char *output, size_t output_capacity, size_t *output_size);

/** Handle RPC with conservative P4 defaults, primarily for host probes. */
jetkvm_control_result_t jetkvm_control_handle_rpc(
    const uint8_t *input, size_t input_size,
    char *output, size_t output_capacity, size_t *output_size);

#ifdef __cplusplus
}
#endif

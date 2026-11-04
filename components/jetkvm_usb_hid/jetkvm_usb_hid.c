/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_usb_hid.h"
#include "jetkvm_audio.h"
#include "usb_audio_descriptor.h"
#include "jetkvm_virtual_media.h"
#include "cdrom_toc.h"
#include "keyboard_rollover.h"

#include <stddef.h>
#include <string.h>

#include "class/hid/hid_device.h"
#include "class/msc/msc_device.h"
#include "device/dcd.h"
#include "device/usbd_pvt.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#if !CONFIG_IDF_TARGET_ESP32P4
#error "jetkvm_usb_hid is intentionally restricted to ESP32-P4"
#endif

#if SOC_USB_OTG_PERIPH_NUM < 2
#error "jetkvm_usb_hid requires the ESP32-P4 dedicated USB2 HS controller"
#endif

#if !defined(CONFIG_TINYUSB_HID_COUNT) || CONFIG_TINYUSB_HID_COUNT != 4
#error "jetkvm_usb_hid requires CONFIG_TINYUSB_HID_COUNT=4"
#endif

#if CONFIG_TINYUSB_CDC_ENABLED
#error "jetkvm_usb_hid first-stage firmware must not enable TinyUSB CDC"
#endif

#if !CONFIG_TINYUSB_MSC_ENABLED || !CONFIG_TINYUSB_MSC_CUSTOM_BACKEND
#error "jetkvm_usb_hid requires the custom image-backed TinyUSB MSC backend"
#endif

/* The build keeps the independent USB Serial/JTAG port and its console, and
 * the USB debugging setting (components/jetkvm_usb_debugging) disconnects
 * the port at each start while it is off. */
#if !defined(CONFIG_USJ_ENABLE_USB_SERIAL_JTAG) || !CONFIG_USJ_ENABLE_USB_SERIAL_JTAG
#error "jetkvm_usb_hid requires USB Serial/JTAG in the build, so that the USB debugging setting can turn it on"
#endif

#if !defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED) || \
    !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
#error "jetkvm_usb_hid requires the USB Serial/JTAG console path, which USB debugging turns on"
#endif

_Static_assert(TINYUSB_PORT_HIGH_SPEED_0 != TINYUSB_PORT_FULL_SPEED_0,
               "ESP32-P4 HS and FS TinyUSB ports must remain distinct");

/* The MSC read worker runs esp_http_client and TLS. On CPU0 below TinyUSB's
 * task (5) and the OS Services task (4), so its decryption takes only the
 * time they leave. */
#define MSC_READER_STACK_BYTES 8192U
#define MSC_READER_PRIORITY 3U

static const char *TAG = "jetkvm_usb_hid";

typedef enum {
    HID_FUNCTION_KEYBOARD = 0,
    HID_FUNCTION_ABSOLUTE_MOUSE,
    HID_FUNCTION_RELATIVE_MOUSE,
    HID_FUNCTION_WAKE,
    HID_FUNCTION_COUNT,
} hid_function_t;

enum {
    HID_INSTANCE_CAPACITY = HID_FUNCTION_COUNT,
    DESCRIPTOR_BANK_COUNT = 2,
};

enum {
    STRING_LANGID = 0,
    STRING_MANUFACTURER,
    STRING_PRODUCT,
    STRING_SERIAL,
    STRING_KEYBOARD,
    STRING_ABSOLUTE_MOUSE,
    STRING_RELATIVE_MOUSE,
    STRING_MSC_CDROM,
    STRING_COUNT,
};
_Static_assert(STRING_COUNT <= 8, "esp_tinyusb accepts at most 8 strings");

enum {
    ENDPOINT_KEYBOARD = 1,
    ENDPOINT_ABSOLUTE_MOUSE,
    ENDPOINT_RELATIVE_MOUSE,
    ENDPOINT_WAKE,
    ENDPOINT_MSC,
    ENDPOINT_AUDIO,
    ENDPOINT_OS_SERVICES,
};

enum {
    ABSOLUTE_REPORT_ID_POINTER = 1,
    ABSOLUTE_REPORT_ID_WHEEL = 2,
};

#define HID_ENDPOINT_SIZE 8U
#define HID_FS_POLL_INTERVAL 1U
/* At high speed, bInterval=4 requests 2^(4-1) microframes = 1 ms. */
#define HID_HS_POLL_INTERVAL 4U
#define HID_REPORT_DEADLINE_MS 20U

/* Keep this formula identical to esp_tinyusb's default device descriptor. */
#define JETKVM_USB_HID_DEFAULT_PRODUCT_ID                                  \
    (0x4000U | ((uint16_t)CFG_TUD_CDC << 0U) |                             \
     ((uint16_t)CFG_TUD_MSC << 1U) | ((uint16_t)CFG_TUD_HID << 2U) |       \
     ((uint16_t)CFG_TUD_MIDI << 3U) | ((uint16_t)CFG_TUD_AUDIO << 4U) |    \
     ((uint16_t)CFG_TUD_VENDOR << 5U))

_Static_assert(JETKVM_USB_HID_DEFAULT_PRODUCT_ID == 0x4012U,
               "four-HID plus MSC gadget PID must remain 0x4012");

/* Copied from the current JetKVM Linux USB gadget implementation. */
static const uint8_t s_keyboard_report_descriptor[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x06,       /* Usage (Keyboard) */
    0xa1, 0x01,       /* Collection (Application) */
    0x05, 0x07,       /*   Usage Page (Keyboard) */
    0x19, 0xe0,       /*   Usage Minimum (Keyboard LeftControl) */
    0x29, 0xe7,       /*   Usage Maximum (Keyboard Right GUI) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0x01,       /*   Logical Maximum (1) */
    0x75, 0x01,       /*   Report Size (1) */
    0x95, 0x08,       /*   Report Count (8) */
    0x81, 0x02,       /*   Input (Data, Variable, Absolute) */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x08,       /*   Report Size (8) */
    0x81, 0x03,       /*   Input (Constant, Variable, Absolute) */
    0x95, 0x05,       /*   Report Count (5) */
    0x75, 0x01,       /*   Report Size (1) */
    0x05, 0x08,       /*   Usage Page (LEDs) */
    0x19, 0x01,       /*   Usage Minimum (Num Lock) */
    0x29, 0x05,       /*   Usage Maximum (Kana) */
    0x91, 0x02,       /*   Output (Data, Variable, Absolute) */
    0x95, 0x01,       /*   Report Count (1) */
    0x75, 0x03,       /*   Report Size (3) */
    0x91, 0x03,       /*   Output (Constant, Variable, Absolute) */
    0x95, 0x06,       /*   Report Count (6) */
    0x75, 0x08,       /*   Report Size (8) */
    0x15, 0x00,       /*   Logical Minimum (0) */
    0x25, 0xff,       /*   Logical Maximum (255) */
    0x05, 0x07,       /*   Usage Page (Keyboard) */
    0x19, 0x00,       /*   Usage Minimum (Reserved) */
    0x29, 0xff,       /*   Usage Maximum (Keyboard Application) */
    0x81, 0x00,       /*   Input (Data, Array, Absolute) */
    0xc0,             /* End Collection */
};

static const uint8_t s_absolute_mouse_report_descriptor[] = {
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x02,             /* Usage (Mouse) */
    0xa1, 0x01,             /* Collection (Application) */
    0x85, 0x01,             /*   Report ID (1): absolute pointer */
    0x09, 0x01,             /*   Usage (Pointer) */
    0xa1, 0x00,             /*   Collection (Physical) */
    0x05, 0x09,             /*     Usage Page (Button) */
    0x19, 0x01,             /*     Usage Minimum (1) */
    0x29, 0x05,             /*     Usage Maximum (5) */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x25, 0x01,             /*     Logical Maximum (1) */
    0x75, 0x01,             /*     Report Size (1) */
    0x95, 0x05,             /*     Report Count (5) */
    0x81, 0x02,             /*     Input (Data, Variable, Absolute) */
    0x95, 0x01,             /*     Report Count (1) */
    0x75, 0x03,             /*     Report Size (3) */
    0x81, 0x03,             /*     Input (Constant, Variable, Absolute) */
    0x05, 0x01,             /*     Usage Page (Generic Desktop) */
    0x09, 0x30,             /*     Usage (X) */
    0x09, 0x31,             /*     Usage (Y) */
    0x16, 0x00, 0x00,       /*     Logical Minimum (0) */
    0x26, 0xff, 0x7f,       /*     Logical Maximum (32767) */
    0x36, 0x00, 0x00,       /*     Physical Minimum (0) */
    0x46, 0xff, 0x7f,       /*     Physical Maximum (32767) */
    0x75, 0x10,             /*     Report Size (16) */
    0x95, 0x02,             /*     Report Count (2) */
    0x81, 0x02,             /*     Input (Data, Variable, Absolute) */
    0xc0,                   /*   End Collection */
    0x85, 0x02,             /*   Report ID (2): wheel */
    0x09, 0x38,             /*   Usage (Wheel) */
    0x15, 0x81,             /*   Logical Minimum (-127) */
    0x25, 0x7f,             /*   Logical Maximum (127) */
    0x35, 0x00,             /*   Physical Minimum (reset) */
    0x45, 0x00,             /*   Physical Maximum (reset) */
    0x75, 0x08,             /*   Report Size (8) */
    0x95, 0x01,             /*   Report Count (1) */
    0x81, 0x06,             /*   Input (Data, Variable, Relative) */
    0x05, 0x0c,             /*   Usage Page (Consumer) */
    0x0a, 0x38, 0x02,       /*   Usage (AC Pan) */
    0x15, 0x81,             /*   Logical Minimum (-127) */
    0x25, 0x7f,             /*   Logical Maximum (127) */
    0x75, 0x08,             /*   Report Size (8) */
    0x95, 0x01,             /*   Report Count (1) */
    0x81, 0x06,             /*   Input (Data, Variable, Relative) */
    0xc0,                   /* End Collection */
};

static const uint8_t s_relative_mouse_report_descriptor[] = {
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x02,             /* Usage (Mouse) */
    0xa1, 0x01,             /* Collection (Application) */
    0x09, 0x01,             /*   Usage (Pointer) */
    0xa1, 0x00,             /*   Collection (Physical) */
    0x05, 0x09,             /*     Usage Page (Button) */
    0x19, 0x01,             /*     Usage Minimum (1) */
    0x29, 0x08,             /*     Usage Maximum (8) */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x25, 0x01,             /*     Logical Maximum (1) */
    0x95, 0x08,             /*     Report Count (8) */
    0x75, 0x01,             /*     Report Size (1) */
    0x81, 0x02,             /*     Input (Data, Variable, Absolute) */
    0x05, 0x01,             /*     Usage Page (Generic Desktop) */
    0x09, 0x30,             /*     Usage (X) */
    0x09, 0x31,             /*     Usage (Y) */
    0x09, 0x38,             /*     Usage (Wheel) */
    0x15, 0x81,             /*     Logical Minimum (-127) */
    0x25, 0x7f,             /*     Logical Maximum (127) */
    0x75, 0x08,             /*     Report Size (8) */
    0x95, 0x03,             /*     Report Count (3) */
    0x81, 0x06,             /*     Input (Data, Variable, Relative) */
    0x05, 0x0c,             /*     Usage Page (Consumer) */
    0x0a, 0x38, 0x02,       /*     Usage (AC Pan) */
    0x15, 0x81,             /*     Logical Minimum (-127) */
    0x25, 0x7f,             /*     Logical Maximum (127) */
    0x75, 0x08,             /*     Report Size (8) */
    0x95, 0x01,             /*     Report Count (1) */
    0x81, 0x06,             /*     Input (Data, Variable, Relative) */
    0xc0,                   /*   End Collection */
    0xc0,                   /* End Collection */
};

/* JetKVM's always-present hid.usb3 wake function. On Linux, configfs'
 * wakeup_on_write attribute turns writes to this vendor report into a remote
 * wake signal. ESP32-P4 emits that signal through tinyusb_remote_wakeup(), but
 * retains the same host-visible HID interface and report descriptor. */
static const uint8_t s_wake_report_descriptor[] = {
    0x06, 0x00, 0xff,       /* Usage Page (Vendor Defined 0xff00) */
    0x09, 0x01,             /* Usage (1) */
    0xa1, 0x01,             /* Collection (Application) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x26, 0xff, 0x00,       /*   Logical Maximum (255) */
    0x75, 0x08,             /*   Report Size (8) */
    0x95, 0x01,             /*   Report Count (1) */
    0x09, 0x01,             /*   Usage (1) */
    0x81, 0x02,             /*   Input (Data, Variable, Absolute) */
    0xc0,                   /* End Collection */
};

/* The IP-KVM OS Services interface: one vendor interface with a bulk OUT and
 * a bulk IN endpoint (OS Services protocol, Transport section). */
#define OS_SERVICES_INTERFACE_CLASS 0xffU
#define OS_SERVICES_INTERFACE_SUBCLASS 0x4aU
#define OS_SERVICES_INTERFACE_PROTOCOL 0x01U
#define OS_SERVICES_DESC_LEN TUD_VENDOR_DESC_LEN

#define CONFIG_DESCRIPTOR_MAX_LENGTH \
    (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + \
     (3U * TUD_HID_DESC_LEN) + TUD_MSC_DESC_LEN + CFG_TUD_AUDIO_FUNC_1_DESC_LEN + \
     OS_SERVICES_DESC_LEN)
#define USB_STRING_STORAGE_CAPACITY \
    (JETKVM_USB_HID_USB_STRING_MAX_LENGTH + 1U)

typedef struct {
    tusb_desc_device_t device;
    uint8_t full_speed_configuration[CONFIG_DESCRIPTOR_MAX_LENGTH];
    uint8_t high_speed_configuration[CONFIG_DESCRIPTOR_MAX_LENGTH];
    char string_storage[STRING_COUNT][USB_STRING_STORAGE_CAPACITY];
    const char *string_descriptors[STRING_COUNT];
    uint8_t function_by_instance[HID_INSTANCE_CAPACITY];
    uint8_t instance_by_function[HID_FUNCTION_COUNT];
    uint8_t interface_by_function[HID_FUNCTION_COUNT];
    uint8_t interface_count;
    uint16_t configuration_length;
    bool mass_storage;
    uint8_t mass_storage_string;
    bool audio;
    uint8_t audio_interface;
    bool os_services;
    uint8_t os_services_interface;
    bool ready;
} hid_descriptor_bank_t;

static hid_descriptor_bank_t s_descriptor_banks[DESCRIPTOR_BANK_COUNT];
static uint8_t s_selected_descriptor_bank;
static uint8_t s_active_descriptor_bank;
static bool s_descriptor_bank_ready;

typedef struct __attribute__((packed)) {
    uint8_t buttons;
    uint16_t x;
    uint16_t y;
} absolute_pointer_report_t;

typedef struct __attribute__((packed)) {
    int8_t wheel_y;
    int8_t wheel_x;
} absolute_wheel_report_t;

typedef struct __attribute__((packed)) {
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel_y;
    int8_t wheel_x;
} relative_mouse_report_t;

_Static_assert(sizeof(absolute_pointer_report_t) == 5,
               "absolute pointer report must match the JetKVM descriptor");
_Static_assert(sizeof(absolute_wheel_report_t) == 2,
               "absolute wheel report must match the JetKVM descriptor");
_Static_assert(sizeof(relative_mouse_report_t) == 5,
               "relative mouse report must match the JetKVM descriptor");
_Static_assert(CONFIG_DESCRIPTOR_MAX_LENGTH <= UINT16_MAX,
               "configuration descriptor length must fit USB wTotalLength");
_Static_assert(DESCRIPTOR_BANK_COUNT == 2,
               "descriptor lifetime scheme requires exactly two banks");
_Static_assert(offsetof(tusb_desc_interface_t, bInterfaceSubClass) == 6,
               "USB interface subclass byte offset changed");
_Static_assert(offsetof(tusb_desc_interface_t, bInterfaceProtocol) == 7,
               "USB interface protocol byte offset changed");

/*
 * Windows binds WinUSB to the OS Services interface through Microsoft OS 2.0
 * descriptors: a BOS platform capability names the vendor request that
 * returns the descriptor set, and the set gives the interface the WINUSB
 * compatible ID and the DeviceInterfaceGUID senders open. The BOS needs
 * bcdUSB 0x0210, so only a configuration with OS Services reports it.
 */
#define MS_OS_20_VENDOR_CODE 0x20U
#define MS_OS_20_DESCRIPTOR_INDEX 7U
#define MS_OS_20_SET_HEADER_LEN 10U
#define MS_OS_20_CONFIGURATION_SUBSET_LEN 8U
#define MS_OS_20_FUNCTION_SUBSET_LEN 8U
#define MS_OS_20_COMPATIBLE_ID_LEN 20U
#define MS_OS_20_REGISTRY_PROPERTY_LEN 132U
#define MS_OS_20_DESC_LEN \
    (MS_OS_20_SET_HEADER_LEN + MS_OS_20_CONFIGURATION_SUBSET_LEN + \
     MS_OS_20_FUNCTION_SUBSET_LEN + MS_OS_20_COMPATIBLE_ID_LEN + \
     MS_OS_20_REGISTRY_PROPERTY_LEN)
/* Offset of bFirstInterface in the function subset header. */
#define MS_OS_20_FIRST_INTERFACE_OFFSET \
    (MS_OS_20_SET_HEADER_LEN + MS_OS_20_CONFIGURATION_SUBSET_LEN + 4U)

static const uint8_t s_bos_descriptor[] = {
    TUD_BOS_DESCRIPTOR(TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, MS_OS_20_VENDOR_CODE),
};

#define UTF16LE_CHAR(c) (c), 0x00

static const uint8_t s_ms_os_20_template[] = {
    /* Set header: Windows 8.1 and later. */
    U16_TO_U8S_LE(MS_OS_20_SET_HEADER_LEN),
    U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),
    /* Configuration subset. Windows reads this byte as the configuration
     * index, not bConfigurationValue, so the only configuration is 0. */
    U16_TO_U8S_LE(MS_OS_20_CONFIGURATION_SUBSET_LEN),
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION), 0, 0,
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - MS_OS_20_SET_HEADER_LEN),
    /* Function subset; bFirstInterface is patched per configuration. */
    U16_TO_U8S_LE(MS_OS_20_FUNCTION_SUBSET_LEN),
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION), 0, 0,
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - MS_OS_20_SET_HEADER_LEN -
                  MS_OS_20_CONFIGURATION_SUBSET_LEN),
    /* Compatible ID WINUSB. */
    U16_TO_U8S_LE(MS_OS_20_COMPATIBLE_ID_LEN),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    /* DeviceInterfaceGUIDs, REG_MULTI_SZ, per the OS Services protocol. */
    U16_TO_U8S_LE(MS_OS_20_REGISTRY_PROPERTY_LEN),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002a),
    UTF16LE_CHAR('D'), UTF16LE_CHAR('e'), UTF16LE_CHAR('v'),
    UTF16LE_CHAR('i'), UTF16LE_CHAR('c'), UTF16LE_CHAR('e'),
    UTF16LE_CHAR('I'), UTF16LE_CHAR('n'), UTF16LE_CHAR('t'),
    UTF16LE_CHAR('e'), UTF16LE_CHAR('r'), UTF16LE_CHAR('f'),
    UTF16LE_CHAR('a'), UTF16LE_CHAR('c'), UTF16LE_CHAR('e'),
    UTF16LE_CHAR('G'), UTF16LE_CHAR('U'), UTF16LE_CHAR('I'),
    UTF16LE_CHAR('D'), UTF16LE_CHAR('s'), UTF16LE_CHAR('\0'),
    U16_TO_U8S_LE(0x0050),
    UTF16LE_CHAR('{'), UTF16LE_CHAR('5'), UTF16LE_CHAR('D'),
    UTF16LE_CHAR('1'), UTF16LE_CHAR('4'), UTF16LE_CHAR('C'),
    UTF16LE_CHAR('7'), UTF16LE_CHAR('C'), UTF16LE_CHAR('2'),
    UTF16LE_CHAR('-'), UTF16LE_CHAR('6'), UTF16LE_CHAR('F'),
    UTF16LE_CHAR('C'), UTF16LE_CHAR('8'), UTF16LE_CHAR('-'),
    UTF16LE_CHAR('4'), UTF16LE_CHAR('F'), UTF16LE_CHAR('5'),
    UTF16LE_CHAR('0'), UTF16LE_CHAR('-'), UTF16LE_CHAR('9'),
    UTF16LE_CHAR('A'), UTF16LE_CHAR('6'), UTF16LE_CHAR('E'),
    UTF16LE_CHAR('-'), UTF16LE_CHAR('3'), UTF16LE_CHAR('B'),
    UTF16LE_CHAR('4'), UTF16LE_CHAR('8'), UTF16LE_CHAR('2'),
    UTF16LE_CHAR('6'), UTF16LE_CHAR('C'), UTF16LE_CHAR('2'),
    UTF16LE_CHAR('E'), UTF16LE_CHAR('3'), UTF16LE_CHAR('8'),
    UTF16LE_CHAR('A'), UTF16LE_CHAR('}'), UTF16LE_CHAR('\0'),
    UTF16LE_CHAR('\0'),
};

_Static_assert(sizeof(s_bos_descriptor) ==
                   TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN,
               "BOS descriptor length");
_Static_assert(sizeof(s_ms_os_20_template) == MS_OS_20_DESC_LEN,
               "Microsoft OS 2.0 descriptor set length");
_Static_assert(MS_OS_20_VENDOR_CODE != 0x40U && MS_OS_20_VENDOR_CODE != 0x41U,
               "the MS OS 2.0 vendor code must differ from OS Services requests");

/* The response for the current request: the template with this
 * configuration's interface number. Only TinyUSB's task touches it. */
static uint8_t s_ms_os_20_descriptor[MS_OS_20_DESC_LEN];

typedef struct {
    bool initializing;
    bool deinitializing;
    bool stopping;
    bool configuring;
    uint32_t lifecycle_generation;
    uint32_t state_event_revision;
    jetkvm_usb_hid_state_t state;
    uint8_t keyboard_modifier;
    uint8_t keyboard_keys[JETKVM_USB_HID_KEY_COUNT];
    jetkvm_keyboard_usage_state_t keyboard_usages;
    absolute_pointer_report_t absolute_pointer;
    uint8_t relative_buttons;
    jetkvm_usb_hid_led_callback_t led_callback;
    void *led_callback_ctx;
    jetkvm_usb_hid_os_services_hooks_t os_services_hooks;
} usb_hid_context_t;

static usb_hid_context_t s_ctx;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
/* READ(10) is the hot path. The capacity changes only with the medium, so
 * retain the block count of one medium instead of taking the media mutex
 * once more per TinyUSB endpoint fragment. The count has the generation of
 * its medium, so that a transfer never takes the count of another medium,
 * also when a read worker that is late stores one. Under s_state_lock. */
static uint32_t s_msc_block_count;
static uint32_t s_msc_block_count_generation;
static bool s_msc_read_command_active;
static bool s_msc_read_command_success;
static bool s_msc_expected_lba_valid;
static uint32_t s_msc_command_next_lba;
static uint32_t s_msc_expected_lba;
static int64_t s_msc_command_started_us;
static int64_t s_msc_last_callback_end_us;
static int64_t s_msc_last_command_complete_us;
static bool s_msc_write_command_active;
static int64_t s_msc_write_command_started_us;
static SemaphoreHandle_t s_report_mutex[HID_INSTANCE_CAPACITY];
static SemaphoreHandle_t s_report_complete[HID_INSTANCE_CAPACITY];
/* Serializes the bounded DWC2 wake signal with TinyUSB driver teardown. */
static SemaphoreHandle_t s_wakeup_mutex;

/* The retained block count of the medium of this generation, or 0. */
static uint32_t msc_block_count(uint32_t generation)
{
    portENTER_CRITICAL(&s_state_lock);
    const uint32_t block_count =
        generation != 0U && generation == s_msc_block_count_generation ?
            s_msc_block_count : 0U;
    portEXIT_CRITICAL(&s_state_lock);
    return block_count;
}

static void msc_retain_block_count(uint32_t generation, uint32_t block_count)
{
    portENTER_CRITICAL(&s_state_lock);
    s_msc_block_count_generation = generation;
    s_msc_block_count = block_count;
    portEXIT_CRITICAL(&s_state_lock);
}

/* Called with s_state_lock held. Keep cumulative counters, but never carry a
 * partially observed BOT command across a detach/re-enumeration boundary. */
static void reset_msc_read_timing_locked(void)
{
    s_msc_read_command_active = false;
    s_msc_read_command_success = false;
    s_msc_expected_lba_valid = false;
    s_msc_command_next_lba = 0U;
    s_msc_expected_lba = 0U;
    s_msc_command_started_us = 0;
    s_msc_last_callback_end_us = 0;
    s_msc_last_command_complete_us = 0;
    s_msc_write_command_active = false;
    s_msc_write_command_started_us = 0;
}

static void record_msc_read_timing_reset_locked(void)
{
    reset_msc_read_timing_locked();
    s_ctx.state.msc_read_timing_resets++;
}

/* Reset the private tracker as soon as the target bus resets or unplugs. The
 * hook runs immediately after the DCD event is queued and may therefore
 * execute in ISR context. Do not reset on every EP0 setup: composite HID
 * control traffic can legitimately overlap an uninterrupted MSC command. */
void tud_event_hook_cb(uint8_t rhport, uint32_t event_id, bool in_isr)
{
    if (rhport != TINYUSB_PORT_HIGH_SPEED_0 ||
        (event_id != DCD_EVENT_BUS_RESET &&
         event_id != DCD_EVENT_UNPLUGGED)) {
        return;
    }
    if (in_isr) {
        portENTER_CRITICAL_ISR(&s_state_lock);
        record_msc_read_timing_reset_locked();
        portEXIT_CRITICAL_ISR(&s_state_lock);
    } else {
        portENTER_CRITICAL(&s_state_lock);
        record_msc_read_timing_reset_locked();
        portEXIT_CRITICAL(&s_state_lock);
    }
}

typedef struct {
    TickType_t started;
    TickType_t budget;
} report_deadline_t;

static inline int32_t clamp_absolute_coordinate(int32_t coordinate)
{
    if (coordinate < 0) {
        return 0;
    }
    if (coordinate > JETKVM_USB_HID_ABSOLUTE_MAX) {
        return JETKVM_USB_HID_ABSOLUTE_MAX;
    }
    return coordinate;
}

static void record_error(esp_err_t error)
{
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.state.last_error = error;
    if (error == ESP_ERR_TIMEOUT || error == ESP_ERR_NOT_FINISHED) {
        s_ctx.state.reports_busy++;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static void record_report_sent(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.state.reports_sent++;
    s_ctx.state.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_state_lock);
}

/*
 * The esp_tinyusb attach event means "configured", not merely connected.
 * TinyUSB's tud_connected() is the authoritative earlier signal: it becomes
 * true after the first setup packet following bus reset. Refresh it when state
 * is observed so pre-configuration connectivity is not misreported.
 */
static void refresh_bus_state(void)
{
    uint32_t lifecycle_generation;
    uint32_t state_event_revision;
    bool active;

    portENTER_CRITICAL(&s_state_lock);
    active = s_ctx.state.initialized && !s_ctx.stopping;
    lifecycle_generation = s_ctx.lifecycle_generation;
    state_event_revision = s_ctx.state_event_revision;
    portEXIT_CRITICAL(&s_state_lock);
    if (!active) {
        return;
    }

    /* tud_* state helpers dereference the live TinyUSB device stack. Protect
     * them with the same lifecycle mutex used by teardown and remote wake;
     * stats readers never wait, and simply retain the last coherent cache if
     * teardown/wake currently owns the controller. */
    SemaphoreHandle_t lifecycle_mutex = s_wakeup_mutex;
    if (lifecycle_mutex == NULL ||
        xSemaphoreTake(lifecycle_mutex, 0) != pdTRUE) {
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    active = s_ctx.state.initialized && !s_ctx.stopping &&
             lifecycle_generation == s_ctx.lifecycle_generation;
    portEXIT_CRITICAL(&s_state_lock);
    if (!active) {
        xSemaphoreGive(lifecycle_mutex);
        return;
    }

    const bool connected = tud_connected();
    const bool mounted = tud_mounted();
    const bool suspended = tud_suspended();
    const bool link_high_speed = connected && tud_speed_get() == TUSB_SPEED_HIGH;

    portENTER_CRITICAL(&s_state_lock);
    if (s_ctx.state.initialized && !s_ctx.stopping &&
        lifecycle_generation == s_ctx.lifecycle_generation &&
        state_event_revision == s_ctx.state_event_revision) {
        s_ctx.state.connected = connected;
        s_ctx.state.mounted = mounted;
        s_ctx.state.suspended = suspended;
        s_ctx.state.link_high_speed = link_high_speed;
        if (!connected || !mounted || !suspended) {
            s_ctx.state.remote_wakeup_enabled = false;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(lifecycle_mutex);
}

static bool reports_allowed(void)
{
    bool allowed;
    portENTER_CRITICAL(&s_state_lock);
    allowed = s_ctx.state.initialized && s_ctx.state.mounted &&
              !s_ctx.state.suspended && !s_ctx.stopping;
    portEXIT_CRITICAL(&s_state_lock);
    return allowed;
}

static TickType_t milliseconds_to_ticks_ceil(uint32_t milliseconds)
{
    uint64_t ticks = ((uint64_t)milliseconds * CONFIG_FREERTOS_HZ + 999U) / 1000U;
    if (ticks == 0) {
        ticks = 1;
    }
    if (ticks > portMAX_DELAY) {
        ticks = portMAX_DELAY;
    }
    return (TickType_t)ticks;
}

static void report_deadline_start(report_deadline_t *deadline)
{
    deadline->started = xTaskGetTickCount();
    deadline->budget = milliseconds_to_ticks_ceil(HID_REPORT_DEADLINE_MS);
}

static TickType_t report_deadline_remaining(const report_deadline_t *deadline)
{
    const TickType_t elapsed = xTaskGetTickCount() - deadline->started;
    if (elapsed >= deadline->budget) {
        return 0;
    }
    return deadline->budget - elapsed;
}

static esp_err_t lock_instance(uint8_t instance, report_deadline_t *deadline)
{
    if (instance >= HID_INSTANCE_CAPACITY || s_report_mutex[instance] == NULL ||
        s_report_complete[instance] == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!reports_allowed()) {
        return ESP_ERR_INVALID_STATE;
    }
    report_deadline_start(deadline);
    if (xSemaphoreTake(s_report_mutex[instance], deadline->budget) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!reports_allowed()) {
        xSemaphoreGive(s_report_mutex[instance]);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/* The caller owns s_report_mutex[instance]. */
static esp_err_t queue_report_locked(uint8_t instance,
                                     uint8_t report_id,
                                     const void *report,
                                     uint16_t report_size,
                                     const report_deadline_t *deadline)
{
    for (;;) {
        if (!reports_allowed()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (tud_hid_n_ready(instance) &&
            tud_hid_n_report(instance, report_id, report, report_size)) {
            record_report_sent();
            return ESP_OK;
        }

        const TickType_t remaining = report_deadline_remaining(deadline);
        if (remaining == 0) {
            return ESP_ERR_TIMEOUT;
        }

        /*
         * Completion, transfer failure, suspend or detach wakes this wait.
         * A stale token is harmless: readiness is always rechecked in-loop.
         */
        if (xSemaphoreTake(s_report_complete[instance], remaining) != pdTRUE) {
            if (tud_hid_n_ready(instance) &&
                tud_hid_n_report(instance, report_id, report, report_size)) {
                record_report_sent();
                return ESP_OK;
            }
            return reports_allowed() ? ESP_ERR_TIMEOUT : ESP_ERR_INVALID_STATE;
        }
    }
}

static esp_err_t send_report(uint8_t instance,
                             uint8_t report_id,
                             const void *report,
                             uint16_t report_size)
{
    report_deadline_t deadline;
    esp_err_t error = lock_instance(instance, &deadline);
    if (error != ESP_OK) {
        record_error(error);
        return error;
    }
    error = queue_report_locked(instance, report_id, report, report_size,
                                &deadline);
    xSemaphoreGive(s_report_mutex[instance]);
    if (error != ESP_OK) {
        record_error(error);
    }
    return error;
}

static void wake_report_waiters(void)
{
    for (size_t i = 0; i < HID_INSTANCE_CAPACITY; ++i) {
        if (s_report_complete[i] != NULL) {
            (void)xSemaphoreGive(s_report_complete[i]);
        }
    }
}

static void reset_input_state(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.keyboard_modifier = 0;
    memset(s_ctx.keyboard_keys, 0, sizeof(s_ctx.keyboard_keys));
    jetkvm_keyboard_usage_clear(&s_ctx.keyboard_usages);
    s_ctx.absolute_pointer.buttons = 0;
    s_ctx.relative_buttons = 0;
    portEXIT_CRITICAL(&s_state_lock);
}

static uint16_t compiled_descriptor_vendor_id(void)
{
#if CONFIG_TINYUSB_DESC_USE_ESPRESSIF_VID
    return TINYUSB_ESPRESSIF_VID;
#else
    return (uint16_t)CONFIG_TINYUSB_DESC_CUSTOM_VID;
#endif
}

static uint16_t compiled_descriptor_product_id(void)
{
#if CONFIG_TINYUSB_DESC_USE_DEFAULT_PID
    return (uint16_t)JETKVM_USB_HID_DEFAULT_PRODUCT_ID;
#else
    return (uint16_t)CONFIG_TINYUSB_DESC_CUSTOM_PID;
#endif
}

static void copy_descriptor_string(
    char destination[JETKVM_USB_HID_DESCRIPTOR_STRING_CAPACITY],
    const char *source)
{
    size_t length = source ? strlen(source) : 0U;
    if (length >= JETKVM_USB_HID_DESCRIPTOR_STRING_CAPACITY) {
        length = JETKVM_USB_HID_DESCRIPTOR_STRING_CAPACITY - 1U;
    }
    if (length > 0U) {
        memcpy(destination, source, length);
    }
    destination[length] = '\0';
}

static jetkvm_usb_hid_descriptor_configuration_t
compiled_default_descriptor_configuration(void)
{
    return (jetkvm_usb_hid_descriptor_configuration_t) {
        .vendor_id = compiled_descriptor_vendor_id(),
        .product_id = compiled_descriptor_product_id(),
        .serial_number = "ESP32-P4",
        .manufacturer = "JetKVM",
        .product = "JetKVM USB Emulation Device",
        .absolute_mouse = true,
        .relative_mouse = true,
        .keyboard = true,
        .mass_storage = true,
        .audio = true,
        .os_services = false,
    };
}

static esp_err_t copy_usb_string(
    char destination[USB_STRING_STORAGE_CAPACITY], const char *source)
{
    if (source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t length = 0;
    while (length <= JETKVM_USB_HID_USB_STRING_MAX_LENGTH &&
           source[length] != '\0') {
        const uint8_t character = (uint8_t)source[length];
        if (character < 0x20U || character > 0x7eU) {
            return ESP_ERR_INVALID_ARG;
        }
        length++;
    }
    if (length > JETKVM_USB_HID_USB_STRING_MAX_LENGTH) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(destination, source, length);
    destination[length] = '\0';
    return ESP_OK;
}

static bool descriptor_function_enabled(
    const jetkvm_usb_hid_descriptor_configuration_t *configuration,
    hid_function_t function)
{
    switch (function) {
    case HID_FUNCTION_KEYBOARD:
        return configuration->keyboard;
    case HID_FUNCTION_ABSOLUTE_MOUSE:
        return configuration->absolute_mouse;
    case HID_FUNCTION_RELATIVE_MOUSE:
        return configuration->relative_mouse;
    case HID_FUNCTION_WAKE:
        return true;
    default:
        return false;
    }
}

static uint16_t descriptor_length_for_function(hid_function_t function)
{
    return function == HID_FUNCTION_KEYBOARD ? TUD_HID_INOUT_DESC_LEN :
                                               TUD_HID_DESC_LEN;
}

static size_t append_hid_function_descriptor(
    uint8_t *destination,
    hid_function_t function,
    uint8_t interface_number,
    uint8_t interval)
{
    switch (function) {
    case HID_FUNCTION_KEYBOARD: {
        const uint8_t descriptor[] = {
            TUD_HID_INOUT_DESCRIPTOR(
                interface_number, STRING_KEYBOARD, HID_ITF_PROTOCOL_KEYBOARD,
                sizeof(s_keyboard_report_descriptor), ENDPOINT_KEYBOARD,
                (0x80U | ENDPOINT_KEYBOARD), HID_ENDPOINT_SIZE, interval),
        };
        memcpy(destination, descriptor, sizeof(descriptor));
        return sizeof(descriptor);
    }
    case HID_FUNCTION_ABSOLUTE_MOUSE: {
        uint8_t descriptor[] = {
            TUD_HID_DESCRIPTOR(
                interface_number, STRING_ABSOLUTE_MOUSE, HID_ITF_PROTOCOL_NONE,
                sizeof(s_absolute_mouse_report_descriptor),
                (0x80U | ENDPOINT_ABSOLUTE_MOUSE), HID_ENDPOINT_SIZE, interval),
        };
        /* Linux configfs exposes the JetKVM absolute pointer as subclass 0,
         * protocol 2. TinyUSB's descriptor macro couples any non-zero protocol
         * to boot subclass 1, so set protocol explicitly without changing the
         * required subclass byte. */
        descriptor[7] = HID_ITF_PROTOCOL_MOUSE;
        memcpy(destination, descriptor, sizeof(descriptor));
        return sizeof(descriptor);
    }
    case HID_FUNCTION_RELATIVE_MOUSE: {
        const uint8_t descriptor[] = {
            TUD_HID_DESCRIPTOR(
                interface_number, STRING_RELATIVE_MOUSE, HID_ITF_PROTOCOL_MOUSE,
                sizeof(s_relative_mouse_report_descriptor),
                (0x80U | ENDPOINT_RELATIVE_MOUSE), HID_ENDPOINT_SIZE, interval),
        };
        memcpy(destination, descriptor, sizeof(descriptor));
        return sizeof(descriptor);
    }
    case HID_FUNCTION_WAKE: {
        const uint8_t descriptor[] = {
            TUD_HID_DESCRIPTOR(
                interface_number, 0, HID_ITF_PROTOCOL_NONE,
                sizeof(s_wake_report_descriptor), (0x80U | ENDPOINT_WAKE),
                HID_ENDPOINT_SIZE, interval),
        };
        memcpy(destination, descriptor, sizeof(descriptor));
        return sizeof(descriptor);
    }
    default:
        return 0;
    }
}

static esp_err_t build_configuration_descriptor(
    const hid_descriptor_bank_t *bank,
    uint8_t interval,
    uint8_t destination[CONFIG_DESCRIPTOR_MAX_LENGTH])
{
    const uint8_t header[] = {
        TUD_CONFIG_DESCRIPTOR(
            1, bank->interface_count, 0, bank->configuration_length,
            TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    };
    memcpy(destination, header, sizeof(header));
    size_t offset = sizeof(header);

    for (hid_function_t function = HID_FUNCTION_KEYBOARD;
         function < HID_FUNCTION_COUNT; ++function) {
        const uint8_t interface_number = bank->interface_by_function[function];
        if (interface_number == JETKVM_USB_HID_INTERFACE_DISABLED) {
            continue;
        }
        offset += append_hid_function_descriptor(
            destination + offset, function, interface_number, interval);
    }

    if (bank->audio) {
        const uint8_t descriptor[] = {
            JETKVM_USB_AUDIO_DESCRIPTOR(bank->audio_interface, ENDPOINT_AUDIO, interval == HID_HS_POLL_INTERVAL),
        };
        _Static_assert(sizeof(descriptor) == CFG_TUD_AUDIO_FUNC_1_DESC_LEN, "audio descriptor size");
        memcpy(destination + offset, descriptor, sizeof(descriptor));
        offset += sizeof(descriptor);
    }

    if (bank->os_services) {
        const uint16_t endpoint_size = interval == HID_HS_POLL_INTERVAL ?
                                           512U : 64U;
        const uint8_t descriptor[] = {
            9, TUSB_DESC_INTERFACE, bank->os_services_interface, 0, 2,
            OS_SERVICES_INTERFACE_CLASS, OS_SERVICES_INTERFACE_SUBCLASS,
            OS_SERVICES_INTERFACE_PROTOCOL, 0,
            7, TUSB_DESC_ENDPOINT, ENDPOINT_OS_SERVICES, TUSB_XFER_BULK,
            U16_TO_U8S_LE(endpoint_size), 0,
            7, TUSB_DESC_ENDPOINT, (0x80U | ENDPOINT_OS_SERVICES),
            TUSB_XFER_BULK, U16_TO_U8S_LE(endpoint_size), 0,
        };
        _Static_assert(sizeof(descriptor) == OS_SERVICES_DESC_LEN,
                       "OS Services descriptor size");
        memcpy(destination + offset, descriptor, sizeof(descriptor));
        offset += sizeof(descriptor);
    }

    if (bank->mass_storage) {
        const uint16_t endpoint_size = interval == HID_HS_POLL_INTERVAL ?
                                           512U : 64U;
        const uint8_t descriptor[] = {
            TUD_MSC_DESCRIPTOR(
                (uint8_t)(bank->interface_count - 1U),
                bank->mass_storage_string,
                ENDPOINT_MSC, (0x80U | ENDPOINT_MSC), endpoint_size),
        };
        memcpy(destination + offset, descriptor, sizeof(descriptor));
        offset += sizeof(descriptor);
    }

    return offset == bank->configuration_length ? ESP_OK : ESP_FAIL;
}

static esp_err_t build_descriptor_bank(
    hid_descriptor_bank_t *bank,
    const jetkvm_usb_hid_descriptor_configuration_t *requested)
{
    const jetkvm_usb_hid_descriptor_configuration_t defaults =
        compiled_default_descriptor_configuration();
    const jetkvm_usb_hid_descriptor_configuration_t *configuration =
        requested == NULL ? &defaults : requested;

    if (configuration->vendor_id == 0U || configuration->product_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(bank, 0, sizeof(*bank));
    memset(bank->function_by_instance, JETKVM_USB_HID_INTERFACE_DISABLED,
           sizeof(bank->function_by_instance));
    memset(bank->instance_by_function, JETKVM_USB_HID_INTERFACE_DISABLED,
           sizeof(bank->instance_by_function));
    memset(bank->interface_by_function, JETKVM_USB_HID_INTERFACE_DISABLED,
           sizeof(bank->interface_by_function));

    bank->string_storage[STRING_LANGID][0] = 0x09;
    bank->string_storage[STRING_LANGID][1] = 0x04;
    for (size_t i = 0; i < STRING_COUNT; ++i) {
        bank->string_descriptors[i] = bank->string_storage[i];
    }

    esp_err_t error = copy_usb_string(
        bank->string_storage[STRING_MANUFACTURER],
        configuration->manufacturer);
    if (error == ESP_OK) {
        error = copy_usb_string(bank->string_storage[STRING_PRODUCT],
                                configuration->product);
    }
    if (error == ESP_OK) {
        error = copy_usb_string(bank->string_storage[STRING_SERIAL],
                                configuration->serial_number);
    }
    if (error == ESP_OK) {
        error = copy_usb_string(bank->string_storage[STRING_KEYBOARD],
                                "JetKVM Keyboard");
    }
    if (error == ESP_OK) {
        error = copy_usb_string(bank->string_storage[STRING_ABSOLUTE_MOUSE],
                                "JetKVM Absolute Mouse");
    }
    if (error == ESP_OK) {
        error = copy_usb_string(bank->string_storage[STRING_RELATIVE_MOUSE],
                                "JetKVM Relative Mouse");
    }
    if (error == ESP_OK) {
        error = copy_usb_string(bank->string_storage[STRING_MSC_CDROM],
                                "JetKVM Virtual CD-ROM");
    }
    if (error != ESP_OK) {
        return error;
    }

    bank->configuration_length = TUD_CONFIG_DESC_LEN;
    for (hid_function_t function = HID_FUNCTION_KEYBOARD;
         function < HID_FUNCTION_COUNT; ++function) {
        if (!descriptor_function_enabled(configuration, function)) {
            continue;
        }
        const uint8_t index = bank->interface_count++;
        bank->interface_by_function[function] = index;
        bank->instance_by_function[function] = index;
        bank->function_by_instance[index] = function;
        bank->configuration_length += descriptor_length_for_function(function);
    }
    bank->audio = configuration->audio;
    bank->audio_interface = bank->interface_count;
    if (bank->audio) {
        bank->interface_count += 2;
        bank->configuration_length += CFG_TUD_AUDIO_FUNC_1_DESC_LEN;
    }
    bank->os_services = configuration->os_services;
    bank->os_services_interface = JETKVM_USB_HID_INTERFACE_DISABLED;
    if (bank->os_services) {
        bank->os_services_interface = bank->interface_count++;
        bank->configuration_length += OS_SERVICES_DESC_LEN;
    }
    bank->mass_storage = configuration->mass_storage;
    bank->mass_storage_string = STRING_MSC_CDROM;
    if (bank->mass_storage) {
        bank->interface_count++;
        bank->configuration_length += TUD_MSC_DESC_LEN;
    }

    bank->device = (tusb_desc_device_t) {
        .bLength = sizeof(tusb_desc_device_t),
        .bDescriptorType = TUSB_DESC_DEVICE,
        /* 2.1 makes hosts read the BOS with the Microsoft OS 2.0
         * capability. */
        .bcdUSB = bank->os_services ? 0x0210 : 0x0200,
        .bDeviceClass = 0x00,
        .bDeviceSubClass = 0x00,
        .bDeviceProtocol = 0x00,
        .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
        .idVendor = configuration->vendor_id,
        .idProduct = configuration->product_id,
        .bcdDevice = CONFIG_TINYUSB_DESC_BCD_DEVICE,
        .iManufacturer = STRING_MANUFACTURER,
        .iProduct = STRING_PRODUCT,
        /* Windows fails to enumerate a 2.1 device whose serial number
         * string is empty, so a device without one reports none. */
        .iSerialNumber = configuration->serial_number[0] != '\0' ?
                             STRING_SERIAL : 0,
        .bNumConfigurations = 1,
    };

    error = build_configuration_descriptor(
        bank, HID_FS_POLL_INTERVAL, bank->full_speed_configuration);
    if (error == ESP_OK) {
        error = build_configuration_descriptor(
            bank, HID_HS_POLL_INTERVAL, bank->high_speed_configuration);
    }
    if (error != ESP_OK) {
        return error;
    }
    bank->ready = true;
    return ESP_OK;
}

static esp_err_t select_descriptor_configuration(
    const jetkvm_usb_hid_descriptor_configuration_t *configuration)
{
    uint8_t destination_bank;

    portENTER_CRITICAL(&s_state_lock);
    if (s_ctx.state.initialized || s_ctx.initializing || s_ctx.deinitializing ||
        s_ctx.stopping || s_ctx.configuring) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.configuring = true;
    destination_bank = s_descriptor_bank_ready ?
        (uint8_t)(s_selected_descriptor_bank ^ 1U) : 0U;
    portEXIT_CRITICAL(&s_state_lock);

    esp_err_t error = build_descriptor_bank(
        &s_descriptor_banks[destination_bank], configuration);

    portENTER_CRITICAL(&s_state_lock);
    if (error == ESP_OK) {
        s_selected_descriptor_bank = destination_bank;
        s_descriptor_bank_ready = true;
    }
    s_ctx.configuring = false;
    s_ctx.state.last_error = error;
    portEXIT_CRITICAL(&s_state_lock);
    return error;
}

static esp_err_t ensure_descriptor_bank_ready(void)
{
    portENTER_CRITICAL(&s_state_lock);
    const bool ready = s_descriptor_bank_ready;
    const bool configuring = s_ctx.configuring;
    portEXIT_CRITICAL(&s_state_lock);
    if (ready) {
        return ESP_OK;
    }
    if (configuring) {
        return ESP_ERR_INVALID_STATE;
    }
    return select_descriptor_configuration(NULL);
}

static uint8_t active_instance_for_function(hid_function_t function)
{
    uint8_t instance = JETKVM_USB_HID_INTERFACE_DISABLED;
    portENTER_CRITICAL(&s_state_lock);
    if (s_descriptor_bank_ready && function < HID_FUNCTION_COUNT) {
        instance = s_descriptor_banks[s_active_descriptor_bank]
                       .instance_by_function[function];
    }
    portEXIT_CRITICAL(&s_state_lock);
    return instance;
}

static uint8_t active_function_for_instance(uint8_t instance)
{
    uint8_t function = JETKVM_USB_HID_INTERFACE_DISABLED;
    portENTER_CRITICAL(&s_state_lock);
    if (s_descriptor_bank_ready && instance < HID_INSTANCE_CAPACITY) {
        function = s_descriptor_banks[s_active_descriptor_bank]
                       .function_by_instance[instance];
    }
    portEXIT_CRITICAL(&s_state_lock);
    return function;
}

/* The OS Services interface of the running configuration, or
 * JETKVM_USB_HID_INTERFACE_DISABLED. */
static uint8_t active_os_services_interface(void)
{
    uint8_t interface_number = JETKVM_USB_HID_INTERFACE_DISABLED;
    portENTER_CRITICAL(&s_state_lock);
    if (s_descriptor_bank_ready &&
        s_descriptor_banks[s_active_descriptor_bank].os_services) {
        interface_number = s_descriptor_banks[s_active_descriptor_bank]
                               .os_services_interface;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return interface_number;
}

static bool active_function_enabled(hid_function_t function)
{
    return active_instance_for_function(function) !=
           JETKVM_USB_HID_INTERFACE_DISABLED;
}

static void tinyusb_event_callback(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event == NULL || event->rhport != TINYUSB_PORT_HIGH_SPEED_0) {
        return;
    }

    jetkvm_usb_hid_led_callback_t callback = NULL;
    void *callback_ctx = NULL;
    bool clear_leds = false;
    bool wake_waiters = false;

    portENTER_CRITICAL(&s_state_lock);
    s_ctx.state_event_revision++;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        reset_msc_read_timing_locked();
        s_ctx.state.connected = tud_connected();
        s_ctx.state.mounted = true;
        s_ctx.state.suspended = false;
        s_ctx.state.remote_wakeup_enabled = false;
        s_ctx.state.link_high_speed = tud_speed_get() == TUSB_SPEED_HIGH;
        s_ctx.state.mount_count++;
        break;
    case TINYUSB_EVENT_DETACHED:
        reset_msc_read_timing_locked();
        s_ctx.state.connected = tud_connected();
        s_ctx.state.mounted = false;
        s_ctx.state.suspended = false;
        s_ctx.state.remote_wakeup_enabled = false;
        s_ctx.state.link_high_speed =
            s_ctx.state.connected && tud_speed_get() == TUSB_SPEED_HIGH;
        s_ctx.state.unmount_count++;
        clear_leds = s_ctx.state.keyboard_leds != 0;
        s_ctx.state.keyboard_leds = 0;
        callback = s_ctx.led_callback;
        callback_ctx = s_ctx.led_callback_ctx;
        s_ctx.keyboard_modifier = 0;
        memset(s_ctx.keyboard_keys, 0, sizeof(s_ctx.keyboard_keys));
        jetkvm_keyboard_usage_clear(&s_ctx.keyboard_usages);
        s_ctx.absolute_pointer.buttons = 0;
        s_ctx.relative_buttons = 0;
        wake_waiters = true;
        break;
#ifdef CONFIG_TINYUSB_SUSPEND_CALLBACK
    case TINYUSB_EVENT_SUSPENDED:
        s_ctx.state.connected = true;
        s_ctx.state.suspended = true;
        s_ctx.state.remote_wakeup_enabled = event->suspended.remote_wakeup;
        s_ctx.state.suspend_count++;
        wake_waiters = true;
        break;
#endif
#ifdef CONFIG_TINYUSB_RESUME_CALLBACK
    case TINYUSB_EVENT_RESUMED:
        s_ctx.state.connected = true;
        s_ctx.state.suspended = false;
        s_ctx.state.remote_wakeup_enabled = false;
        s_ctx.state.resume_count++;
        wake_waiters = true;
        break;
#endif
    default:
        break;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (wake_waiters) {
        wake_report_waiters();
    }
    if (event->id == TINYUSB_EVENT_DETACHED) jetkvm_audio_stream_stopped();
    if (clear_leds && callback != NULL) {
        callback(0, callback_ctx);
    }
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    switch (active_function_for_instance(instance)) {
    case HID_FUNCTION_KEYBOARD:
        return s_keyboard_report_descriptor;
    case HID_FUNCTION_ABSOLUTE_MOUSE:
        return s_absolute_mouse_report_descriptor;
    case HID_FUNCTION_RELATIVE_MOUSE:
        return s_relative_mouse_report_descriptor;
    case HID_FUNCTION_WAKE:
        return s_wake_report_descriptor;
    default:
        return NULL;
    }
}

/* The type of the virtual-media drive as the host has it (sr or sd). The
 * host learns it from INQUIRY when it enumerates the device and does not ask
 * again, so this changes only before a TinyUSB start: it is then the type of
 * the mounted image, and CD-ROM with nothing mounted. An unmount leaves the
 * drive on the host with its type and without a medium. A medium of the other
 * type is not in the drive: its mount re-enumerates USB, and until then the
 * drive is empty. */
static jetkvm_virtual_media_mode_t s_msc_enumerated_mode;

/* The medium generation that the host has been told of, or 0. The host can
 * read, write and size the medium only while this is the mounted medium.
 * After a mount it is not: the host first gets "medium may have changed" and
 * reads the new capacity. A command that the host sends for the previous
 * image then fails and never reaches the new one. */
static uint32_t s_msc_host_generation;

/* The medium generation in the drive, or 0 for an empty drive, at the last
 * GET EVENT STATUS NOTIFICATION that reported a change. */
static uint32_t s_msc_event_generation;

/* When the host last asked the drive for its medium, with TEST UNIT READY or
 * GET EVENT STATUS NOTIFICATION, and the medium generation at that time.
 * Linux and Windows ask a drive every second or two, with and without a
 * medium. macOS asks an empty drive twice per second and stops once the drive
 * has a medium, also after that medium is gone; a computer in its boot
 * firmware and a suspended host do not ask either. Only a host that asks
 * finds a new medium in a drive that it keeps. Under s_state_lock; not asked
 * since the last TinyUSB start. */
#define MSC_HOST_ASKS_WITHIN_MS 3000U
static bool s_msc_asked;
static uint32_t s_msc_asked_ms;
static uint32_t s_msc_asked_generation;

static void msc_host_asked(void)
{
    const uint32_t generation = jetkvm_virtual_media_medium_generation();
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&s_state_lock);
    s_msc_asked = true;
    s_msc_asked_ms = now_ms;
    s_msc_asked_generation = generation;
    portEXIT_CRITICAL(&s_state_lock);
}

/* What the drive holds, as of medium generation s_msc_drive_looked: the
 * generation of its medium, or 0 when it is empty. */
static uint32_t s_msc_drive_looked;
static uint32_t s_msc_drive_found;

/* The generation of the medium in the drive, or 0 for an empty drive. A host
 * asks the drive every second or two, so this takes the medium lock only
 * when the generation has advanced since it last looked: what the drive
 * holds changes with the generation only. For TinyUSB's task, and for the
 * USB owner before it starts TinyUSB. */
static uint32_t msc_drive_generation(void)
{
    const uint32_t generation = jetkvm_virtual_media_medium_generation();
    if (generation != s_msc_drive_looked) {
        jetkvm_virtual_media_state_t state = {0};
        if (jetkvm_virtual_media_get_state(&state) != ESP_OK) {
            return 0U;
        }
        s_msc_drive_found =
            state.image_mounted && state.mode == s_msc_enumerated_mode ?
                state.medium_generation : 0U;
        /* A state newer than this generation is looked up once more. */
        s_msc_drive_looked = generation;
    }
    return s_msc_drive_found;
}

/* The block size of every medium of the drive, as
 * jetkvm_virtual_media_capacity() reports it for the drive's type. */
static uint16_t msc_drive_block_size(void)
{
    return s_msc_enumerated_mode == JETKVM_VIRTUAL_MEDIA_MODE_CDROM ?
               2048U : JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE;
}

/* The generation of the medium in the drive when the host has been told of
 * it, else 0. Takes no lock: READ(10) asks for every endpoint fragment. */
static uint32_t msc_host_medium(uint8_t lun)
{
    if (lun != 0U) {
        return 0U;
    }
    const uint32_t generation = jetkvm_virtual_media_medium_generation();
    return generation == __atomic_load_n(&s_msc_host_generation,
                                         __ATOMIC_ACQUIRE) ? generation : 0U;
}

/* The number of logical units: one. TinyUSB answers GET MAX LUN with this
 * number minus one, and stalls the request when it is 0. Windows clears that
 * stall with CLEAR_FEATURE(ENDPOINT_HALT) for endpoint 0, which TinyUSB does
 * not answer, and waits 5 s for it three times before it starts the drive. */
uint8_t tud_msc_get_maxlun_cb(void)
{
    return 1U;
}

uint32_t tud_msc_inquiry2_cb(uint8_t lun,
                             scsi_inquiry_resp_t *response,
                             uint32_t buffer_size)
{
    if (lun != 0U || response == NULL || buffer_size < sizeof(*response)) {
        return 0U;
    }
    response->peripheral_device_type =
        s_msc_enumerated_mode == JETKVM_VIRTUAL_MEDIA_MODE_CDROM ? 0x05U : 0x00U;
    response->is_removable = 1U;
    memcpy(response->vendor_id, "JetKVM  ", sizeof(response->vendor_id));
    memcpy(response->product_id, "Virtual Media   ",
           sizeof(response->product_id));
    memcpy(response->product_rev, "1.0 ", sizeof(response->product_rev));
    return sizeof(*response);
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id, "JetKVM  ", 8U);
    memcpy(product_id, "Virtual Media   ", 16U);
    memcpy(product_rev, "1.0 ", 4U);
}

/* Set before each TinyUSB start; the TinyUSB task clears it. A host that
 * kept the device across a re-enumeration can still have a previous
 * capacity. */
static bool s_msc_started;

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (lun == 0U) {
        msc_host_asked();
    }
    const uint32_t generation = lun == 0U ? msc_drive_generation() : 0U;
    const bool ready = generation != 0U;
    const bool changed = ready && generation != s_msc_host_generation;
    if (changed) {
        __atomic_store_n(&s_msc_host_generation, generation,
                         __ATOMIC_RELEASE);
    }
    if (!ready) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3aU, 0x00U);
        return false;
    }
    if (changed || s_msc_started) {
        /* "Medium may have changed" makes the host read the capacity again
         * and drop what it kept of the previous medium, as after a card
         * swap. */
        s_msc_started = false;
        tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28U, 0x00U);
        return false;
    }
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    return msc_host_medium(lun) != 0U && jetkvm_virtual_media_is_writable();
}

bool tud_msc_has_write_cache_cb(uint8_t lun)
{
    /* FatFs may retain a dirty sector window after f_write(). Advertising
     * WCE makes the host issue SYNCHRONIZE CACHE at fsync/eject boundaries,
     * which tud_msc_scsi_cb maps to f_sync(). */
    return msc_host_medium(lun) != 0U && jetkvm_virtual_media_is_writable();
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count,
                         uint16_t *block_size)
{
    const uint32_t generation = msc_host_medium(lun);
    bool valid = generation != 0U && block_count != NULL && block_size != NULL;
    /* Check the medium again after the lookup, so that a mount during the
     * lookup cannot report the size of another medium. */
    valid = valid &&
        jetkvm_virtual_media_capacity(block_count, block_size) == ESP_OK &&
        msc_host_medium(lun) == generation;
    if (!valid) {
        if (block_count != NULL) {
            *block_count = 0U;
        }
        if (block_size != NULL) {
            *block_size = JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE;
        }
    }
    if (valid) {
        msc_retain_block_count(generation, *block_count);
    }
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                           bool start, bool load_eject)
{
    (void)power_condition;
    if (lun != 0U) {
        return false;
    }
    /* An empty drive has nothing to eject or to flush, and neither has a
     * drive whose medium the host has not been told of. An eject names the
     * medium that the host was told of: when a mount has replaced it in the
     * meantime, that medium is out already and the new one stays. */
    const uint32_t generation = msc_host_medium(lun);
    if (!start && generation != 0U) {
        esp_err_t error = load_eject ?
            jetkvm_virtual_media_eject(generation) :
            jetkvm_virtual_media_sync();
        if (error == ESP_ERR_INVALID_STATE && load_eject) {
            error = ESP_OK;
        }
        if (error != ESP_OK) {
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0cU, 0x00U);
            return false;
        }
    }
    return true;
}

static int32_t msc_read(uint8_t lun, uint32_t lba, uint32_t offset,
                        void *buffer, uint32_t buffer_size)
{
    int64_t started_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    if (!s_msc_read_command_active) {
        if (s_msc_expected_lba_valid && lba == s_msc_expected_lba &&
            s_msc_last_command_complete_us > 0 &&
            started_us >= s_msc_last_command_complete_us) {
            uint32_t gap_us = (uint32_t)(
                started_us - s_msc_last_command_complete_us);
            s_ctx.state.msc_read_sequential_command_gap_samples++;
            s_ctx.state.msc_read_sequential_command_gap_total_us += gap_us;
            if (gap_us >
                s_ctx.state.msc_read_max_sequential_command_gap_us) {
                s_ctx.state.msc_read_max_sequential_command_gap_us = gap_us;
            }
        }
        s_msc_read_command_active = true;
        s_msc_read_command_success = true;
        s_msc_command_started_us = started_us;
    } else if (s_msc_last_callback_end_us > 0 &&
               started_us >= s_msc_last_callback_end_us) {
        uint32_t gap_us =
            (uint32_t)(started_us - s_msc_last_callback_end_us);
        s_ctx.state.msc_read_intra_gap_samples++;
        s_ctx.state.msc_read_intra_gap_total_us += gap_us;
        if (gap_us > s_ctx.state.msc_read_max_intra_gap_us) {
            s_ctx.state.msc_read_max_intra_gap_us = gap_us;
        }
    }
    portEXIT_CRITICAL(&s_state_lock);

    const uint32_t generation = msc_host_medium(lun);
    const uint16_t block_size = msc_drive_block_size();
    uint32_t block_count = msc_block_count(generation);
    if (generation != 0U && block_count == 0U) {
        uint32_t refreshed_count = 0U;
        uint16_t refreshed_size = JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE;
        if (jetkvm_virtual_media_capacity(&refreshed_count,
                                          &refreshed_size) == ESP_OK) {
            block_count = refreshed_count;
            msc_retain_block_count(generation, block_count);
        }
    }
    /* The read worker can run this, so it sets no sense data: TinyUSB
     * reports every failed READ(10) as medium not present. The read fails
     * when the medium is no longer the one the host has been told of. */
    bool success = generation != 0U && block_count != 0U &&
                   lba < block_count &&
                   jetkvm_virtual_media_read(
                       generation, (uint64_t)lba * block_size + offset,
                       buffer, buffer_size) == ESP_OK;
    int64_t finished_us = esp_timer_get_time();
    uint32_t elapsed_us = (uint32_t)(finished_us - started_us);
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.state.msc_read_calls++;
    if (success) {
        s_ctx.state.msc_read_bytes += buffer_size;
    } else {
        s_ctx.state.msc_read_errors++;
    }
    s_ctx.state.msc_read_total_us += elapsed_us;
    if (elapsed_us > s_ctx.state.msc_read_max_us) {
        s_ctx.state.msc_read_max_us = elapsed_us;
    }
    if (s_ctx.state.msc_read_min_size == 0U ||
        buffer_size < s_ctx.state.msc_read_min_size) {
        s_ctx.state.msc_read_min_size = buffer_size;
    }
    if (buffer_size > s_ctx.state.msc_read_max_size) {
        s_ctx.state.msc_read_max_size = buffer_size;
    }
    s_msc_read_command_success = s_msc_read_command_success && success;
    if (success && block_size > 0U) {
        uint64_t next_byte =
            (uint64_t)lba * block_size + offset + buffer_size;
        if (next_byte % block_size == 0U &&
            next_byte / block_size <= UINT32_MAX) {
            s_msc_command_next_lba = (uint32_t)(next_byte / block_size);
        } else {
            s_msc_read_command_success = false;
        }
    }
    s_msc_last_callback_end_us = finished_us;
    portEXIT_CRITICAL(&s_state_lock);
    return success ? (int32_t)buffer_size : -1;
}

/* READ(10) data of an HTTP(S) image comes from this worker, not from
 * TinyUSB's task: an HTTPS image read can take hundreds of milliseconds, and
 * TinyUSB's task also runs HID, audio and OS Services. The worker reads into
 * its own buffer, and TinyUSB's task completes the command with it only
 * while the command still waits: a bus or BOT reset ends the command, and a
 * newer READ(10) replaces the job. */
typedef struct {
    uint32_t sequence;
    uint8_t lun;
    uint32_t lba;
    uint32_t offset;
    uint32_t size;
    int32_t result;
} msc_read_job_t;

static struct {
    TaskHandle_t task;
    uint8_t *data;
    /* The worker reads and completes under it; TinyUSB is installed while
     * stack_alive is set. */
    SemaphoreHandle_t stack_mutex;
    bool stack_alive;
    /* Under s_state_lock. */
    msc_read_job_t job;
} s_msc_reader;

/* TinyUSB's task. */
static void msc_read_done(void *param)
{
    portENTER_CRITICAL(&s_state_lock);
    const msc_read_job_t job = s_msc_reader.job;
    portEXIT_CRITICAL(&s_state_lock);
    if (job.sequence == (uint32_t)(uintptr_t)param) {
        tud_msc_async_read10_done_in_task(s_msc_reader.data, job.result);
    }
}

static void msc_reader_task(void *arg)
{
    (void)arg;
    uint32_t taken = 0U;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        portENTER_CRITICAL(&s_state_lock);
        const msc_read_job_t job = s_msc_reader.job;
        portEXIT_CRITICAL(&s_state_lock);
        /* A job queued after the notification was taken leaves another
         * notification for the same job. */
        if (job.sequence == taken) {
            continue;
        }
        taken = job.sequence;
        xSemaphoreTake(s_msc_reader.stack_mutex, portMAX_DELAY);
        if (s_msc_reader.stack_alive) {
            const int32_t result = msc_read(job.lun, job.lba, job.offset,
                                            s_msc_reader.data, job.size);
            portENTER_CRITICAL(&s_state_lock);
            if (s_msc_reader.job.sequence == job.sequence) {
                s_msc_reader.job.result = result;
            }
            portEXIT_CRITICAL(&s_state_lock);
            usbd_defer_func(msc_read_done, (void *)(uintptr_t)job.sequence,
                            false);
        }
        xSemaphoreGive(s_msc_reader.stack_mutex);
    }
}

static esp_err_t msc_reader_start(void)
{
    if (s_msc_reader.task != NULL) {
        return ESP_OK;
    }
    s_msc_reader.stack_mutex = xSemaphoreCreateMutex();
    /* The SD card's DMA can write it directly. */
    s_msc_reader.data = heap_caps_malloc(
        CFG_TUD_MSC_EP_BUFSIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED);
    if (s_msc_reader.stack_mutex != NULL && s_msc_reader.data != NULL &&
        xTaskCreatePinnedToCoreWithCaps(
            msc_reader_task, "usb_msc_read", MSC_READER_STACK_BYTES, NULL,
            MSC_READER_PRIORITY, &s_msc_reader.task, 0,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        return ESP_OK;
    }
    if (s_msc_reader.stack_mutex != NULL) {
        vSemaphoreDelete(s_msc_reader.stack_mutex);
    }
    heap_caps_free(s_msc_reader.data);
    s_msc_reader.stack_mutex = NULL;
    s_msc_reader.data = NULL;
    s_msc_reader.task = NULL;
    return ESP_ERR_NO_MEM;
}

/* TinyUSB's task runs before the install returns; the worker waits for the
 * result before it queues a completion. */
static esp_err_t msc_reader_install(const tinyusb_config_t *config)
{
    xSemaphoreTake(s_msc_reader.stack_mutex, portMAX_DELAY);
    const esp_err_t error = tinyusb_driver_install(config);
    s_msc_reader.stack_alive = error == ESP_OK;
    xSemaphoreGive(s_msc_reader.stack_mutex);
    return error;
}

/* Before an uninstall: waits for a read in progress, and the worker reads
 * nothing more and queues no completion. A failed uninstall may have stopped
 * TinyUSB's task already, so this stays in force after one. */
static void msc_reader_stop(void)
{
    xSemaphoreTake(s_msc_reader.stack_mutex, portMAX_DELAY);
    s_msc_reader.stack_alive = false;
    xSemaphoreGive(s_msc_reader.stack_mutex);
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t buffer_size)
{
    /* Local reads stay in this task: the worker is preempted by the TF
     * prefetch task while it holds the medium lock. */
    if (!jetkvm_virtual_media_is_remote()) {
        return msc_read(lun, lba, offset, buffer, buffer_size);
    }
    portENTER_CRITICAL(&s_state_lock);
    s_msc_reader.job = (msc_read_job_t){
        .sequence = s_msc_reader.job.sequence + 1U,
        .lun = lun,
        .lba = lba,
        .offset = offset,
        .size = buffer_size,
        .result = TUD_MSC_RET_ERROR,
    };
    portEXIT_CRITICAL(&s_state_lock);
    xTaskNotifyGive(s_msc_reader.task);
    return TUD_MSC_RET_ASYNC;
}

void tud_msc_read10_complete_cb(uint8_t lun)
{
    int64_t completed_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    if (lun == 0U && s_msc_read_command_active &&
        completed_us >= s_msc_command_started_us) {
        uint32_t elapsed_us =
            (uint32_t)(completed_us - s_msc_command_started_us);
        s_ctx.state.msc_read_commands++;
        s_ctx.state.msc_read_command_total_us += elapsed_us;
        if (elapsed_us > s_ctx.state.msc_read_max_command_us) {
            s_ctx.state.msc_read_max_command_us = elapsed_us;
        }
        s_msc_expected_lba_valid = s_msc_read_command_success;
        s_msc_expected_lba = s_msc_command_next_lba;
        s_msc_last_command_complete_us = completed_us;
    } else {
        s_msc_expected_lba_valid = false;
        s_msc_last_command_complete_us = 0;
    }
    s_msc_read_command_active = false;
    s_msc_read_command_success = false;
    s_msc_last_callback_end_us = 0;
    portEXIT_CRITICAL(&s_state_lock);
}

void tud_msc_write10_complete_cb(uint8_t lun)
{
    if (lun != 0U) {
        return;
    }
    int64_t completed_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    if (s_msc_write_command_active &&
        completed_us >= s_msc_write_command_started_us) {
        uint32_t elapsed_us =
            (uint32_t)(completed_us - s_msc_write_command_started_us);
        s_ctx.state.msc_write_commands++;
        s_ctx.state.msc_write_command_total_us += elapsed_us;
        if (elapsed_us > s_ctx.state.msc_write_max_command_us) {
            s_ctx.state.msc_write_max_command_us = elapsed_us;
        }
    }
    s_ctx.state.msc_read_non_read_commands++;
    record_msc_read_timing_reset_locked();
    portEXIT_CRITICAL(&s_state_lock);
}

void tud_msc_scsi_complete_cb(uint8_t lun,
                              const uint8_t scsi_command[16])
{
    (void)scsi_command;
    if (lun != 0U) {
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.state.msc_read_non_read_commands++;
    /* BOT is serial, so a non-READ completion while a READ is still active
     * proves that reset/error recovery interrupted the READ. In the normal
     * between-command case retain expected_lba: the next gap should include
     * this real host/BOT traffic instead of disappearing into unclassified
     * wall time. */
    if (s_msc_read_command_active) {
        record_msc_read_timing_reset_locked();
    }
    portEXIT_CRITICAL(&s_state_lock);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t buffer_size)
{
    int64_t started_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    if (!s_msc_write_command_active) {
        s_msc_write_command_active = true;
        s_msc_write_command_started_us = started_us;
    }
    portEXIT_CRITICAL(&s_state_lock);

    const uint32_t generation = msc_host_medium(lun);
    const uint16_t block_size = msc_drive_block_size();
    uint32_t block_count = msc_block_count(generation);
    if (generation != 0U && block_count == 0U) {
        uint32_t refreshed_count = 0U;
        uint16_t refreshed_size = JETKVM_VIRTUAL_MEDIA_BLOCK_SIZE;
        if (jetkvm_virtual_media_capacity(&refreshed_count,
                                          &refreshed_size) == ESP_OK) {
            block_count = refreshed_count;
            msc_retain_block_count(generation, block_count);
        }
    }

    bool success = false;
    if (!tud_msc_is_writable_cb(lun)) {
        tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27U, 0x00U);
    } else {
        uint64_t capacity_bytes = (uint64_t)block_count * block_size;
        uint64_t byte_offset = (uint64_t)lba * block_size + offset;
        if (block_count == 0U || block_size == 0U || lba >= block_count ||
            byte_offset > capacity_bytes ||
            buffer_size > capacity_bytes - byte_offset) {
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21U, 0x00U);
        } else if (jetkvm_virtual_media_write(generation, byte_offset, buffer,
                                               buffer_size) != ESP_OK) {
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0cU, 0x00U);
        } else {
            success = true;
        }
    }

    uint32_t elapsed_us =
        (uint32_t)(esp_timer_get_time() - started_us);
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.state.msc_write_calls++;
    if (success) {
        s_ctx.state.msc_write_bytes += buffer_size;
    } else {
        s_ctx.state.msc_write_errors++;
    }
    s_ctx.state.msc_write_total_us += elapsed_us;
    if (elapsed_us > s_ctx.state.msc_write_max_us) {
        s_ctx.state.msc_write_max_us = elapsed_us;
    }
    if (s_ctx.state.msc_write_min_size == 0U ||
        buffer_size < s_ctx.state.msc_write_min_size) {
        s_ctx.state.msc_write_min_size = buffer_size;
    }
    if (buffer_size > s_ctx.state.msc_write_max_size) {
        s_ctx.state.msc_write_max_size = buffer_size;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return success ? (int32_t)buffer_size : -1;
}

int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t scsi_command[16],
                        void *buffer, uint16_t buffer_size)
{
    if (lun == 0U && scsi_command != NULL) {
        /* Linux normally probes MODE SENSE(6), which TinyUSB handles. Some
         * hosts prefer MODE SENSE(10), so report the same caching page here. */
        if (scsi_command[0] == 0x5aU && buffer != NULL && buffer_size > 0U) {
            uint8_t page_control = (uint8_t)(scsi_command[2] >> 6U);
            uint8_t page_code = (uint8_t)(scsi_command[2] & 0x3fU);
            bool include_cache_page = scsi_command[3] == 0U &&
                (page_code == 0x08U || page_code == 0x3fU);
            uint8_t response[28] = {0};
            size_t response_size = 8U;

            response[1] = 6U; /* eight-byte header minus this length field */
            response[3] = tud_msc_is_writable_cb(lun) ? 0U : 0x80U;
            if (include_cache_page) {
                response_size = sizeof(response);
                response[1] = (uint8_t)(sizeof(response) - 2U);
                response[8] = 0x08U;
                response[9] = 0x12U;
                response[10] =
                    (tud_msc_has_write_cache_cb(lun) && page_control != 1U) ?
                        0x04U : 0U;
            }
            response_size = response_size < buffer_size ?
                response_size : buffer_size;
            memcpy(buffer, response, response_size);
            return (int32_t)response_size;
        }
        /* TinyUSB 0.19 does not have built-in SYNCHRONIZE CACHE handling, so
         * both common CDB widths arrive through this generic callback. */
        if (scsi_command[0] == 0x35U || scsi_command[0] == 0x91U) {
            /* A drive without a medium that the host has been told of has
             * nothing to flush. */
            esp_err_t error = msc_host_medium(lun) != 0U ?
                jetkvm_virtual_media_sync() : ESP_OK;
            if (error == ESP_OK) {
                return 0;
            }
            tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0cU, 0x00U);
            return -1;
        }
        if (scsi_command[0] == SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL) {
            return 0;
        }
        /* Linux sends TEST UNIT READY to a CD-ROM drive only when the drive
         * is opened. Its poll, every 2 s, sends GET EVENT STATUS
         * NOTIFICATION, and takes UNIT ATTENTION as a changed medium, on
         * which it opens the drive. The CD-ROM drive gives that answer once
         * for each medium that came or went, and else rejects the command. */
        if (scsi_command[0] == 0x4aU &&
            s_msc_enumerated_mode == JETKVM_VIRTUAL_MEDIA_MODE_CDROM) {
            msc_host_asked();
            const uint32_t generation = msc_drive_generation();
            if (generation != s_msc_event_generation) {
                s_msc_event_generation = generation;
                tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28U,
                                  0x00U);
                return -1;
            }
        }
        /* macOS reads the TOC to find the data track; without it, the disc
         * has no partition to mount. */
        if (scsi_command[0] == 0x43U && buffer != NULL &&
            s_msc_enumerated_mode == JETKVM_VIRTUAL_MEDIA_MODE_CDROM) {
            uint32_t block_count = 0U;
            uint16_t block_size = 0U;
            tud_msc_capacity_cb(lun, &block_count, &block_size);
            if (block_count == 0U) {
                tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3aU, 0x00U);
                return -1;
            }
            int32_t length = jetkvm_cdrom_read_toc(scsi_command, block_count,
                                                   buffer, buffer_size);
            if (length < 0) {
                tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x24U,
                                  0x00U);
            }
            return length;
        }
    }
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20U, 0x00U);
    return -1;
}

uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t requested_length)
{
    if (buffer == NULL || report_type != HID_REPORT_TYPE_INPUT) {
        return 0;
    }

    uint8_t report[8] = {0};
    size_t report_length = 0;
    const uint8_t function = active_function_for_instance(instance);

    portENTER_CRITICAL(&s_state_lock);
    if (function == HID_FUNCTION_KEYBOARD && report_id == 0) {
        report[0] = s_ctx.keyboard_modifier;
        memcpy(&report[2], s_ctx.keyboard_keys, sizeof(s_ctx.keyboard_keys));
        report_length = sizeof(report);
    } else if (function == HID_FUNCTION_ABSOLUTE_MOUSE &&
               report_id == ABSOLUTE_REPORT_ID_POINTER) {
        memcpy(report, &s_ctx.absolute_pointer, sizeof(s_ctx.absolute_pointer));
        report_length = sizeof(s_ctx.absolute_pointer);
    } else if (function == HID_FUNCTION_ABSOLUTE_MOUSE &&
               report_id == ABSOLUTE_REPORT_ID_WHEEL) {
        report_length = sizeof(absolute_wheel_report_t);
    } else if (function == HID_FUNCTION_RELATIVE_MOUSE && report_id == 0) {
        report[0] = s_ctx.relative_buttons;
        report_length = sizeof(relative_mouse_report_t);
    } else if (function == HID_FUNCTION_WAKE && report_id == 0) {
        report_length = 1U;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (report_length == 0 || requested_length < report_length) {
        return 0;
    }
    memcpy(buffer, report, report_length);
    return (uint16_t)report_length;
}

void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer,
                           uint16_t buffer_size)
{
    if (active_function_for_instance(instance) != HID_FUNCTION_KEYBOARD ||
        report_id != 0 || buffer == NULL ||
        buffer_size == 0 ||
        (report_type != HID_REPORT_TYPE_OUTPUT && report_type != HID_REPORT_TYPE_INVALID)) {
        return;
    }

    uint8_t leds = buffer[0] & 0x1fU;
    jetkvm_usb_hid_led_callback_t callback;
    void *callback_ctx;
    bool changed;

    portENTER_CRITICAL(&s_state_lock);
    changed = leds != s_ctx.state.keyboard_leds;
    s_ctx.state.keyboard_leds = leds;
    callback = s_ctx.led_callback;
    callback_ctx = s_ctx.led_callback_ctx;
    portEXIT_CRITICAL(&s_state_lock);

    if (changed && callback != NULL) {
        callback(leds, callback_ctx);
    }
}

void tud_hid_report_complete_cb(uint8_t instance,
                                uint8_t const *report,
                                uint16_t length)
{
    (void)report;
    (void)length;
    if (instance < HID_INSTANCE_CAPACITY && s_report_complete[instance] != NULL) {
        /* TinyUSB task context: wake only, never submit another report here. */
        (void)xSemaphoreGive(s_report_complete[instance]);
    }
}

void tud_hid_report_failed_cb(uint8_t instance,
                              hid_report_type_t report_type,
                              uint8_t const *report,
                              uint16_t transferred_bytes)
{
    (void)report_type;
    (void)report;
    (void)transferred_bytes;
    if (instance < HID_INSTANCE_CAPACITY && s_report_complete[instance] != NULL) {
        /* A failed transfer also releases a bounded endpoint-ready waiter. */
        (void)xSemaphoreGive(s_report_complete[instance]);
    }
}

#ifndef CONFIG_TINYUSB_SUSPEND_CALLBACK
void tud_suspend_cb(bool remote_wakeup_enabled)
{
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.state_event_revision++;
    s_ctx.state.connected = true;
    s_ctx.state.suspended = true;
    s_ctx.state.remote_wakeup_enabled = remote_wakeup_enabled;
    s_ctx.state.suspend_count++;
    portEXIT_CRITICAL(&s_state_lock);
    wake_report_waiters();
}
#endif

#ifndef CONFIG_TINYUSB_RESUME_CALLBACK
void tud_resume_cb(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.state_event_revision++;
    s_ctx.state.connected = true;
    s_ctx.state.suspended = false;
    s_ctx.state.remote_wakeup_enabled = false;
    s_ctx.state.resume_count++;
    portEXIT_CRITICAL(&s_state_lock);
    wake_report_waiters();
}
#endif

uint8_t const *tud_descriptor_bos_cb(void)
{
    return active_os_services_interface() == JETKVM_USB_HID_INTERFACE_DISABLED ?
               NULL : s_bos_descriptor;
}

/*
 * TinyUSB sends every vendor-type control request here, whatever its
 * recipient. The device answers the Microsoft OS 2.0 descriptor request
 * itself and passes requests to the OS Services interface to its handler.
 */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    const uint8_t interface_number = active_os_services_interface();
    if (interface_number == JETKVM_USB_HID_INTERFACE_DISABLED) {
        return false;
    }

    if (request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_DEVICE &&
        request->bmRequestType_bit.direction == TUSB_DIR_IN &&
        request->bRequest == MS_OS_20_VENDOR_CODE &&
        request->wIndex == MS_OS_20_DESCRIPTOR_INDEX) {
        if (stage != CONTROL_STAGE_SETUP) {
            return true;
        }
        memcpy(s_ms_os_20_descriptor, s_ms_os_20_template,
               sizeof(s_ms_os_20_descriptor));
        s_ms_os_20_descriptor[MS_OS_20_FIRST_INTERFACE_OFFSET] =
            interface_number;
        return tud_control_xfer(rhport, request, s_ms_os_20_descriptor,
                                sizeof(s_ms_os_20_descriptor));
    }

    if (request->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE ||
        tu_u16_low(request->wIndex) != interface_number) {
        return false;
    }
    jetkvm_usb_hid_os_services_hooks_t hooks;
    portENTER_CRITICAL(&s_state_lock);
    hooks = s_ctx.os_services_hooks;
    portEXIT_CRITICAL(&s_state_lock);
    return hooks.vendor_request != NULL &&
           hooks.vendor_request(rhport, stage, request, hooks.ctx);
}

esp_err_t jetkvm_usb_hid_set_descriptor_configuration(
    const jetkvm_usb_hid_descriptor_configuration_t *configuration)
{
    return select_descriptor_configuration(configuration);
}

/* Before TinyUSB starts: record the drive type that this enumeration
 * presents, and return the name of the MSC interface, which only a CD-ROM
 * drive has. A host that enumerates the device learns the drive's medium from
 * the first commands, so it has the mounted medium from the start.
 * Linux keeps a device across a port reset when its descriptors are
 * unchanged. usb-storage resets the port when a command fails during the
 * re-enumeration detach; that reset can consume the connect change, and the
 * host then keeps the previous SCSI device type (sd or sr). A different
 * configuration descriptor makes Linux drop and re-probe the device. */
static uint8_t msc_drive_start(void)
{
    s_msc_started = true;
    const uint32_t generation = jetkvm_virtual_media_medium_generation();
    jetkvm_virtual_media_state_t state = {0};
    (void)jetkvm_virtual_media_get_state(&state);
    portENTER_CRITICAL(&s_state_lock);
    s_msc_enumerated_mode = state.mode;
    s_msc_asked = false;
    portEXIT_CRITICAL(&s_state_lock);
    s_msc_drive_found = state.image_mounted ? state.medium_generation : 0U;
    s_msc_drive_looked = generation;
    __atomic_store_n(&s_msc_host_generation, s_msc_drive_found,
                     __ATOMIC_RELEASE);
    s_msc_event_generation = s_msc_drive_found;
    return state.mode == JETKVM_VIRTUAL_MEDIA_MODE_CDROM ?
               STRING_MSC_CDROM : 0U;
}

bool jetkvm_usb_hid_cdrom_drive_takes_medium(void)
{
    /* A mount needs an empty drive, so the generation before the one of a
     * mounted image is the one of the empty drive. A host that asked then, or
     * later, asks a drive in the state that this one was in. */
    const uint32_t generation = jetkvm_virtual_media_medium_generation();
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&s_state_lock);
    const bool takes =
        s_ctx.state.initialized && !s_ctx.stopping &&
        s_descriptor_banks[s_active_descriptor_bank].mass_storage &&
        s_msc_enumerated_mode == JETKVM_VIRTUAL_MEDIA_MODE_CDROM &&
        s_msc_asked && generation - s_msc_asked_generation <= 1U &&
        now_ms - s_msc_asked_ms <= MSC_HOST_ASKS_WITHIN_MS;
    portEXIT_CRITICAL(&s_state_lock);
    return takes;
}

esp_err_t jetkvm_usb_hid_init(void)
{
    esp_err_t error = ensure_descriptor_bank_ready();
    if (error != ESP_OK) {
        record_error(error);
        return error;
    }

    uint8_t descriptor_bank_index;
    portENTER_CRITICAL(&s_state_lock);
    if (s_ctx.state.initialized || s_ctx.initializing ||
        s_ctx.deinitializing || s_ctx.stopping || s_ctx.configuring ||
        !s_descriptor_bank_ready) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.initializing = true;
    descriptor_bank_index = s_selected_descriptor_bank;
    s_active_descriptor_bank = descriptor_bank_index;
    s_ctx.lifecycle_generation++;
    s_ctx.state.connected = false;
    s_ctx.state.mounted = false;
    s_ctx.state.suspended = false;
    s_ctx.state.remote_wakeup_enabled = false;
    s_ctx.state.link_high_speed = false;
    s_ctx.state.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_state_lock);

    for (size_t i = 0; i < HID_INSTANCE_CAPACITY; ++i) {
        if (s_report_mutex[i] == NULL) {
            s_report_mutex[i] = xSemaphoreCreateMutex();
            if (s_report_mutex[i] == NULL) {
                portENTER_CRITICAL(&s_state_lock);
                s_ctx.initializing = false;
                s_ctx.state.last_error = ESP_ERR_NO_MEM;
                portEXIT_CRITICAL(&s_state_lock);
                return ESP_ERR_NO_MEM;
            }
        }
        if (s_report_complete[i] == NULL) {
            s_report_complete[i] = xSemaphoreCreateBinary();
            if (s_report_complete[i] == NULL) {
                portENTER_CRITICAL(&s_state_lock);
                s_ctx.initializing = false;
                s_ctx.state.last_error = ESP_ERR_NO_MEM;
                portEXIT_CRITICAL(&s_state_lock);
                return ESP_ERR_NO_MEM;
            }
        }
    }
    if (s_wakeup_mutex == NULL) {
        s_wakeup_mutex = xSemaphoreCreateMutex();
    }
    if (s_wakeup_mutex == NULL || msc_reader_start() != ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.initializing = false;
        s_ctx.state.last_error = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_NO_MEM;
    }

    hid_descriptor_bank_t *descriptor_bank =
        &s_descriptor_banks[descriptor_bank_index];
    if (descriptor_bank->mass_storage) {
        /* The initializing flag keeps select_descriptor_configuration() away
         * from this bank while the descriptors are rebuilt. */
        descriptor_bank->mass_storage_string = msc_drive_start();
        error = build_configuration_descriptor(
            descriptor_bank, HID_FS_POLL_INTERVAL,
            descriptor_bank->full_speed_configuration);
        if (error == ESP_OK) {
            error = build_configuration_descriptor(
                descriptor_bank, HID_HS_POLL_INTERVAL,
                descriptor_bank->high_speed_configuration);
        }
        if (error != ESP_OK) {
            portENTER_CRITICAL(&s_state_lock);
            s_ctx.initializing = false;
            s_ctx.state.last_error = error;
            portEXIT_CRITICAL(&s_state_lock);
            return error;
        }
    }
    /* esp_tinyusb prints a 23-line device descriptor table (about 2 KB) on
     * every install, and every USB enable and re-enumeration installs it. */
    esp_log_level_set("tusb_desc", ESP_LOG_WARN);
    tinyusb_config_t config = TINYUSB_CONFIG_HIGH_SPEED(tinyusb_event_callback, NULL);
    config.port = TINYUSB_PORT_HIGH_SPEED_0;
    config.phy.skip_setup = false;
    config.phy.self_powered = false;
    config.phy.vbus_monitor_io = -1;
    /* MSC writes and TF card reads enter FatFs and the SD driver here, and
     * without the read worker an HTTPS READ(10) enters the HTTPS Range
     * reader. Keep enough stack for esp_http_client + TLS, not only the USB
     * core. */
    config.task = TINYUSB_TASK_CUSTOM(8192, 5, 0);
    config.descriptor.device = &descriptor_bank->device;
    config.descriptor.qualifier = NULL;
    config.descriptor.string = descriptor_bank->string_descriptors;
    config.descriptor.string_count = STRING_COUNT;
    config.descriptor.full_speed_config =
        descriptor_bank->full_speed_configuration;
    config.descriptor.high_speed_config =
        descriptor_bank->high_speed_configuration;

    if (config.port != TINYUSB_PORT_HIGH_SPEED_0 || config.task.xCoreID != 0 ||
        config.phy.skip_setup) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.initializing = false;
        s_ctx.state.last_error = ESP_ERR_INVALID_STATE;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t audio_error = ESP_OK;
    if (descriptor_bank->audio) {
        audio_error = jetkvm_audio_prepare_usb();
        if (audio_error != ESP_OK) {
            /* Optional codec allocation must not remove keyboard/mouse/MSC.
             * No driver is installed yet; rebuild this bank without UAC2.
             * Preserve the saved preference so a later reconfigure can retry. */
            portENTER_CRITICAL(&s_state_lock);
            descriptor_bank->audio = false;
            descriptor_bank->interface_count -= 2;
            descriptor_bank->configuration_length -= CFG_TUD_AUDIO_FUNC_1_DESC_LEN;
            /* OS Services follows audio and moves into its two numbers. */
            if (descriptor_bank->os_services) {
                descriptor_bank->os_services_interface -= 2;
            }
            error = build_configuration_descriptor(descriptor_bank, HID_FS_POLL_INTERVAL,
                                                    descriptor_bank->full_speed_configuration);
            if (error == ESP_OK) {
                error = build_configuration_descriptor(descriptor_bank, HID_HS_POLL_INTERVAL,
                                                        descriptor_bank->high_speed_configuration);
            }
            portEXIT_CRITICAL(&s_state_lock);
            ESP_LOGW(TAG, "USB audio unavailable; retaining HID/MSC: %s", esp_err_to_name(audio_error));
            if (error != ESP_OK) {
                portENTER_CRITICAL(&s_state_lock);
                s_ctx.initializing = false;
                s_ctx.state.last_error = error;
                portEXIT_CRITICAL(&s_state_lock);
                return error;
            }
        }
    }
    error = msc_reader_install(&config);
    jetkvm_audio_usb_ready(error == ESP_OK && descriptor_bank->audio);
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.initializing = false;
    s_ctx.state.initialized = error == ESP_OK;
    if (error != ESP_OK) {
        s_ctx.state.connected = false;
        s_ctx.state.mounted = false;
        s_ctx.state.suspended = false;
        s_ctx.state.remote_wakeup_enabled = false;
        s_ctx.state.link_high_speed = false;
    }
    s_ctx.state.last_error = error != ESP_OK ? error : audio_error;
    portEXIT_CRITICAL(&s_state_lock);

    if (error == ESP_OK) {
        ESP_LOGI(TAG,
                 "HID-only TinyUSB installed on P4 HS/UTMI with %u interface(s); task pinned to CPU0",
                 descriptor_bank->interface_count);
    } else {
        ESP_LOGE(TAG, "TinyUSB HS install failed: %s", esp_err_to_name(error));
    }
    return error;
}

esp_err_t jetkvm_usb_hid_deinit(void)
{
    portENTER_CRITICAL(&s_state_lock);
    if (!s_ctx.state.initialized || s_ctx.deinitializing || s_ctx.stopping) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.deinitializing = true;
    portEXIT_CRITICAL(&s_state_lock);

    esp_err_t release_error = jetkvm_usb_hid_release_all();

    portENTER_CRITICAL(&s_state_lock);
    s_ctx.stopping = true;
    portEXIT_CRITICAL(&s_state_lock);
    wake_report_waiters();

    size_t locked_count = 0;
    const TickType_t stop_wait = milliseconds_to_ticks_ceil(HID_REPORT_DEADLINE_MS);
    if (s_wakeup_mutex == NULL ||
        xSemaphoreTake(s_wakeup_mutex, stop_wait) != pdTRUE) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.deinitializing = false;
        s_ctx.stopping = false;
        s_ctx.state.last_error = ESP_ERR_TIMEOUT;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_TIMEOUT;
    }
    for (; locked_count < HID_INSTANCE_CAPACITY; ++locked_count) {
        if (xSemaphoreTake(s_report_mutex[locked_count], stop_wait) != pdTRUE) {
            for (size_t i = locked_count; i > 0; --i) {
                xSemaphoreGive(s_report_mutex[i - 1]);
            }
            xSemaphoreGive(s_wakeup_mutex);
            portENTER_CRITICAL(&s_state_lock);
            s_ctx.deinitializing = false;
            s_ctx.stopping = false;
            s_ctx.state.last_error = ESP_ERR_TIMEOUT;
            portEXIT_CRITICAL(&s_state_lock);
            return ESP_ERR_TIMEOUT;
        }
    }
    jetkvm_audio_usb_pause(true);
    portENTER_CRITICAL(&s_state_lock);
    const jetkvm_usb_hid_os_services_hooks_t os_services_hooks =
        s_ctx.os_services_hooks;
    portEXIT_CRITICAL(&s_state_lock);
    if (os_services_hooks.stack_stopping != NULL) {
        os_services_hooks.stack_stopping(os_services_hooks.ctx);
    }
    msc_reader_stop();
    /* The uninstall deletes the TinyUSB task wherever it is; it must not be
     * inside an MSC callback that holds the medium lock. */
    const bool media_paused = jetkvm_virtual_media_pause_io() == ESP_OK;
    esp_err_t error = tinyusb_driver_uninstall();
    if (media_paused) jetkvm_virtual_media_resume_io();
    if (error == ESP_OK) jetkvm_audio_usb_ready(false);
    else jetkvm_audio_usb_pause(false);
    for (size_t i = HID_INSTANCE_CAPACITY; i > 0; --i) {
        xSemaphoreGive(s_report_mutex[i - 1]);
    }

    portENTER_CRITICAL(&s_state_lock);
    s_ctx.deinitializing = false;
    s_ctx.stopping = false;
    if (error == ESP_OK) {
        s_ctx.state.initialized = false;
        s_ctx.state.connected = false;
        s_ctx.state.mounted = false;
        s_ctx.state.suspended = false;
        s_ctx.state.remote_wakeup_enabled = false;
        s_ctx.state.link_high_speed = false;
        s_ctx.state.keyboard_leds = 0;
    }
    s_ctx.state.last_error = error != ESP_OK ? error :
        (release_error == ESP_ERR_INVALID_STATE ? ESP_OK : release_error);
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_wakeup_mutex);

    if (error != ESP_OK) {
        return error;
    }
    reset_input_state();
    return release_error == ESP_ERR_INVALID_STATE ? ESP_OK : release_error;
}

esp_err_t jetkvm_usb_hid_get_state(jetkvm_usb_hid_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    refresh_bus_state();
    portENTER_CRITICAL(&s_state_lock);
    *state = s_ctx.state;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

bool jetkvm_usb_hid_bus_events_waiting(void)
{
    /* tud_task_event_ready() reads TinyUSB's event queue, which a teardown
     * deletes: ask under the lifecycle mutex, as refresh_bus_state() does. */
    SemaphoreHandle_t lifecycle_mutex = s_wakeup_mutex;
    if (lifecycle_mutex == NULL ||
        xSemaphoreTake(lifecycle_mutex, 0) != pdTRUE) {
        return true;
    }
    portENTER_CRITICAL(&s_state_lock);
    const bool active = s_ctx.state.initialized && !s_ctx.stopping;
    portEXIT_CRITICAL(&s_state_lock);
    const bool waiting = !active || tud_task_event_ready();
    xSemaphoreGive(lifecycle_mutex);
    return waiting;
}

esp_err_t jetkvm_usb_hid_get_configuration_snapshot(
    jetkvm_usb_hid_configuration_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t error = ensure_descriptor_bank_ready();
    if (error != ESP_OK) {
        return error;
    }

    hid_descriptor_bank_t bank;
    bool emulation_enabled;
    portENTER_CRITICAL(&s_state_lock);
    bank = s_descriptor_banks[s_selected_descriptor_bank];
    emulation_enabled = s_ctx.state.initialized;
    portEXIT_CRITICAL(&s_state_lock);

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->vendor_id = bank.device.idVendor;
    snapshot->product_id = bank.device.idProduct;
    copy_descriptor_string(snapshot->serial_number,
                           bank.string_storage[STRING_SERIAL]);
    copy_descriptor_string(snapshot->manufacturer,
                           bank.string_storage[STRING_MANUFACTURER]);
    copy_descriptor_string(snapshot->product,
                           bank.string_storage[STRING_PRODUCT]);

    snapshot->keyboard_instance =
        bank.instance_by_function[HID_FUNCTION_KEYBOARD];
    snapshot->absolute_mouse_instance =
        bank.instance_by_function[HID_FUNCTION_ABSOLUTE_MOUSE];
    snapshot->relative_mouse_instance =
        bank.instance_by_function[HID_FUNCTION_RELATIVE_MOUSE];
    snapshot->keyboard_interface =
        bank.interface_by_function[HID_FUNCTION_KEYBOARD];
    snapshot->absolute_mouse_interface =
        bank.interface_by_function[HID_FUNCTION_ABSOLUTE_MOUSE];
    snapshot->relative_mouse_interface =
        bank.interface_by_function[HID_FUNCTION_RELATIVE_MOUSE];
    snapshot->keyboard = snapshot->keyboard_instance !=
                         JETKVM_USB_HID_INTERFACE_DISABLED;
    snapshot->absolute_mouse = snapshot->absolute_mouse_instance !=
                               JETKVM_USB_HID_INTERFACE_DISABLED;
    snapshot->relative_mouse = snapshot->relative_mouse_instance !=
                               JETKVM_USB_HID_INTERFACE_DISABLED;
    snapshot->mass_storage = bank.mass_storage;
    snapshot->serial_console = false;
    snapshot->audio = bank.audio;
    snapshot->os_services = bank.os_services;
    snapshot->os_services_interface = bank.os_services_interface;
    snapshot->emulation_enabled = emulation_enabled;
    return ESP_OK;
}

esp_err_t jetkvm_usb_hid_remote_wakeup(void)
{
    refresh_bus_state();

    SemaphoreHandle_t wakeup_mutex;
    bool allowed;
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.state.wake_attempts++;
    allowed = s_ctx.state.initialized && s_ctx.state.connected &&
              s_ctx.state.mounted && s_ctx.state.suspended &&
              s_ctx.state.remote_wakeup_enabled && !s_ctx.initializing &&
              !s_ctx.deinitializing && !s_ctx.stopping;
    wakeup_mutex = s_wakeup_mutex;
    if (!allowed || wakeup_mutex == NULL) {
        s_ctx.state.wake_failures++;
        s_ctx.state.last_error = ESP_ERR_INVALID_STATE;
    }
    portEXIT_CRITICAL(&s_state_lock);
    if (!allowed || wakeup_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Never queue wake requests behind either another wake or teardown. */
    if (xSemaphoreTake(wakeup_mutex, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.state.wake_failures++;
        s_ctx.state.last_error = ESP_ERR_TIMEOUT;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_TIMEOUT;
    }

    portENTER_CRITICAL(&s_state_lock);
    allowed = s_ctx.state.initialized && s_ctx.state.connected &&
              s_ctx.state.mounted && s_ctx.state.suspended &&
              s_ctx.state.remote_wakeup_enabled && !s_ctx.initializing &&
              !s_ctx.deinitializing && !s_ctx.stopping;
    portEXIT_CRITICAL(&s_state_lock);

    esp_err_t error = ESP_ERR_INVALID_STATE;
    if (allowed) {
        /*
         * esp_tinyusb validates the host's one-shot enable bit again and the
         * P4 DWC2 backend clears its wake signal after one scheduler tick.
         */
        error = tinyusb_remote_wakeup();
    }

    portENTER_CRITICAL(&s_state_lock);
    if (error == ESP_OK) {
        s_ctx.state.wake_success++;
        s_ctx.state.remote_wakeup_enabled = false;
    } else {
        s_ctx.state.wake_failures++;
        if (error == ESP_ERR_INVALID_STATE) {
            s_ctx.state.remote_wakeup_enabled = false;
        }
    }
    s_ctx.state.last_error = error;
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(wakeup_mutex);
    return error;
}

esp_err_t jetkvm_usb_hid_get_keyboard_state(
    jetkvm_usb_hid_keyboard_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    state->modifier = s_ctx.keyboard_modifier;
    memcpy(state->keycodes, s_ctx.keyboard_keys, sizeof(state->keycodes));
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t jetkvm_usb_hid_keyboard_report(
    uint8_t modifier,
    const uint8_t keycodes[JETKVM_USB_HID_KEY_COUNT])
{
    const uint8_t instance =
        active_instance_for_function(HID_FUNCTION_KEYBOARD);
    if (instance == JETKVM_USB_HID_INTERFACE_DISABLED) {
        record_error(ESP_ERR_NOT_SUPPORTED);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t keys[JETKVM_USB_HID_KEY_COUNT] = {0};
    if (keycodes != NULL) {
        memcpy(keys, keycodes, sizeof(keys));
    }

    hid_keyboard_report_t report = {
        .modifier = modifier,
        .reserved = 0,
    };
    memcpy(report.keycode, keys, sizeof(report.keycode));

    report_deadline_t deadline;
    esp_err_t error = lock_instance(instance, &deadline);
    if (error != ESP_OK) {
        record_error(error);
        return error;
    }
    error = queue_report_locked(instance, 0, &report, sizeof(report),
                                &deadline);
    if (error == ESP_OK) {
        jetkvm_keyboard_usage_state_t usages;
        jetkvm_keyboard_usage_from_report(&usages, keys);
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.keyboard_modifier = modifier;
        memcpy(s_ctx.keyboard_keys, keys, sizeof(keys));
        s_ctx.keyboard_usages = usages;
        portEXIT_CRITICAL(&s_state_lock);
    }
    xSemaphoreGive(s_report_mutex[instance]);
    if (error != ESP_OK) {
        record_error(error);
    }
    return error;
}

esp_err_t jetkvm_usb_hid_keypress(uint8_t usage, bool pressed)
{
    if (usage == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t instance =
        active_instance_for_function(HID_FUNCTION_KEYBOARD);
    if (instance == JETKVM_USB_HID_INTERFACE_DISABLED) {
        record_error(ESP_ERR_NOT_SUPPORTED);
        return ESP_ERR_NOT_SUPPORTED;
    }

    report_deadline_t deadline;
    esp_err_t error = lock_instance(instance, &deadline);
    if (error != ESP_OK) {
        record_error(error);
        return error;
    }

    uint8_t modifier;
    uint8_t keys[JETKVM_USB_HID_KEY_COUNT];
    jetkvm_keyboard_usage_state_t usages;
    portENTER_CRITICAL(&s_state_lock);
    modifier = s_ctx.keyboard_modifier;
    usages = s_ctx.keyboard_usages;
    portEXIT_CRITICAL(&s_state_lock);

    bool changed = false;
    if (usage >= 0xe0U && usage <= 0xe7U) {
        const uint8_t mask = (uint8_t)(1U << (usage - 0xe0U));
        const uint8_t updated = pressed ? (modifier | mask) : (modifier & (uint8_t)~mask);
        changed = updated != modifier;
        modifier = updated;
    } else {
        changed = jetkvm_keyboard_usage_update(&usages, usage, pressed);
    }

    if (!changed) {
        xSemaphoreGive(s_report_mutex[instance]);
        return ESP_OK;
    }

    jetkvm_keyboard_usage_build_report(&usages, keys);

    hid_keyboard_report_t report = {
        .modifier = modifier,
        .reserved = 0,
    };
    memcpy(report.keycode, keys, sizeof(report.keycode));
    error = queue_report_locked(instance, 0, &report, sizeof(report),
                                &deadline);
    if (error == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.keyboard_modifier = modifier;
        memcpy(s_ctx.keyboard_keys, keys, sizeof(keys));
        s_ctx.keyboard_usages = usages;
        portEXIT_CRITICAL(&s_state_lock);
    }
    xSemaphoreGive(s_report_mutex[instance]);
    if (error != ESP_OK) {
        record_error(error);
    }
    return error;
}

esp_err_t jetkvm_usb_hid_absolute_mouse_report(int32_t x,
                                               int32_t y,
                                               uint8_t buttons)
{
    const uint8_t instance =
        active_instance_for_function(HID_FUNCTION_ABSOLUTE_MOUSE);
    if (instance == JETKVM_USB_HID_INTERFACE_DISABLED) {
        record_error(ESP_ERR_NOT_SUPPORTED);
        return ESP_ERR_NOT_SUPPORTED;
    }

    absolute_pointer_report_t report = {
        .buttons = buttons & 0x1fU,
        .x = (uint16_t)clamp_absolute_coordinate(x),
        .y = (uint16_t)clamp_absolute_coordinate(y),
    };
    report_deadline_t deadline;
    esp_err_t error = lock_instance(instance, &deadline);
    if (error != ESP_OK) {
        record_error(error);
        return error;
    }
    error = queue_report_locked(instance, ABSOLUTE_REPORT_ID_POINTER, &report,
                                sizeof(report), &deadline);
    if (error == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.absolute_pointer = report;
        portEXIT_CRITICAL(&s_state_lock);
    }
    xSemaphoreGive(s_report_mutex[instance]);
    if (error != ESP_OK) {
        record_error(error);
    }
    return error;
}

esp_err_t jetkvm_usb_hid_absolute_wheel_report(int8_t wheel_y,
                                               int8_t wheel_x)
{
    const uint8_t instance =
        active_instance_for_function(HID_FUNCTION_ABSOLUTE_MOUSE);
    if (instance == JETKVM_USB_HID_INTERFACE_DISABLED) {
        record_error(ESP_ERR_NOT_SUPPORTED);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (wheel_y == 0 && wheel_x == 0) {
        return ESP_OK;
    }
    const absolute_wheel_report_t report = {
        .wheel_y = wheel_y,
        .wheel_x = wheel_x,
    };
    return send_report(instance, ABSOLUTE_REPORT_ID_WHEEL, &report,
                       sizeof(report));
}

esp_err_t jetkvm_usb_hid_wheel_report(int8_t wheel_y, int8_t wheel_x)
{
    if (active_function_enabled(HID_FUNCTION_ABSOLUTE_MOUSE)) {
        return jetkvm_usb_hid_absolute_wheel_report(wheel_y, wheel_x);
    }
    if (active_function_enabled(HID_FUNCTION_RELATIVE_MOUSE)) {
        return jetkvm_usb_hid_relative_mouse_report(
            0, 0, 0, wheel_y, wheel_x);
    }
    return ESP_OK;
}

esp_err_t jetkvm_usb_hid_relative_mouse_report(int8_t dx,
                                               int8_t dy,
                                               uint8_t buttons,
                                               int8_t wheel_y,
                                               int8_t wheel_x)
{
    const uint8_t instance =
        active_instance_for_function(HID_FUNCTION_RELATIVE_MOUSE);
    if (instance == JETKVM_USB_HID_INTERFACE_DISABLED) {
        record_error(ESP_ERR_NOT_SUPPORTED);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const relative_mouse_report_t report = {
        .buttons = buttons,
        .x = dx,
        .y = dy,
        .wheel_y = wheel_y,
        .wheel_x = wheel_x,
    };
    report_deadline_t deadline;
    esp_err_t error = lock_instance(instance, &deadline);
    if (error != ESP_OK) {
        record_error(error);
        return error;
    }
    error = queue_report_locked(instance, 0, &report, sizeof(report),
                                &deadline);
    if (error == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.relative_buttons = buttons;
        portEXIT_CRITICAL(&s_state_lock);
    }
    xSemaphoreGive(s_report_mutex[instance]);
    if (error != ESP_OK) {
        record_error(error);
    }
    return error;
}

esp_err_t jetkvm_usb_hid_release_all(void)
{
    uint16_t absolute_x;
    uint16_t absolute_y;
    uint8_t keyboard_modifier;
    uint8_t keyboard_keys[JETKVM_USB_HID_KEY_COUNT];
    uint8_t absolute_buttons;
    uint8_t relative_buttons;
    portENTER_CRITICAL(&s_state_lock);
    absolute_x = s_ctx.absolute_pointer.x;
    absolute_y = s_ctx.absolute_pointer.y;
    keyboard_modifier = s_ctx.keyboard_modifier;
    memcpy(keyboard_keys, s_ctx.keyboard_keys, sizeof(keyboard_keys));
    absolute_buttons = s_ctx.absolute_pointer.buttons;
    relative_buttons = s_ctx.relative_buttons;
    portEXIT_CRITICAL(&s_state_lock);

    bool keyboard_released = keyboard_modifier == 0U;
    for (size_t i = 0; i < JETKVM_USB_HID_KEY_COUNT; ++i) {
        keyboard_released = keyboard_released && keyboard_keys[i] == 0U;
    }

    esp_err_t first_error = ESP_OK;
    esp_err_t error;
    /*
     * Linux only polls a HID interrupt-IN endpoint while that input device has
     * an open consumer. A report already queued on an unused mouse endpoint
     * can therefore remain TinyUSB-busy indefinitely. Do not enqueue a
     * duplicate all-up report when the successfully submitted shadow is
     * already released: there is no host state left for this barrier to
     * clear, and waiting for an unpolled endpoint would block unrelated
     * keyboard input across every later WebRTC generation.
     *
     * A non-zero shadow is still released fail-closed and retains the existing
     * retry barrier. This includes the case where a prior down report is
     * pending or has reached the host.
     */
    if (!keyboard_released &&
        active_function_enabled(HID_FUNCTION_KEYBOARD)) {
        error = jetkvm_usb_hid_keyboard_report(0, NULL);
        if (error != ESP_OK) {
            first_error = error;
        }
    }
    if (absolute_buttons != 0U &&
        active_function_enabled(HID_FUNCTION_ABSOLUTE_MOUSE)) {
        error = jetkvm_usb_hid_absolute_mouse_report(
            absolute_x, absolute_y, 0);
        if (first_error == ESP_OK && error != ESP_OK) {
            first_error = error;
        }
    }
    if (relative_buttons != 0U &&
        active_function_enabled(HID_FUNCTION_RELATIVE_MOUSE)) {
        error = jetkvm_usb_hid_relative_mouse_report(0, 0, 0, 0, 0);
        if (first_error == ESP_OK && error != ESP_OK) {
            first_error = error;
        }
    }
    return first_error;
}

esp_err_t jetkvm_usb_hid_set_os_services_hooks(
    const jetkvm_usb_hid_os_services_hooks_t *hooks)
{
    portENTER_CRITICAL(&s_state_lock);
    if (hooks == NULL) {
        memset(&s_ctx.os_services_hooks, 0, sizeof(s_ctx.os_services_hooks));
    } else {
        s_ctx.os_services_hooks = *hooks;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t jetkvm_usb_hid_set_led_callback(
    jetkvm_usb_hid_led_callback_t callback,
    void *ctx)
{
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.led_callback = callback;
    s_ctx.led_callback_ctx = ctx;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

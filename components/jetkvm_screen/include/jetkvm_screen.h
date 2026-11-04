#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JETKVM_SCREEN_IPV4_CAPACITY 16U
#define JETKVM_SCREEN_IPV6_CAPACITY 64U
#define JETKVM_SCREEN_MAC_CAPACITY 18U
#define JETKVM_SCREEN_WIFI_SSID_CAPACITY 33U
#define JETKVM_SCREEN_WIFI_SETUP_CODE_CAPACITY 8U
/* "<reason>\n<SSID>" of a failed Wi-Fi join, with its NUL. */
#define JETKVM_SCREEN_WIFI_FAILURE_CAPACITY 64U
#define JETKVM_SCREEN_VERSION_CAPACITY 64U
#define JETKVM_SCREEN_DEVICE_ID_CAPACITY 32U
#define JETKVM_SCREEN_METADATA_CAPACITY 96U

/*
 * These IDs intentionally match the generated JetKVM LVGL screen IDs.
 * AUTO keeps the home/no-network selection driven by network_connected.
 */
typedef enum {
    JETKVM_SCREEN_PAGE_AUTO = 0,
    JETKVM_SCREEN_PAGE_BOOT = 1,
    JETKVM_SCREEN_PAGE_NO_NETWORK = 2,
    JETKVM_SCREEN_PAGE_HOME = 3,
    JETKVM_SCREEN_PAGE_MENU = 4,
    JETKVM_SCREEN_PAGE_ADVANCED = 5,
    JETKVM_SCREEN_PAGE_NETWORK = 6,
    JETKVM_SCREEN_PAGE_ABOUT = 7,
    JETKVM_SCREEN_PAGE_STATUS = 8,
    JETKVM_SCREEN_PAGE_RESET_CONFIG = 9,
    JETKVM_SCREEN_PAGE_REBOOT = 10,
    JETKVM_SCREEN_PAGE_REBOOTING = 11,
    JETKVM_SCREEN_PAGE_DHCP_CLIENT = 12,
    /* Menu > Regulatory, built beside the generated screens. Only the menu
     * opens it; the state's page cannot request it. */
    JETKVM_SCREEN_PAGE_REGULATORY = 13,
} jetkvm_screen_page_t;

typedef enum {
    JETKVM_SCREEN_CLOUD_NOT_CONFIGURED = 0,
    JETKVM_SCREEN_CLOUD_DISCONNECTED,
    JETKVM_SCREEN_CLOUD_CONNECTING,
    JETKVM_SCREEN_CLOUD_CONNECTED,
} jetkvm_screen_cloud_state_t;

typedef enum {
    /* Looking for a saved network. */
    JETKVM_SCREEN_WIFI_SETUP_CONNECTING = 0,
    /* BLE provisioning is advertising. */
    JETKVM_SCREEN_WIFI_SETUP_ADVERTISING,
    /* A phone is connected over BLE and needs the setup code. */
    JETKVM_SCREEN_WIFI_SETUP_CODE,
    /* The phone sent the right code; it picks a network next. */
    JETKVM_SCREEN_WIFI_SETUP_ACCEPTED,
    /* The phone sent a network; the device joins it. */
    JETKVM_SCREEN_WIFI_SETUP_JOINING,
} jetkvm_screen_wifi_setup_t;

typedef struct {
    bool network_connected;
    char ipv4[JETKVM_SCREEN_IPV4_CAPACITY];
    char ipv6[JETKVM_SCREEN_IPV6_CAPACITY];
    char mac[JETKVM_SCREEN_MAC_CAPACITY];
    bool wifi_connection;
    bool wifi_signal_valid;
    int8_t wifi_rssi;
    char wifi_ssid[JETKVM_SCREEN_WIFI_SSID_CAPACITY];
    /* Wi-Fi board without a connection: BLE setup progress for the
     * no-network screen. The code is shown only while a phone is connected. */
    jetkvm_screen_wifi_setup_t wifi_setup;
    char wifi_setup_code[JETKVM_SCREEN_WIFI_SETUP_CODE_CAPACITY];
    /* While wifi_setup is CONNECTING: the network being joined, or empty
     * while the device looks for a saved network. While it is JOINING: the
     * network the phone sent. */
    char wifi_joining[JETKVM_SCREEN_WIFI_SSID_CAPACITY];
    /* Why the last join failed ("<reason>\n<SSID>"), or empty. The
     * no-network screen shows it below the setup, Paired or search text;
     * not while a join runs, on the setup code page, or in menu setup before
     * a setup device has sent a network. */
    char wifi_failure[JETKVM_SCREEN_WIFI_FAILURE_CAPACITY];
    /* BLE setup started from the local menu: the no-network screen stays up
     * whatever the connection, and the top button ends setup while
     * wifi_setup_stoppable. */
    bool wifi_setup_menu;
    bool wifi_setup_stoppable;
    /* Counts menu setup starts, so the screen learns that a start it asked
     * for happened even when the setup ended before a state showed it. */
    uint32_t wifi_setup_menu_starts;

    jetkvm_screen_cloud_state_t cloud_state;
    uint32_t active_sessions;
    bool hdmi_connected;

    /* Whether the host has mounted the USB device. The screen shows it in
     * the USB status label as Connected or Disconnected. */
    bool usb_connected;

    /* USB debugging: the stored setting, and whether the USB Serial/JTAG
     * port is connected now. They differ until the next restart. */
    bool usb_debugging_enabled;
    bool usb_debugging_active;

    char app_version[JETKVM_SCREEN_VERSION_CAPACITY];
    char system_version[JETKVM_SCREEN_VERSION_CAPACITY];
    char device_id[JETKVM_SCREEN_DEVICE_ID_CAPACITY];
    char cloud_account_id[JETKVM_SCREEN_METADATA_CAPACITY];
    char cloud_domain[JETKVM_SCREEN_METADATA_CAPACITY];
    char build_branch[JETKVM_SCREEN_METADATA_CAPACITY];
    char build_date[JETKVM_SCREEN_METADATA_CAPACITY];
    char runtime_version[JETKVM_SCREEN_METADATA_CAPACITY];
    char kernel_version[JETKVM_SCREEN_METADATA_CAPACITY];
    char cpu_serial[JETKVM_SCREEN_METADATA_CAPACITY];

    jetkvm_screen_page_t page;
} jetkvm_screen_state_t;

typedef enum {
    /* Values 1 to 3 are kept for API compatibility. The firmware does not
     * report them; jetkvm_screen_get_stats() always reports
     * JETKVM_SCREEN_FIT_NATIVE_240_SQUARE. */
    JETKVM_SCREEN_FIT_ROTATION_270_LEFT_VIEWPORT = 1,
    JETKVM_SCREEN_FIT_ROTATION_270_FULL_FRAME = 2,
    JETKVM_SCREEN_FIT_ROTATION_270_240_VIEWPORT = 3,
    /* Native 240x240 LVGL canvas mapped one-to-one to the square panel. */
    JETKVM_SCREEN_FIT_NATIVE_240_SQUARE = 4,
} jetkvm_screen_fit_policy_t;

typedef struct {
    bool initialized;
    bool active;
    bool usb_connected;
    esp_err_t last_error;
    uint16_t display_rotation;
    uint32_t backlight_brightness, backlight_duty;
    esp_err_t display_settings_error;
    uint32_t updates_submitted;
    uint32_t updates_applied;
    uint32_t pending_replacements;
    jetkvm_screen_fit_policy_t fit_policy;
    uint16_t source_width;
    uint16_t source_height;
    uint16_t panel_width;
    uint16_t panel_height;
    bool preview_active;
    uint16_t preview_width;
    uint16_t preview_height;
    uint16_t preview_fps;
    uint32_t preview_frames_submitted;
    uint32_t preview_frames_applied;
    uint32_t preview_frames_dropped;
    uint32_t preview_ppa_frames;
    uint32_t preview_ppa_total_us;
    uint32_t preview_ppa_max_us;
    /* The failure text the no-network screen showed last (it stays after
     * the screen hides it), or empty, and how often the screen put up a
     * failure text (shown after being hidden, or changed). */
    char wifi_failure_shown[JETKVM_SCREEN_WIFI_FAILURE_CAPACITY];
    uint32_t wifi_failure_shows;
} jetkvm_screen_stats_t;

/* Initializes the fixed P4 ST7789 interface and enables the display. */
esp_err_t jetkvm_screen_start(void);

/*
 * Turns the panel and backlight off. The worker and LVGL object graph are
 * retained so a later start is safe; no caller-visible LVGL operation occurs.
 */
esp_err_t jetkvm_screen_stop(void);

/*
 * Latest-state, non-blocking handoff. The caller never invokes LVGL directly;
 * a dedicated display task owns all generated UI objects.
 */
esp_err_t jetkvm_screen_update(const jetkvm_screen_state_t *state);

/*
 * Best-effort HDMI preview handoff for ESP32-P4's packed YUV420 format:
 * line 1 is U,Y,Y..., line 2 is V,Y,Y..., and the pattern repeats. PPA owns
 * the conversion and aspect-fit scaling. HDMI frames bypass LVGL and use two
 * RGB565 buffers to overlap PPA work with direct ST7789 SPI DMA. Calls may be
 * dropped while both stages are occupied.
 */
esp_err_t jetkvm_screen_preview_submit_ouyy_evyy(const uint8_t *pixels,
                                                 size_t size,
                                                 uint16_t width,
                                                 uint16_t height,
                                                 uint16_t fps);

/* Same direct-preview handoff for packed UYVY input. */
esp_err_t jetkvm_screen_preview_submit_uyvy(const uint8_t *pixels,
                                            size_t size,
                                            uint16_t width,
                                            uint16_t height,
                                            uint16_t fps);

/* Hides the live preview and returns to the normal generated status UI. */
esp_err_t jetkvm_screen_preview_stop(void);

esp_err_t jetkvm_screen_get_stats(jetkvm_screen_stats_t *stats);

/*
 * Wi-Fi board: the local menu starts BLE Wi-Fi setup (start=true) or ends
 * it. The handler runs on the screen task, must not block, and returns NULL
 * or an error message.
 */
typedef const char *(*jetkvm_screen_wifi_setup_handler_t)(bool start);
void jetkvm_screen_set_wifi_setup_handler(jetkvm_screen_wifi_setup_handler_t handler);

#ifdef __cplusplus
}
#endif

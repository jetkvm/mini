#include "local_display_settings.h"
#include "jetkvm_screen.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/ppa.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

#include "jetkvm_screen_buttons.h"
#include "jetkvm_screen_fit.h"
#include "jetkvm_screen_orientation.h"
#include "jetkvm_screen_regulatory.h"
#include "jetkvm_screen_usb_debugging.h"
#include "jetkvm_board_config.h"
#include "jetkvm_control.h"
#include "jetkvm_factory_reset.h"
#include "jetkvm_usb_debugging.h"
#include "ui/images.h"
#include "ui/fonts.h"
#include "ui/screens.h"
#include "ui/styles.h"
#include "ui/ui.h"
#include "ui/vars.h"

#define SCREEN_LCD_HOST SPI2_HOST
#define SCREEN_LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define SCREEN_LCD_CMD_BITS 8
#define SCREEN_LCD_PARAM_BITS 8
#define SCREEN_PIN_MOSI JETKVM_SCREEN_PIN_MOSI
#define SCREEN_PIN_SCLK JETKVM_SCREEN_PIN_SCLK
#define SCREEN_PIN_DC JETKVM_SCREEN_PIN_DC
#define SCREEN_PIN_RST JETKVM_SCREEN_PIN_RST
#define SCREEN_PIN_CS JETKVM_SCREEN_PIN_CS
#define SCREEN_PIN_BACKLIGHT JETKVM_SCREEN_PIN_BACKLIGHT

#define SCREEN_BACKLIGHT_PWM_FREQUENCY_HZ 20000
#define SCREEN_BACKLIGHT_DUTY_RESOLUTION LEDC_TIMER_10_BIT
#define SCREEN_BACKLIGHT_DUTY_MAX ((1U << 10U) - 1U)
#define SCREEN_BACKLIGHT_DEFAULT_PERCENT 15U
#define SCREEN_BACKLIGHT_DUTY_FOR_PERCENT(percent) \
    ((SCREEN_BACKLIGHT_DUTY_MAX * (percent)) / 100U)
#define SCREEN_BACKLIGHT_SPEED_MODE LEDC_LOW_SPEED_MODE
#define SCREEN_BACKLIGHT_TIMER LEDC_TIMER_0
#define SCREEN_BACKLIGHT_CHANNEL LEDC_CHANNEL_0

#if defined(CONFIG_JETKVM_SCREEN_ROTATION_0)
#define SCREEN_PANEL_ROTATION_DEGREES 0
#define SCREEN_PANEL_X_GAP 0
#define SCREEN_PANEL_Y_GAP 0
#define SCREEN_PANEL_SWAP_XY false
#define SCREEN_PANEL_MIRROR_X false
#define SCREEN_PANEL_MIRROR_Y false
#elif defined(CONFIG_JETKVM_SCREEN_ROTATION_90)
#define SCREEN_PANEL_ROTATION_DEGREES 90
#define SCREEN_PANEL_X_GAP 0
#define SCREEN_PANEL_Y_GAP 0
#define SCREEN_PANEL_SWAP_XY true
#define SCREEN_PANEL_MIRROR_X true
#define SCREEN_PANEL_MIRROR_Y false
#elif defined(CONFIG_JETKVM_SCREEN_ROTATION_180)
#define SCREEN_PANEL_ROTATION_DEGREES 180
#define SCREEN_PANEL_X_GAP 0
#define SCREEN_PANEL_Y_GAP 80
#define SCREEN_PANEL_SWAP_XY false
#define SCREEN_PANEL_MIRROR_X true
#define SCREEN_PANEL_MIRROR_Y true
#elif defined(CONFIG_JETKVM_SCREEN_ROTATION_270)
#define SCREEN_PANEL_ROTATION_DEGREES 270
#define SCREEN_PANEL_X_GAP 80
#define SCREEN_PANEL_Y_GAP 0
#define SCREEN_PANEL_SWAP_XY true
#define SCREEN_PANEL_MIRROR_X false
#define SCREEN_PANEL_MIRROR_Y true
#else
#error "A JetKVM screen hardware rotation must be selected"
#endif

#define SCREEN_TASK_STACK_BYTES (16 * 1024)
#define SCREEN_TASK_PRIORITY 3
#define SCREEN_TASK_CORE 0
#define SCREEN_LOOP_MIN_DELAY_MS 10U
#define SCREEN_LOOP_MAX_DELAY_MS 100U
#define SCREEN_START_TIMEOUT_MS 5000
#define SCREEN_STOP_TIMEOUT_MS 1000
#define SCREEN_FIRST_REFRESH_TIMEOUT_MS 250

#define SCREEN_PREVIEW_WIDTH 240U
#define SCREEN_PREVIEW_HEIGHT 240U
#define SCREEN_PREVIEW_PIXELS (SCREEN_PREVIEW_WIDTH * SCREEN_PREVIEW_HEIGHT)
#define SCREEN_PREVIEW_BYTES (SCREEN_PREVIEW_PIXELS * sizeof(uint16_t))
#define SCREEN_PREVIEW_ALIGNMENT 128U
#define SCREEN_PREVIEW_BUFFER_COUNT 2U
#define SCREEN_PREVIEW_INTERVAL_MS 0U
#define SCREEN_PREVIEW_OSD_SCALE 2U
#define SCREEN_PREVIEW_OSD_GLYPH_WIDTH 5U
#define SCREEN_PREVIEW_OSD_GLYPH_HEIGHT 7U
#define SCREEN_PREVIEW_OSD_ADVANCE \
    ((SCREEN_PREVIEW_OSD_GLYPH_WIDTH + 1U) * SCREEN_PREVIEW_OSD_SCALE)
#define SCREEN_PREVIEW_OSD_HEIGHT \
    (SCREEN_PREVIEW_OSD_GLYPH_HEIGHT * SCREEN_PREVIEW_OSD_SCALE + 8U)

#define SCREEN_NOTIFY_START (1UL << 0)
#define SCREEN_NOTIFY_STOP (1UL << 1)
#define SCREEN_NOTIFY_BUTTON (1UL << 2)
#define SCREEN_NOTIFY_RPC_FAILED (1UL << 3)
#define SCREEN_NOTIFY_UPDATE (1UL << 2)
#define SCREEN_NOTIFY_PREVIEW (1UL << 3)

#define SCREEN_EVENT_READY (1UL << 0)
#define SCREEN_EVENT_FAILED (1UL << 1)
#define SCREEN_EVENT_ACTIVE (1UL << 2)
#define SCREEN_EVENT_STOPPED (1UL << 3)

typedef enum {
    SCREEN_TRANSFER_NONE = 0,
    SCREEN_TRANSFER_LVGL,
    SCREEN_TRANSFER_PREVIEW,
} screen_transfer_owner_t;

static esp_err_t screen_backlight_set_duty(uint32_t duty)
{
    esp_err_t error = ledc_set_duty(SCREEN_BACKLIGHT_SPEED_MODE,
                                    SCREEN_BACKLIGHT_CHANNEL, duty);
    if (error != ESP_OK) {
        return error;
    }
    return ledc_update_duty(SCREEN_BACKLIGHT_SPEED_MODE,
                            SCREEN_BACKLIGHT_CHANNEL);
}

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    lv_display_t *display;
    uint16_t *render_buffer;
    uint16_t *viewport_buffer;
    size_t viewport_buffer_pixels;
    SemaphoreHandle_t flush_done;
    volatile bool flush_inflight;
    volatile uint32_t transfer_owner;
    bool spi_bus_initialized;
    ppa_client_handle_t preview_ppa;
    SemaphoreHandle_t preview_mutex;
    uint16_t *preview_buffers[SCREEN_PREVIEW_BUFFER_COUNT];
    uint8_t preview_next_buffer;
    volatile bool preview_direct_mode;
    bool preview_layout_valid;
    bool ui_redraw_requested; /* Protected by preview_mutex. */
    uint16_t ui_degrees;      /* Protected by preview_mutex. */
    uint16_t preview_layout_width;
    uint16_t preview_layout_height;
    uint16_t preview_layout_fps;
} screen_context_t;

static const char *TAG = "jetkvm_screen";
static screen_context_t s_screen;
static TaskHandle_t s_task;
static QueueHandle_t s_state_queue;
static EventGroupHandle_t s_events;
static volatile int32_t s_last_error = ESP_OK;
static volatile uint32_t s_updates_submitted;
static volatile uint32_t s_updates_applied;
static volatile uint32_t s_pending_replacements;
static volatile bool s_rendered_usb_connected;
static volatile bool s_preview_network_connected;
static volatile bool s_preview_active;
static volatile bool s_preview_stop_requested;
static volatile uint16_t s_preview_width;
static volatile uint16_t s_preview_height;
static volatile uint16_t s_preview_fps;
static volatile uint32_t s_preview_epoch;
static volatile uint32_t s_preview_last_attempt_ms;
static volatile uint32_t s_preview_frames_submitted;
static volatile uint32_t s_preview_frames_applied;
static volatile uint32_t s_preview_frames_dropped;
static volatile uint32_t s_preview_ppa_frames;
static volatile uint32_t s_preview_ppa_total_us;
static volatile uint32_t s_preview_ppa_max_us;
static jetkvm_screen_cloud_state_t s_rendered_cloud_state =
    JETKVM_SCREEN_CLOUD_NOT_CONFIGURED;
/* Set by the display RPC worker when a reset could not be scheduled; the
 * screen task then leaves the rebooting screen. */
static bool s_rpc_failed;
static bool s_cloud_blink_active;
static uint32_t s_cloud_blink_started_ms;
static lv_opa_t s_cloud_blink_last_opacity = LV_OPA_COVER;
static lv_obj_t *s_wifi_status_row;
static jetkvm_screen_wifi_setup_handler_t s_wifi_setup_handler;
/* Screen task only. Setup was started here and no state since reports the
 * start (wifi_setup_menu_starts unchanged): keep the setup page up
 * meanwhile. */
static bool s_wifi_setup_requested;
static uint32_t s_wifi_setup_menu_starts;
/* Screen task only: the Wi-Fi connection of the last state, for the
 * confirmation before setup leaves it. */
static bool s_wifi_connected;
static char s_wifi_connected_ssid[JETKVM_SCREEN_WIFI_SSID_CAPACITY];
#if JETKVM_BOARD_WIFI
LV_FONT_DECLARE(ui_font_font_bold20);
static lv_obj_t *s_wifi_setup_ssid;
/* The no-network screen's lines with the reason of the last failed join,
 * and the extra space between the reason and the SSID. */
static lv_obj_t *s_wifi_failure_label;
#define WIFI_FAILURE_LINE_SPACE 3
#endif
/* The failure text the screen showed last, and how often it put up a
 * failure text; written on the screen task, read by jetkvm_screen_get_stats. */
static portMUX_TYPE s_wifi_failure_shown_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_wifi_failure_shown[JETKVM_SCREEN_WIFI_FAILURE_CAPACITY];
static uint32_t s_wifi_failure_shows;
static lv_obj_t *s_wifi_ssid_label;
static lv_obj_t *s_wifi_signal_icon;

static uint32_t screen_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static esp_err_t screen_preview_resources_init(screen_context_t *screen)
{
    screen->preview_mutex = xSemaphoreCreateMutex();
    if (screen->preview_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t index = 0; index < SCREEN_PREVIEW_BUFFER_COUNT; ++index) {
        screen->preview_buffers[index] = heap_caps_aligned_alloc(
            SCREEN_PREVIEW_ALIGNMENT, SCREEN_PREVIEW_BYTES,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (screen->preview_buffers[index] == NULL) {
            return ESP_ERR_NO_MEM;
        }
        memset(screen->preview_buffers[index], 0, SCREEN_PREVIEW_BYTES);
    }

    ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        /* A 64-byte burst finishes the full-frame downscale quickly without
         * the long encoder stalls observed with the 8-byte setting. */
        .data_burst_length = PPA_DATA_BURST_LENGTH_64,
    };
    esp_err_t error = ppa_register_client(&ppa_config, &screen->preview_ppa);
    if (error != ESP_OK) {
        return error;
    }
    return ESP_OK;
}

static const uint8_t *screen_preview_osd_glyph(char character)
{
    static const uint8_t glyphs[][SCREEN_PREVIEW_OSD_GLYPH_HEIGHT] = {
        {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}, /* 0 */
        {0x04, 0x0c, 0x14, 0x04, 0x04, 0x04, 0x1f}, /* 1 */
        {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}, /* 2 */
        {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}, /* 3 */
        {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}, /* 4 */
        {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e}, /* 5 */
        {0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e}, /* 6 */
        {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
        {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}, /* 8 */
        {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e}, /* 9 */
        {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}, /* X */
        {0x0e, 0x11, 0x17, 0x15, 0x17, 0x10, 0x0f}, /* @ */
        {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}, /* H */
        {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}, /* Z */
        {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, /* N */
        {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}, /* O */
        {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}, /* E */
        {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* T */
        {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}, /* W */
        {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}, /* R */
        {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, /* K */
        {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, /* space */
    };
    if (character >= '0' && character <= '9') {
        return glyphs[(size_t)(character - '0')];
    }
    switch (character) {
    case 'X': return glyphs[10];
    case '@': return glyphs[11];
    case 'H': return glyphs[12];
    case 'Z': return glyphs[13];
    case 'N': return glyphs[14];
    case 'O': return glyphs[15];
    case 'E': return glyphs[16];
    case 'T': return glyphs[17];
    case 'W': return glyphs[18];
    case 'R': return glyphs[19];
    case 'K': return glyphs[20];
    default: return glyphs[21];
    }
}

static void screen_preview_draw_text_box(uint16_t *buffer, const char *text,
                                         uint32_t box_y)
{
    if (buffer == NULL || text == NULL || text[0] == '\0') {
        return;
    }
    size_t length = strnlen(text, 23U);
    uint32_t text_width = (uint32_t)length * SCREEN_PREVIEW_OSD_ADVANCE;
    if (text_width >= SCREEN_PREVIEW_OSD_SCALE) {
        text_width -= SCREEN_PREVIEW_OSD_SCALE;
    }
    uint32_t box_width = text_width + 8U;
    uint32_t box_x = (SCREEN_PREVIEW_WIDTH - box_width) / 2U;
    for (uint32_t y = box_y; y < box_y + SCREEN_PREVIEW_OSD_HEIGHT; ++y) {
        memset(buffer + y * SCREEN_PREVIEW_WIDTH + box_x, 0,
               box_width * sizeof(uint16_t));
    }

    uint32_t cursor_x = box_x + 4U;
    uint32_t text_y = box_y + 4U;
    for (size_t character_index = 0; character_index < length;
         ++character_index) {
        const uint8_t *glyph = screen_preview_osd_glyph(text[character_index]);
        for (uint32_t row = 0; row < SCREEN_PREVIEW_OSD_GLYPH_HEIGHT; ++row) {
            for (uint32_t column = 0; column < SCREEN_PREVIEW_OSD_GLYPH_WIDTH;
                 ++column) {
                if ((glyph[row] & (1U <<
                    (SCREEN_PREVIEW_OSD_GLYPH_WIDTH - 1U - column))) == 0U) {
                    continue;
                }
                for (uint32_t scale_y = 0; scale_y < SCREEN_PREVIEW_OSD_SCALE;
                     ++scale_y) {
                    uint16_t *destination = buffer +
                        (text_y + row * SCREEN_PREVIEW_OSD_SCALE + scale_y) *
                            SCREEN_PREVIEW_WIDTH +
                        cursor_x + column * SCREEN_PREVIEW_OSD_SCALE;
                    for (uint32_t scale_x = 0;
                         scale_x < SCREEN_PREVIEW_OSD_SCALE; ++scale_x) {
                        destination[scale_x] = UINT16_MAX;
                    }
                }
            }
        }
        cursor_x += SCREEN_PREVIEW_OSD_ADVANCE;
    }
}

static void screen_preview_draw_osd(uint16_t *buffer,
                                    uint16_t width,
                                    uint16_t height,
                                    uint16_t fps)
{
    char mode_text[24];
    int mode_text_length = snprintf(mode_text, sizeof(mode_text),
                                    "%uX%u@%uHZ", (unsigned)width,
                                    (unsigned)height, (unsigned)fps);
    if (buffer == NULL || mode_text_length <= 0) {
        return;
    }
    screen_preview_draw_text_box(
        buffer, mode_text,
        SCREEN_PREVIEW_HEIGHT - SCREEN_PREVIEW_OSD_HEIGHT - 5U);
    if (!__atomic_load_n(&s_preview_network_connected, __ATOMIC_ACQUIRE)) {
        screen_preview_draw_text_box(buffer, "NO NETWORK", 5U);
    }
}

static void screen_preview_swap_rgb565_bytes(uint16_t *buffer,
                                             size_t pixel_count)
{
    for (size_t index = 0; index < pixel_count; ++index) {
        buffer[index] = __builtin_bswap16(buffer[index]);
    }
}

static void set_label_text(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }
    const char *current = lv_label_get_text(label);
    if (current == NULL || strcmp(current, text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void set_visible(lv_obj_t *object, bool visible)
{
    if (object == NULL) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_optional_label(lv_obj_t *label, lv_obj_t *container, const char *text)
{
    bool visible = text != NULL && text[0] != '\0';
    if (visible) {
        set_label_text(label, text);
    }
    set_visible(label, visible);
    set_visible(container, visible);
}

/* setUsbDebuggingState runs the control protocol's RPC dispatch, which
 * keeps a default context on the stack. */
#define SCREEN_RPC_WORKER_STACK 6144
#define SCREEN_RPC_WORKER_PRIORITY 4
/* Time for the rebooting screen to reach the panel before the reset. */
#define SCREEN_RPC_REBOOT_DELAY_MS 700

/* The value of the setUsbDebuggingState request the worker runs. */
static bool s_usb_debugging_request;

static void screen_rpc_failed(void)
{
    __atomic_store_n(&s_rpc_failed, true, __ATOMIC_RELEASE);
    if (s_task != NULL) {
        (void)xTaskNotify(s_task, SCREEN_NOTIFY_RPC_FAILED, eSetBits);
    }
}

/* Advanced > USB Debugging: the RPC the web UI's Advanced page sends.
 * Returns true when the requested value is the stored setting. The RPC
 * reads the setting again for its answer after the save, so an error in
 * the answer does not mean that the save failed. */
static bool usb_debugging_store(void)
{
    char request[128], response[192];
    size_t length = 0;
    int request_length = snprintf(
        request, sizeof(request),
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"setUsbDebuggingState\","
        "\"params\":{\"enabled\":%s}}",
        s_usb_debugging_request ? "true" : "false");
    jetkvm_control_result_t result = jetkvm_control_handle_rpc(
        (const uint8_t *)request, (size_t)request_length, response, sizeof(response), &length);
    bool stored = jetkvm_usb_debugging_enabled() == s_usb_debugging_request;
    if (!stored) {
        ESP_LOGE(TAG, "USB debugging from the display not stored: %.*s",
                 result == JETKVM_CONTROL_RESPONSE_READY ? (int)length : 0, response);
    }
    return stored;
}

/* Runs on its own internal-RAM stack: the screen task's stack lives in PSRAM
 * and the factory reset and USB debugging write NVS, which disables the
 * external cache. */
static void screen_rpc_worker(void *arg)
{
    const char *method = arg;
    vTaskDelay(pdMS_TO_TICKS(SCREEN_RPC_REBOOT_DELAY_MS));
    if (strcmp(method, "resetConfig") == 0) {
        const char *error = jetkvm_factory_reset_request(NULL);
        if (error != NULL) {
            ESP_LOGE(TAG, "factory reset from the display refused: %s", error);
            screen_rpc_failed();
        } else {
            ESP_LOGW(TAG, "factory reset requested from the display");
            jetkvm_factory_reset_dispatch_complete();
        }
    } else if (strcmp(method, "setUsbDebuggingState") == 0) {
        /* The setting takes effect at a restart, so the hold restarts. */
        if (usb_debugging_store()) {
            ESP_LOGW(TAG, "USB debugging %s from the display; restarting (%u bytes of stack left)",
                     s_usb_debugging_request ? "on" : "off",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            esp_restart();
        }
        screen_rpc_failed();
    } else {
        ESP_LOGW(TAG, "reboot requested from the display");
        esp_restart();
    }
    vTaskDelete(NULL);
}

/* Undo what the generated reset action did before it called the hook, so a
 * refused reset can be retried, and return to the Advanced menu. Runs on the
 * screen task with LVGL owned. */
static void screen_rpc_restore(void)
{
    set_visible(objects.reset_config_spinner, false);
    set_visible(objects.reset_config_button, true);
    set_label_text(objects.reset_config_label, "Press and hold for\n10 seconds");
    loadScreen(SCREEN_ID_MENU_ADVANCED_SCREEN);
}

#if JETKVM_BOARD_WIFI
/* Start or end BLE Wi-Fi setup for the menu, on the screen task. */
static void wifi_setup_request(bool start)
{
    jetkvm_screen_wifi_setup_handler_t handler =
        __atomic_load_n(&s_wifi_setup_handler, __ATOMIC_ACQUIRE);
    const char *error = handler != NULL ? handler(start) : "Wi-Fi setup is not available";
    if (error != NULL) {
        ESP_LOGW(TAG, "Wi-Fi setup from the display: %s", error);
        if (start) loadScreen(SCREEN_ID_MENU_SCREEN);
        return;
    }
    if (start) {
        s_wifi_setup_requested = true;
        /* The next state tells whether a saved network lets setup end. */
        jetkvm_screen_buttons_set_wifi_setup(true, false);
        loadScreen(SCREEN_ID_NO_NETWORK_SCREEN);
    }
}

/* Settings > Set up Wi-Fi. Leaving a working network needs the hold on the
 * confirmation screen; otherwise setup starts at once. */
static void wifi_setup_menu_clicked(lv_event_t *event)
{
    (void)event;
    if (!s_wifi_connected) {
        wifi_setup_request(true);
        return;
    }
    set_label_text(s_wifi_setup_ssid, s_wifi_connected_ssid[0] != '\0' ? s_wifi_connected_ssid
                                                                       : "this network");
    loadScreen(SCREEN_ID_SWITCH_DHCP_CLIENT_SCREEN);
}

/* The buttons accept the confirmation with the menu hold. */
static void wifi_setup_confirmed(lv_event_t *event)
{
    (void)event;
    wifi_setup_request(true);
}
#endif

/* The Mini has one DHCP client, so the generated DHCP client entry and its
 * confirmation screen are free: the Wi-Fi board uses them for Set up Wi-Fi.
 * The confirmation takes the 700 ms menu hold, not the generated 5 s
 * countdown, so its countdown text is hidden. */
static void wifi_setup_menu_init(void)
{
#if JETKVM_BOARD_WIFI
    lv_obj_t *label = lv_obj_get_child(objects.menu_btn_network, 0);
    if (label != NULL && lv_obj_check_type(label, &lv_label_class)) {
        lv_label_set_text(label, "Set up Wi-Fi");
    }
    lv_obj_add_event_cb(objects.menu_btn_network, wifi_setup_menu_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *title = lv_obj_get_child(objects.dhcp_client_header, 1);
    if (title != NULL && lv_obj_check_type(title, &lv_label_class)) {
        lv_label_set_text(title, "Set up Wi-Fi");
    }
    set_visible(objects.dhcpc_label, false);
    lv_obj_add_event_cb(objects.obj2, wifi_setup_confirmed, LV_EVENT_CLICKED, NULL);
    /* "Disconnects from" on one line, the SSID in bold below it. */
    lv_obj_t *notice = lv_label_create(objects.dhcp_client_label_container);
    add_style_info_content_label(notice);
    lv_obj_set_style_text_font(notice, &ui_font_font_book20, LV_PART_MAIN);
    lv_obj_set_width(notice, LV_PCT(100));
    lv_label_set_text(notice, "Disconnects from");
    lv_obj_move_to_index(notice, 0);
    s_wifi_setup_ssid = lv_label_create(objects.dhcp_client_label_container);
    add_style_info_content_label(s_wifi_setup_ssid);
    lv_obj_set_style_text_font(s_wifi_setup_ssid, &ui_font_font_bold20, LV_PART_MAIN);
    /* Two lines at most, so a long SSID does not push the button off the page. */
    lv_obj_set_size(s_wifi_setup_ssid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(s_wifi_setup_ssid,
                                2 * lv_font_get_line_height(&ui_font_font_bold20), LV_PART_MAIN);
    lv_label_set_long_mode(s_wifi_setup_ssid, LV_LABEL_LONG_DOT);
    lv_obj_move_to_index(s_wifi_setup_ssid, 1);
    lv_label_set_text(s_wifi_setup_ssid, "");
    lv_label_set_text(objects.dhcp_client_change_label, "Hold to start");

    /* Below the setup or search text: two lines at most, in the red of a
     * weak signal on the home screen. */
    s_wifi_failure_label = lv_label_create(objects.no_network_content_container);
    add_style_label_font16(s_wifi_failure_label);
    lv_obj_set_style_text_color(s_wifi_failure_label, lv_color_hex(0xef4444), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_wifi_failure_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_size(s_wifi_failure_label, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_text_line_space(s_wifi_failure_label, WIFI_FAILURE_LINE_SPACE,
                                     LV_PART_MAIN);
    lv_obj_set_style_max_height(s_wifi_failure_label,
                                2 * lv_font_get_line_height(lv_obj_get_style_text_font(
                                        s_wifi_failure_label, LV_PART_MAIN)) +
                                    WIFI_FAILURE_LINE_SPACE,
                                LV_PART_MAIN);
    lv_label_set_long_mode(s_wifi_failure_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_wifi_failure_label, "");
    set_visible(s_wifi_failure_label, false);
#endif
}

/* The generated hold actions call this after their countdown, on the screen
 * task with LVGL owned. Only the maintenance methods the Mini supports act. */
static void screen_rpc_handler(const char *method, const char *params)
{
#if JETKVM_BOARD_WIFI
    /* The buttons end menu setup with a held top button. */
    if (strcmp(method, "stopWifiSetup") == 0) {
        wifi_setup_request(false);
        return;
    }
#endif
    static const char *const supported[] = {"reboot", "resetConfig", "setUsbDebuggingState"};
    const char *action = NULL;
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (strcmp(method, supported[i]) == 0) {
            action = supported[i];
        }
    }
    if (action == NULL) {
        ESP_LOGW(TAG, "display RPC %s is not supported on this device", method);
        return;
    }
    if (strcmp(action, "setUsbDebuggingState") == 0) {
        cJSON *request = params != NULL ? cJSON_Parse(params) : NULL;
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(request, "enabled");
        bool valid = cJSON_IsBool(enabled);
        s_usb_debugging_request = cJSON_IsTrue(enabled);
        cJSON_Delete(request);
        if (!valid) {
            ESP_LOGW(TAG, "display RPC %s needs {\"enabled\": bool}", action);
            return;
        }
    }
    if (xTaskCreate(screen_rpc_worker, "screen_rpc", SCREEN_RPC_WORKER_STACK,
                    (void *)action, SCREEN_RPC_WORKER_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the display %s worker", action);
        screen_rpc_restore();
        return;
    }
    loadScreen(SCREEN_ID_REBOOTING_SCREEN);
}

/* The generated About rows carry the RV1106's titles. Retitle them for what
 * the Mini reports in each value without editing the generated screens. */
static void retitle_row(lv_obj_t *container, const char *title)
{
    lv_obj_t *label = container != NULL ? lv_obj_get_child(container, 0) : NULL;
    if (label != NULL && lv_obj_check_type(label, &lv_label_class)) {
        lv_label_set_text(label, title);
    }
}

static void about_rows_init(void)
{
    retitle_row(objects.system_version_container, "Hardware");
    retitle_row(objects.app_version_container, "Firmware");
    retitle_row(objects.build_date_container, "Build Date");
    retitle_row(objects.golang_version_container, "ESP-IDF");
    retitle_row(objects.kernel_version_container, "FreeRTOS");
    retitle_row(objects.cpu_serial_container, "Device ID");
}

static void wifi_status_init(void)
{
    lv_obj_t *parent = lv_obj_get_parent(objects.home_info_mac_addr);
    s_wifi_status_row = lv_obj_create(parent);
    lv_obj_remove_style_all(s_wifi_status_row);
    lv_obj_set_size(s_wifi_status_row, LV_PCT(98), 17);
    lv_obj_set_flex_flow(s_wifi_status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_wifi_status_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_wifi_status_row, 6, LV_PART_MAIN);
    lv_obj_clear_flag(s_wifi_status_row,
                      LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_wifi_status_row, LV_OBJ_FLAG_HIDDEN);

    s_wifi_signal_icon = lv_label_create(s_wifi_status_row);
    lv_obj_set_style_text_font(s_wifi_signal_icon, &lv_font_montserrat_14,
                               LV_PART_MAIN);
    lv_label_set_text(s_wifi_signal_icon, LV_SYMBOL_WIFI);

    s_wifi_ssid_label = lv_label_create(s_wifi_status_row);
    lv_obj_set_width(s_wifi_ssid_label, 0);
    lv_obj_set_height(s_wifi_ssid_label, 17);
    lv_obj_set_flex_grow(s_wifi_ssid_label, 1);
    lv_label_set_long_mode(s_wifi_ssid_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_wifi_ssid_label, &ui_font_font_book16,
                               LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wifi_ssid_label, lv_color_hex(0xffffff),
                                LV_PART_MAIN);
}

static void set_wifi_status(bool visible, const char *ssid,
                            bool signal_valid, int8_t rssi)
{
    set_visible(objects.home_info_ipv6_addr, !visible);
    set_visible(objects.home_info_mac_addr, !visible);
    set_visible(s_wifi_status_row, visible);
    if (!visible) {
        return;
    }

    set_label_text(s_wifi_ssid_label,
                   ssid != NULL && ssid[0] != '\0' ? ssid : "...");
    uint32_t signal_color = 0x808080U;
    if (signal_valid) {
        signal_color = rssi >= -55 ? 0x22c55eU :
                       rssi >= -67 ? 0x84cc16U :
                       rssi >= -75 ? 0xeab308U : 0xef4444U;
    }
    lv_obj_set_style_text_color(s_wifi_signal_icon,
                                lv_color_hex(signal_color), LV_PART_MAIN);
}

static void set_connection_label(lv_obj_t *label, bool connected)
{
    set_label_text(label, connected ? "Connected" : "Disconnected");
    if (connected) {
        lv_obj_add_state(label, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(label, LV_STATE_CHECKED);
    }
}

static void normalize_state(jetkvm_screen_state_t *destination,
                            const jetkvm_screen_state_t *source)
{
    *destination = *source;
    destination->ipv4[JETKVM_SCREEN_IPV4_CAPACITY - 1U] = '\0';
    destination->ipv6[JETKVM_SCREEN_IPV6_CAPACITY - 1U] = '\0';
    destination->mac[JETKVM_SCREEN_MAC_CAPACITY - 1U] = '\0';
    destination->wifi_ssid[JETKVM_SCREEN_WIFI_SSID_CAPACITY - 1U] = '\0';
    destination->wifi_setup_code[JETKVM_SCREEN_WIFI_SETUP_CODE_CAPACITY - 1U] = '\0';
    destination->wifi_joining[JETKVM_SCREEN_WIFI_SSID_CAPACITY - 1U] = '\0';
    destination->wifi_failure[JETKVM_SCREEN_WIFI_FAILURE_CAPACITY - 1U] = '\0';
    if ((int)destination->wifi_setup < (int)JETKVM_SCREEN_WIFI_SETUP_CONNECTING ||
        destination->wifi_setup > JETKVM_SCREEN_WIFI_SETUP_JOINING) {
        destination->wifi_setup = JETKVM_SCREEN_WIFI_SETUP_CONNECTING;
    }
    destination->app_version[JETKVM_SCREEN_VERSION_CAPACITY - 1U] = '\0';
    destination->system_version[JETKVM_SCREEN_VERSION_CAPACITY - 1U] = '\0';
    destination->device_id[JETKVM_SCREEN_DEVICE_ID_CAPACITY - 1U] = '\0';
    destination->cloud_account_id[JETKVM_SCREEN_METADATA_CAPACITY - 1U] = '\0';
    destination->cloud_domain[JETKVM_SCREEN_METADATA_CAPACITY - 1U] = '\0';
    destination->build_branch[JETKVM_SCREEN_METADATA_CAPACITY - 1U] = '\0';
    destination->build_date[JETKVM_SCREEN_METADATA_CAPACITY - 1U] = '\0';
    destination->runtime_version[JETKVM_SCREEN_METADATA_CAPACITY - 1U] = '\0';
    destination->kernel_version[JETKVM_SCREEN_METADATA_CAPACITY - 1U] = '\0';
    destination->cpu_serial[JETKVM_SCREEN_METADATA_CAPACITY - 1U] = '\0';

    if ((int)destination->cloud_state < (int)JETKVM_SCREEN_CLOUD_NOT_CONFIGURED ||
        destination->cloud_state > JETKVM_SCREEN_CLOUD_CONNECTED) {
        destination->cloud_state = JETKVM_SCREEN_CLOUD_NOT_CONFIGURED;
    }
    if ((int)destination->page < (int)JETKVM_SCREEN_PAGE_AUTO ||
        destination->page > JETKVM_SCREEN_PAGE_DHCP_CLIENT) {
        destination->page = JETKVM_SCREEN_PAGE_AUTO;
    }

}

static void apply_cloud_state(const jetkvm_screen_state_t *state)
{
    char sessions[32];
    (void)snprintf(sessions, sizeof(sessions), "%" PRIu32 " active", state->active_sessions);
    set_label_text(objects.cloud_status_label, sessions);

    switch (state->cloud_state) {
    case JETKVM_SCREEN_CLOUD_NOT_CONFIGURED:
        s_cloud_blink_active = false;
        set_visible(objects.cloud_status_icon, false);
        break;
    case JETKVM_SCREEN_CLOUD_DISCONNECTED:
        s_cloud_blink_active = false;
        set_visible(objects.cloud_status_icon, true);
        lv_image_set_src(objects.cloud_status_icon, &img_cloud_disconnected);
        lv_obj_set_style_opa(objects.cloud_status_icon, LV_OPA_COVER, LV_PART_MAIN);
        break;
    case JETKVM_SCREEN_CLOUD_CONNECTING:
        set_visible(objects.cloud_status_icon, true);
        lv_image_set_src(objects.cloud_status_icon, &img_cloud);
        if (s_rendered_cloud_state != JETKVM_SCREEN_CLOUD_CONNECTING) {
            s_cloud_blink_active = true;
            s_cloud_blink_started_ms = lv_tick_get();
            s_cloud_blink_last_opacity = LV_OPA_COVER;
            lv_obj_set_style_opa(objects.cloud_status_icon, LV_OPA_COVER, LV_PART_MAIN);
        }
        break;
    case JETKVM_SCREEN_CLOUD_CONNECTED:
        s_cloud_blink_active = false;
        set_visible(objects.cloud_status_icon, true);
        lv_image_set_src(objects.cloud_status_icon, &img_cloud);
        lv_obj_set_style_opa(objects.cloud_status_icon, LV_OPA_COVER, LV_PART_MAIN);
        break;
    }
    s_rendered_cloud_state = state->cloud_state;
}

static void tick_cloud_blink(void)
{
    if (!s_cloud_blink_active || objects.cloud_status_icon == NULL) {
        return;
    }

    uint32_t elapsed_ms = lv_tick_elaps(s_cloud_blink_started_ms);
    lv_opa_t opacity = LV_OPA_COVER;
    if (elapsed_ms >= 2000U) {
        uint32_t phase_ms = (elapsed_ms - 2000U) % 2000U;
        if (phase_ms < 1000U) {
            opacity = (lv_opa_t)(255U - (phase_ms * 255U) / 1000U);
        } else {
            opacity = (lv_opa_t)(((phase_ms - 1000U) * 255U) / 1000U);
        }
    }
    if (opacity != s_cloud_blink_last_opacity) {
        lv_obj_set_style_opa(objects.cloud_status_icon, opacity, LV_PART_MAIN);
        s_cloud_blink_last_opacity = opacity;
    }
}

/* The page on the panel, read from LVGL so navigation done by the buttons or
 * the display RPCs counts as much as navigation done through the state. */
static jetkvm_screen_page_t rendered_page(void)
{
    lv_obj_t *active = lv_screen_active();
    lv_obj_t **screens = (lv_obj_t **)&objects;
    for (unsigned i = 0; i < (unsigned)JETKVM_SCREEN_PAGE_DHCP_CLIENT; i++) {
        if (screens[i] == active) {
            return (jetkvm_screen_page_t)(i + 1U);
        }
    }
    if (active == jetkvm_screen_regulatory_screen()) {
        return JETKVM_SCREEN_PAGE_REGULATORY;
    }
    return JETKVM_SCREEN_PAGE_BOOT;
}

#if JETKVM_BOARD_WIFI
/* Show why the last join failed, or hide the line (`text` NULL or ""). */
static void apply_wifi_failure(const char *text)
{
    bool visible = text != NULL && text[0] != '\0';
    if (visible) set_label_text(s_wifi_failure_label, text);
    set_visible(s_wifi_failure_label, visible);
}

/* After the page is chosen: count the failure line as shown when it is on
 * the panel, once per appearance or new text. A line on the no-network
 * screen behind a menu page is not shown. */
static void note_wifi_failure_shown(void)
{
    static bool on_panel;
    bool now = !lv_obj_has_flag(s_wifi_failure_label, LV_OBJ_FLAG_HIDDEN) &&
               lv_screen_active() == objects.no_network_screen;
    const char *text = lv_label_get_text(s_wifi_failure_label);
    portENTER_CRITICAL(&s_wifi_failure_shown_lock);
    if (now && (!on_panel || strcmp(s_wifi_failure_shown, text) != 0)) {
        (void)snprintf(s_wifi_failure_shown, sizeof(s_wifi_failure_shown), "%s", text);
        s_wifi_failure_shows++;
    }
    portEXIT_CRITICAL(&s_wifi_failure_shown_lock);
    on_panel = now;
}
#endif

/* The generated no-network screen has a title and one line of text; the
 * Wi-Fi board fills them from the BLE setup state, and adds the reason of
 * the last failed join below them. */
static void apply_wifi_setup(const jetkvm_screen_state_t *state)
{
#if JETKVM_BOARD_WIFI
    /* The reason stays hidden while a join runs and on the setup code page.
     * A setup device that connects over BLE clears it, so after the code the
     * reason is the failed join of the network that device sent, and it
     * turns the Paired page into "Try again". */
    const char *failure = state->wifi_failure;
    switch (state->wifi_setup) {
    case JETKVM_SCREEN_WIFI_SETUP_CODE:
        set_label_text(objects.no_network_title, state->wifi_setup_code);
        set_label_text(objects.home_info_ipv6_addr_1, "Enter this setup code");
        failure = NULL;
        break;
    case JETKVM_SCREEN_WIFI_SETUP_ACCEPTED:
        /* After a failed join of the network the browser sent: why. */
        set_label_text(objects.no_network_title, failure[0] != '\0' ? "Try again" : "Paired");
        set_label_text(objects.home_info_ipv6_addr_1, "Continue in your browser");
        break;
    case JETKVM_SCREEN_WIFI_SETUP_JOINING: {
        char text[sizeof(state->wifi_joining) + 8];
        snprintf(text, sizeof(text), "Joining %s", state->wifi_joining);
        set_label_text(objects.no_network_title, "Connecting");
        set_label_text(objects.home_info_ipv6_addr_1, text);
        failure = NULL;
        break;
    }
    case JETKVM_SCREEN_WIFI_SETUP_ADVERTISING:
        set_label_text(objects.no_network_title, "Set up Wi-Fi");
        set_label_text(objects.home_info_ipv6_addr_1, "Visit jetkvm.com/wireless");
        break;
    default:
        if (state->wifi_setup_menu || s_wifi_setup_requested) {
            /* Menu setup before the BLE service is up. */
            set_label_text(objects.no_network_title, "Set up Wi-Fi");
            set_label_text(objects.home_info_ipv6_addr_1, "Visit jetkvm.com/wireless");
            break;
        }
        set_label_text(objects.no_network_title, "Connecting");
        if (state->wifi_joining[0] != '\0') {
            char text[sizeof(state->wifi_joining) + 8];
            snprintf(text, sizeof(text), "Joining %s", state->wifi_joining);
            set_label_text(objects.home_info_ipv6_addr_1, text);
            failure = NULL;
        } else {
            set_label_text(objects.home_info_ipv6_addr_1, "Looking for saved Wi-Fi");
        }
        break;
    }
    /* Menu setup was chosen, not caused by a failure; only a join that the
     * setup device sent there has a reason to show. */
    if ((state->wifi_setup_menu || s_wifi_setup_requested) &&
        state->wifi_setup != JETKVM_SCREEN_WIFI_SETUP_ACCEPTED) {
        failure = NULL;
    }
    apply_wifi_failure(failure);
#else
    (void)state;
#endif
}

/* Called on the LVGL owner, including before the first frame reaches GRAM.
 * Poll independently of network updates: failed reset boots never start them. */
static bool apply_reset_status(void)
{
    static bool was_pending;
    static char reboot_text[64];
    bool pending = jetkvm_factory_reset_pending();
    if (pending) {
        if (!was_pending)
            snprintf(reboot_text, sizeof(reboot_text), "%s",
                     lv_label_get_text(objects.reboot_in_progress_label));
        set_label_text(objects.reboot_in_progress_label,
                       jetkvm_factory_reset_error() ? "Reset failed\nReboot to retry" :
                       "Resetting...\nKeep power connected");
        if (rendered_page() != JETKVM_SCREEN_PAGE_REBOOTING)
            loadScreen(SCREEN_ID_REBOOTING_SCREEN);
    } else if (was_pending) {
        set_label_text(objects.reboot_in_progress_label, reboot_text);
        loadScreen(SCREEN_ID_BOOT_SCREEN);
    }
    was_pending = pending;
    return pending;
}

static void apply_page(const jetkvm_screen_state_t *state)
{
    if (apply_reset_status()) return;
    jetkvm_screen_page_t page = state->page;
    jetkvm_screen_page_t rendered = rendered_page();
    /* Menu setup shows its page even before the device leaves the network. */
    bool home = state->network_connected && !state->wifi_setup_menu && !s_wifi_setup_requested;
    set_var_main_screen(home ? "home_screen" : "no_network_screen");

    if (page == JETKVM_SCREEN_PAGE_AUTO) {
        /*
         * Match display.go's SwitchToScreenIf behavior: a network transition
         * may replace only boot/home/no-network.  Menu, status, about, and
         * confirmation pages remain where the user or controller put them.
         */
        if (rendered != JETKVM_SCREEN_PAGE_BOOT &&
            rendered != JETKVM_SCREEN_PAGE_HOME &&
            rendered != JETKVM_SCREEN_PAGE_NO_NETWORK) {
            return;
        }
        page = home ? JETKVM_SCREEN_PAGE_HOME : JETKVM_SCREEN_PAGE_NO_NETWORK;
    }

    if (page != rendered) {
        loadScreen((enum ScreensEnum)page);
    }
}

static void apply_state(const jetkvm_screen_state_t *state)
{
    set_var_app_version(state->app_version);
    set_var_system_version(state->system_version);

    set_label_text(objects.home_info_ipv4_addr,
                   state->ipv4[0] != '\0' ? state->ipv4 : "0.0.0.0");
    if (state->wifi_connection) {
        set_wifi_status(true, state->wifi_ssid, state->wifi_signal_valid,
                        state->wifi_rssi);
    } else {
        set_wifi_status(false, NULL, false, 0);
        set_optional_label(objects.home_info_ipv6_addr, NULL, state->ipv6);
        set_label_text(objects.home_info_mac_addr, state->mac);
        lv_obj_set_style_text_color(objects.home_info_mac_addr,
                                    lv_color_hex(0xffffff), LV_PART_MAIN);
    }

    /* The start reached the state, also when setup has already ended. */
    if (state->wifi_setup_menu_starts != s_wifi_setup_menu_starts) s_wifi_setup_requested = false;
    s_wifi_setup_menu_starts = state->wifi_setup_menu_starts;
    s_wifi_connected = state->wifi_connection && state->network_connected;
    memcpy(s_wifi_connected_ssid, state->wifi_ssid, sizeof(s_wifi_connected_ssid));
    apply_cloud_state(state);
    apply_wifi_setup(state);
    jetkvm_screen_buttons_set_wifi_setup(state->wifi_setup_menu || s_wifi_setup_requested,
                                         state->wifi_setup_stoppable);

    /* Match JetKVM display.go: the unfinished Access menu stays hidden. The
     * Network entry is Set up Wi-Fi on the Wi-Fi board. */
    set_visible(objects.menu_btn_network, JETKVM_BOARD_WIFI);
    set_visible(objects.menu_btn_access, false);

    set_connection_label(objects.usb_status_label, state->usb_connected);
    __atomic_store_n(&s_rendered_usb_connected, state->usb_connected,
                     __ATOMIC_RELEASE);
    set_connection_label(objects.hdmi_status_label, state->hdmi_connected);
    jetkvm_screen_usb_debugging_apply(state->usb_debugging_enabled, state->usb_debugging_active);

    set_label_text(objects.device_id, state->device_id);
    set_optional_label(objects.app_version_1, objects.cloud_account_id_container,
                       state->cloud_account_id);
    set_optional_label(objects.cloud_domain, objects.cloud_domain_container,
                       state->cloud_domain);

    set_optional_label(objects.build_branch, objects.build_branch_container,
                       state->build_branch);
    set_optional_label(objects.build_date, objects.build_date_container, state->build_date);
    set_optional_label(objects.golang_version, objects.golang_version_container,
                       state->runtime_version);
    set_optional_label(objects.kernel_version, objects.kernel_version_container,
                       state->kernel_version);
    set_optional_label(objects.cpu_serial, objects.cpu_serial_container, state->cpu_serial);

    tick_screen_boot_screen();
    tick_screen_about_screen();
    apply_page(state);
#if JETKVM_BOARD_WIFI
    note_wifi_failure_shown();
#endif
    __atomic_fetch_add(&s_updates_applied, 1U, __ATOMIC_RELAXED);
}

static bool screen_color_transfer_done(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_io_event_data_t *event_data,
                                       void *user_context)
{
    (void)panel_io;
    (void)event_data;
    screen_context_t *screen = (screen_context_t *)user_context;
    BaseType_t task_woken = pdFALSE;
    if (screen != NULL) {
        screen_transfer_owner_t owner = (screen_transfer_owner_t)
            __atomic_exchange_n(&screen->transfer_owner,
                                SCREEN_TRANSFER_NONE, __ATOMIC_ACQ_REL);
        __atomic_store_n(&screen->flush_inflight, false, __ATOMIC_RELEASE);
        if (owner == SCREEN_TRANSFER_LVGL && screen->display != NULL) {
            lv_display_flush_ready(screen->display);
        } else if (owner == SCREEN_TRANSFER_PREVIEW) {
            __atomic_store_n(&s_preview_active, true, __ATOMIC_RELEASE);
            __atomic_fetch_add(&s_preview_frames_applied, 1U,
                               __ATOMIC_RELAXED);
        }
        if (screen->flush_done != NULL) {
            xSemaphoreGiveFromISR(screen->flush_done, &task_woken);
        }
    }
    return task_woken == pdTRUE;
}

static void screen_flush_wait(lv_display_t *display)
{
    screen_context_t *screen = (screen_context_t *)lv_display_get_user_data(display);
    if (screen == NULL || screen->flush_done == NULL) {
        return;
    }

    /* The semaphore is an idle-token: every transfer takes it and the ISR
     * returns it. LVGL only wants to wait here, so put the token back for the
     * next flush after the outstanding DMA has completed. */
    if (xSemaphoreTake(screen->flush_done, portMAX_DELAY) == pdTRUE) {
        xSemaphoreGive(screen->flush_done);
    }
}

static esp_err_t screen_transfer_acquire(screen_context_t *screen,
                                         TickType_t timeout_ticks)
{
    if (screen == NULL || screen->flush_done == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(screen->flush_done, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t screen_wait_for_transfer(screen_context_t *screen,
                                          TickType_t timeout_ticks)
{
    esp_err_t error = screen_transfer_acquire(screen, timeout_ticks);
    if (error == ESP_OK) {
        xSemaphoreGive(screen->flush_done);
    }
    return error;
}

static void screen_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixel_map)
{
    screen_context_t *screen = (screen_context_t *)lv_display_get_user_data(display);
    if (screen == NULL || screen->panel == NULL || screen->viewport_buffer == NULL ||
        pixel_map == NULL || area == NULL || area->x1 < 0 ||
        area->x2 >= (int32_t)JETKVM_SCREEN_SOURCE_WIDTH || area->x1 > area->x2 ||
        area->y1 < 0 || area->y2 >= (int32_t)JETKVM_SCREEN_SOURCE_HEIGHT ||
        area->y1 > area->y2) {
        __atomic_store_n(&s_last_error, ESP_ERR_INVALID_ARG, __ATOMIC_RELAXED);
        lv_display_flush_ready(display);
        return;
    }

    jetkvm_screen_fit_span_t panel_span;
    if (!jetkvm_screen_fit_map_span(area->x1, area->x2, &panel_span)) {
        lv_display_flush_ready(display);
        return;
    }

    uint16_t rows = (uint16_t)(area->y2 - area->y1 + 1);
    size_t source_width = (size_t)area->x2 - area->x1 + 1U;
    size_t panel_width = (size_t)panel_span.x2 - panel_span.x1 + 1U;
    size_t source_pixels = source_width * rows;
    size_t panel_pixels = panel_width * rows;
    if (source_pixels > JETKVM_SCREEN_FLUSH_BUFFER_PIXELS ||
        panel_pixels > screen->viewport_buffer_pixels) {
        __atomic_store_n(&s_last_error, ESP_ERR_INVALID_SIZE, __ATOMIC_RELAXED);
        ESP_LOGE(TAG,
                 "invalid partial flush: source=%ux%u (%u px), viewport=%ux%u (%u px)",
                 (unsigned)source_width, (unsigned)rows, (unsigned)source_pixels,
                 (unsigned)panel_width, (unsigned)rows, (unsigned)panel_pixels);
        lv_display_flush_ready(display);
        return;
    }

    jetkvm_screen_fit_copy_rgb565_be((const uint16_t *)pixel_map,
                                     (uint16_t)area->x1,
                                     (uint16_t)area->x2,
                                     rows,
                                     screen->viewport_buffer,
                                     panel_span);

    esp_err_t error = screen_transfer_acquire(screen, portMAX_DELAY);
    if (error != ESP_OK) {
        __atomic_store_n(&s_last_error, error, __ATOMIC_RELAXED);
        lv_display_flush_ready(display);
        return;
    }
    __atomic_store_n(&screen->transfer_owner, SCREEN_TRANSFER_LVGL,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&screen->flush_inflight, true, __ATOMIC_RELEASE);
    error = esp_lcd_panel_draw_bitmap(screen->panel,
                                      panel_span.x1,
                                      area->y1,
                                      panel_span.x2 + 1U,
                                      area->y2 + 1,
                                      screen->viewport_buffer);
    if (error != ESP_OK) {
        __atomic_store_n(&screen->transfer_owner, SCREEN_TRANSFER_NONE,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&screen->flush_inflight, false, __ATOMIC_RELEASE);
        __atomic_store_n(&s_last_error, error, __ATOMIC_RELAXED);
        ESP_LOGE(TAG, "panel flush failed: %s", esp_err_to_name(error));
        xSemaphoreGive(screen->flush_done);
        lv_display_flush_ready(display);
    }
}

/* The display settings mutex is always acquired before this panel mutex.
 * The LVGL task must not acquire the settings mutex while holding the panel. */
static esp_err_t screen_display_rotation(uint16_t rotation)
{
    /* Upstream calls 270 Normal and 90 Inverted. Offset that convention to
     * this board's configured normal mounting orientation. */
    uint16_t degrees = (rotation + 90U + SCREEN_PANEL_ROTATION_DEGREES) % 360U;
    if (!s_screen.preview_mutex || xSemaphoreTake(s_screen.preview_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    esp_err_t error = screen_wait_for_transfer(&s_screen, pdMS_TO_TICKS(500));
    jetkvm_screen_panel_orientation_t o = jetkvm_screen_panel_orientation(degrees);
    if (error == ESP_OK) error = esp_lcd_panel_swap_xy(s_screen.panel, o.swap_xy);
    if (error == ESP_OK) error = esp_lcd_panel_mirror(s_screen.panel, o.mirror_x, o.mirror_y);
    if (error == ESP_OK) error = esp_lcd_panel_set_gap(s_screen.panel, o.x_gap, o.y_gap);
    if (error == ESP_OK) {
        s_screen.preview_layout_valid = false;
        s_screen.ui_degrees = degrees;
        s_screen.ui_redraw_requested = true;
        ESP_LOGI(TAG, "Local display rotation applied: %u", degrees);
    }
    xSemaphoreGive(s_screen.preview_mutex);
    return error;
}
/* Called only by the LVGL owner with preview_mutex held, outside direct mode. */
static void screen_apply_pending_redraw(void)
{
    if (s_screen.ui_redraw_requested) {
        /* The button pillar stays beside the buttons as the picture turns. */
        jetkvm_screen_buttons_set_rotation(s_screen.ui_degrees);
        lv_obj_invalidate(lv_screen_active());
        s_screen.ui_redraw_requested = false;
    }
}
static esp_err_t screen_display_brightness(uint32_t value)
{
    return screen_backlight_set_duty((value * SCREEN_BACKLIGHT_DUTY_MAX) / 255U);
}
static esp_err_t screen_display_active(bool active)
{
    if (!s_screen.preview_mutex || xSemaphoreTake(s_screen.preview_mutex, pdMS_TO_TICKS(1000)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    esp_err_t error = screen_wait_for_transfer(&s_screen, pdMS_TO_TICKS(500));
    if (error == ESP_OK) error = esp_lcd_panel_disp_on_off(s_screen.panel, active);
    xSemaphoreGive(s_screen.preview_mutex);
    return error;
}

static esp_err_t screen_hardware_init(screen_context_t *screen)
{
    esp_err_t error;
    ledc_timer_config_t backlight_timer = {
        .speed_mode = SCREEN_BACKLIGHT_SPEED_MODE,
        .duty_resolution = SCREEN_BACKLIGHT_DUTY_RESOLUTION,
        .timer_num = SCREEN_BACKLIGHT_TIMER,
        .freq_hz = SCREEN_BACKLIGHT_PWM_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    error = ledc_timer_config(&backlight_timer);
    if (error != ESP_OK) {
        return error;
    }
    ledc_channel_config_t backlight_channel = {
        .gpio_num = SCREEN_PIN_BACKLIGHT,
        .speed_mode = SCREEN_BACKLIGHT_SPEED_MODE,
        .channel = SCREEN_BACKLIGHT_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = SCREEN_BACKLIGHT_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    error = ledc_channel_config(&backlight_channel);
    if (error != ESP_OK) {
        return error;
    }

    spi_bus_config_t bus_config = {
        .sclk_io_num = SCREEN_PIN_SCLK,
        .mosi_io_num = SCREEN_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SCREEN_PREVIEW_BYTES + 8U,
    };
    error = spi_bus_initialize(SCREEN_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (error != ESP_OK) {
        return error;
    }
    screen->spi_bus_initialized = true;

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = SCREEN_PIN_DC,
        .cs_gpio_num = SCREEN_PIN_CS,
        .pclk_hz = SCREEN_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = SCREEN_LCD_CMD_BITS,
        .lcd_param_bits = SCREEN_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = screen_color_transfer_done,
        .user_ctx = screen,
        .flags.psram_dma_direct = true,
    };
    error = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SCREEN_LCD_HOST,
                                     &io_config, &screen->io);
    if (error != ESP_OK) {
        return error;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = SCREEN_PIN_RST,
        /* JetKVM's DTS omits `bgr`, so its ST7789 MADCTL uses RGB order. */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    error = esp_lcd_new_panel_st7789(screen->io, &panel_config, &screen->panel);
    if (error != ESP_OK) {
        return error;
    }
    if ((error = esp_lcd_panel_reset(screen->panel)) != ESP_OK ||
        (error = esp_lcd_panel_init(screen->panel)) != ESP_OK ||
        (error = esp_lcd_panel_set_gap(screen->panel,
                                       SCREEN_PANEL_X_GAP,
                                       SCREEN_PANEL_Y_GAP)) != ESP_OK ||
        (error = esp_lcd_panel_invert_color(screen->panel, true)) != ESP_OK ||
        (error = esp_lcd_panel_swap_xy(screen->panel,
                                       SCREEN_PANEL_SWAP_XY)) != ESP_OK ||
        (error = esp_lcd_panel_mirror(screen->panel,
                                      SCREEN_PANEL_MIRROR_X,
                                      SCREEN_PANEL_MIRROR_Y)) != ESP_OK ||
        (error = esp_lcd_panel_disp_on_off(screen->panel, true)) != ESP_OK) {
        return error;
    }

    const size_t render_pixels = JETKVM_SCREEN_FLUSH_BUFFER_PIXELS;
    /* LVGL renders with the CPU and screen_flush() copies each completed
     * strip into the separate internal-DMA viewport before SPI owns it. Keep
     * only that viewport in scarce internal SRAM. */
    screen->render_buffer = heap_caps_malloc(render_pixels * sizeof(uint16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    screen->viewport_buffer_pixels = JETKVM_SCREEN_FLUSH_BUFFER_PIXELS;
    screen->viewport_buffer = heap_caps_malloc(screen->viewport_buffer_pixels *
                                               sizeof(uint16_t),
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    screen->flush_done = xSemaphoreCreateBinary();
    if (screen->render_buffer == NULL || screen->viewport_buffer == NULL ||
        screen->flush_done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(screen->flush_done);
    error = screen_preview_resources_init(screen);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "HDMI preview resources unavailable: %s",
                 esp_err_to_name(error));
        return error;
    }

    error = local_display_prepare();
    if (error != ESP_OK) return error;

    lv_init();
    lv_tick_set_cb(screen_tick_ms);
    screen->display = lv_display_create(JETKVM_SCREEN_SOURCE_WIDTH,
                                        JETKVM_SCREEN_SOURCE_HEIGHT);
    if (screen->display == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_user_data(screen->display, screen);
    lv_display_set_color_format(screen->display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(screen->display, screen_flush);
    lv_display_set_flush_wait_cb(screen->display, screen_flush_wait);
    lv_display_set_buffers(screen->display,
                           screen->render_buffer,
                           NULL,
                           render_pixels * sizeof(uint16_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    ui_init();
    ui_set_rpc_handler(screen_rpc_handler);
    about_rows_init();
    wifi_status_init();
    wifi_setup_menu_init();
    jetkvm_screen_usb_debugging_init();
    jetkvm_screen_regulatory_init();
    jetkvm_screen_state_t initial_state = {
        .cloud_state = JETKVM_SCREEN_CLOUD_NOT_CONFIGURED,
        .usb_connected = false,
        .usb_debugging_enabled = jetkvm_usb_debugging_enabled(),
        .usb_debugging_active = jetkvm_usb_debugging_active(),
        .page = JETKVM_SCREEN_PAGE_BOOT,
    };
    apply_state(&initial_state);

    /*
     * Keep the backlight dark until the complete first LVGL frame has reached
     * ST7789 GRAM.  lv_refr_now() waits between single-buffer chunks; the last
     * SPI DMA is asynchronous, so wait for its completion explicitly.
     */
    lv_refr_now(screen->display);
    if (screen_wait_for_transfer(
            screen, pdMS_TO_TICKS(SCREEN_FIRST_REFRESH_TIMEOUT_MS)) != ESP_OK) {
        __atomic_store_n(&s_last_error, ESP_ERR_TIMEOUT, __ATOMIC_RELAXED);
        ESP_LOGE(TAG, "first LCD refresh did not complete");
        return ESP_ERR_TIMEOUT;
    }

    error = local_display_ready();
    if (error == ESP_OK) error = local_display_active(true);
    if (error == ESP_OK) {
        /* Only now: a failure above ends this task, and an armed button
         * interrupt would notify a task that no longer exists. */
        jetkvm_screen_buttons_init(xTaskGetCurrentTaskHandle(), SCREEN_NOTIFY_BUTTON);
        local_display_state_t display = {0};
        (void)local_display_state(&display);
        ESP_LOGI(TAG,
                 "ST7789P3 ready: SPI2 20MHz 240x240 RGB, "
                 "hardware rotation=%u, backlight=%u/255, "
                 "MOSI=%d SCLK=%d DC=%d RST=%d CS=%d BL=%d, "
                 "native 240x240 UI canvas",
                 (unsigned)((display.rotation + 90U + SCREEN_PANEL_ROTATION_DEGREES) % 360U),
                 (unsigned)display.brightness,
                 SCREEN_PIN_MOSI, SCREEN_PIN_SCLK, SCREEN_PIN_DC,
                 SCREEN_PIN_RST, SCREEN_PIN_CS, SCREEN_PIN_BACKLIGHT);
    }
    return error;
}

static bool screen_set_active(bool active)
{
    esp_err_t error = local_display_active(active);
    if (error != ESP_OK) {
        __atomic_store_n(&s_last_error, error, __ATOMIC_RELAXED);
        return false;
    }
    __atomic_store_n(&s_last_error, ESP_OK, __ATOMIC_RELAXED);
    xEventGroupClearBits(s_events, active ? SCREEN_EVENT_STOPPED : SCREEN_EVENT_ACTIVE);
    xEventGroupSetBits(s_events, active ? SCREEN_EVENT_ACTIVE : SCREEN_EVENT_STOPPED);
    return true;
}

static void screen_task(void *argument)
{
    (void)argument;
    esp_err_t error = screen_hardware_init(&s_screen);
    __atomic_store_n(&s_last_error, error, __ATOMIC_RELAXED);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "initialization failed: %s", esp_err_to_name(error));
        xEventGroupSetBits(s_events, SCREEN_EVENT_FAILED | SCREEN_EVENT_STOPPED);
        s_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    /* Hardware initialization has already completed the first activation. */
    xEventGroupSetBits(s_events, SCREEN_EVENT_READY | SCREEN_EVENT_ACTIVE);

    bool active = true;
    bool requested_active = true;
    uint32_t next_delay_ms = SCREEN_LOOP_MIN_DELAY_MS;
    for (;;) {
        uint32_t notifications = 0;
        TickType_t wait_ticks = portMAX_DELAY;
        if (active || requested_active != active) {
            wait_ticks = pdMS_TO_TICKS(next_delay_ms);
            if (wait_ticks < 1U) {
                wait_ticks = 1U;
            }
        }
        (void)xTaskNotifyWait(0, UINT32_MAX, &notifications, wait_ticks);

        if ((notifications & SCREEN_NOTIFY_STOP) != 0U) {
            requested_active = false;
        }
        if ((notifications & SCREEN_NOTIFY_START) != 0U) {
            requested_active = true;
        }

        if (requested_active != active && screen_set_active(requested_active)) {
            active = requested_active;
        }
        if (!active) {
            continue;
        }

        local_display_tick();

        /* HDMI preview owns the panel directly. The mutex prevents LVGL from
         * issuing a transfer while PPA/SPI direct mode is active. */
        if (xSemaphoreTake(s_screen.preview_mutex, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        bool preview_direct = __atomic_load_n(&s_screen.preview_direct_mode,
                                              __ATOMIC_ACQUIRE);
        bool preview_stop = __atomic_load_n(&s_preview_stop_requested,
                                            __ATOMIC_ACQUIRE);
        if (preview_direct && !preview_stop) {
            (void)jetkvm_screen_buttons_poll(true);
            next_delay_ms = SCREEN_LOOP_MIN_DELAY_MS;
            xSemaphoreGive(s_screen.preview_mutex);
            continue;
        }
        if (preview_direct && preview_stop) {
            esp_err_t wait_error = screen_wait_for_transfer(
                &s_screen, pdMS_TO_TICKS(SCREEN_STOP_TIMEOUT_MS));
            if (wait_error != ESP_OK) {
                __atomic_store_n(&s_last_error, wait_error,
                                 __ATOMIC_RELAXED);
                ESP_LOGE(TAG, "preview SPI drain failed: %s",
                         esp_err_to_name(wait_error));
                xSemaphoreGive(s_screen.preview_mutex);
                continue;
            }
            __atomic_store_n(&s_screen.preview_direct_mode, false,
                             __ATOMIC_RELEASE);
            s_screen.preview_layout_valid = false;
            __atomic_store_n(&s_preview_active, false, __ATOMIC_RELEASE);
            __atomic_store_n(&s_preview_stop_requested, false,
                             __ATOMIC_RELEASE);
            s_screen.ui_redraw_requested = true;
        } else if (preview_stop) {
            s_screen.preview_layout_valid = false;
            __atomic_store_n(&s_preview_stop_requested, false,
                             __ATOMIC_RELEASE);
            __atomic_store_n(&s_preview_active, false, __ATOMIC_RELEASE);
        }
        screen_apply_pending_redraw();
        jetkvm_screen_state_t state;
        if (xQueueReceive(s_state_queue, &state, 0) == pdTRUE) {
            apply_state(&state);
        }
        if (__atomic_exchange_n(&s_rpc_failed, false, __ATOMIC_ACQ_REL)) {
            screen_rpc_restore();
        }
        tick_cloud_blink();
        bool resetting = apply_reset_status();
        bool buttons_busy = !resetting && jetkvm_screen_buttons_poll(false);
        if (!resetting) ui_tick();
        next_delay_ms = lv_timer_handler();
        if (buttons_busy) {
            /* Debounce and hold progress need the loop at its shortest cadence. */
            next_delay_ms = SCREEN_LOOP_MIN_DELAY_MS;
        }
        if (next_delay_ms < SCREEN_LOOP_MIN_DELAY_MS) {
            next_delay_ms = SCREEN_LOOP_MIN_DELAY_MS;
        } else if (next_delay_ms > SCREEN_LOOP_MAX_DELAY_MS) {
            next_delay_ms = SCREEN_LOOP_MAX_DELAY_MS;
        }
        xSemaphoreGive(s_screen.preview_mutex);
    }
}

esp_err_t jetkvm_screen_start(void)
{
    const local_display_driver_t driver = {
        .rotation = screen_display_rotation, .brightness = screen_display_brightness,
        .active = screen_display_active,
    };
    esp_err_t settings_error = local_display_settings_init(
        270, &driver, !jetkvm_factory_reset_pending());
    if (settings_error != ESP_OK) return settings_error;
    if (s_task != NULL) {
        if (xTaskNotify(s_task, SCREEN_NOTIFY_START, eSetBits) != pdPASS) {
            return ESP_FAIL;
        }
        EventBits_t bits = xEventGroupWaitBits(s_events, SCREEN_EVENT_ACTIVE,
                                               pdFALSE, pdTRUE,
                                               pdMS_TO_TICKS(SCREEN_START_TIMEOUT_MS));
        return (bits & SCREEN_EVENT_ACTIVE) != 0U ? ESP_OK : ESP_ERR_TIMEOUT;
    }

    if (s_events != NULL &&
        (xEventGroupGetBits(s_events) & SCREEN_EVENT_FAILED) != 0U) {
        return (esp_err_t)__atomic_load_n(&s_last_error, __ATOMIC_RELAXED);
    }

    if (s_state_queue == NULL) {
        s_state_queue = xQueueCreate(1, sizeof(jetkvm_screen_state_t));
    }
    if (s_events == NULL) {
        s_events = xEventGroupCreate();
    } else {
        xEventGroupClearBits(s_events, UINT32_MAX);
    }
    if (s_state_queue == NULL || s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* UI/LVGL never runs from an ISR or while the external-memory cache is
     * disabled. Keep its honestly sized stack in PSRAM so it cannot consume
     * the internal/DMA floor needed by HDMI, USB and DTLS. */
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        screen_task, "jetkvm_screen", SCREEN_TASK_STACK_BYTES, NULL,
        SCREEN_TASK_PRIORITY, &s_task, SCREEN_TASK_CORE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events,
                                           SCREEN_EVENT_READY | SCREEN_EVENT_FAILED,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(SCREEN_START_TIMEOUT_MS));
    if ((bits & SCREEN_EVENT_FAILED) != 0U) {
        return (esp_err_t)__atomic_load_n(&s_last_error, __ATOMIC_RELAXED);
    }
    if ((bits & SCREEN_EVENT_READY) == 0U) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t jetkvm_screen_stop(void)
{
    TaskHandle_t task = s_task;
    if (task == NULL) {
        return ESP_OK;
    }
    if (xTaskNotify(task, SCREEN_NOTIFY_STOP, eSetBits) != pdPASS) {
        return ESP_FAIL;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, SCREEN_EVENT_STOPPED,
                                           pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(SCREEN_STOP_TIMEOUT_MS));
    return (bits & SCREEN_EVENT_STOPPED) != 0U ? ESP_OK : ESP_ERR_TIMEOUT;
}

void jetkvm_screen_set_wifi_setup_handler(jetkvm_screen_wifi_setup_handler_t handler)
{
    __atomic_store_n(&s_wifi_setup_handler, handler, __ATOMIC_RELEASE);
}

esp_err_t jetkvm_screen_update(const jetkvm_screen_state_t *state)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    TaskHandle_t task = s_task;
    QueueHandle_t queue = s_state_queue;
    if (task == NULL || queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    static uint32_t previous_connection_state;
    uint32_t connection_state = (state->active_sessions << 2) |
        ((uint32_t)state->hdmi_connected << 1) | state->network_connected;
    if (__atomic_exchange_n(&previous_connection_state, connection_state, __ATOMIC_ACQ_REL) != connection_state)
        local_display_wake();
    jetkvm_screen_state_t normalized;
    normalize_state(&normalized, state);
    /* Direct preview bypasses the LVGL task, so publish this one overlay bit
     * here as well as queuing the complete UI state. */
    __atomic_store_n(&s_preview_network_connected,
                     normalized.network_connected, __ATOMIC_RELEASE);
    if (uxQueueMessagesWaiting(queue) != 0U) {
        __atomic_fetch_add(&s_pending_replacements, 1U, __ATOMIC_RELAXED);
    }
    if (xQueueOverwrite(queue, &normalized) != pdPASS) {
        return ESP_FAIL;
    }
    __atomic_fetch_add(&s_updates_submitted, 1U, __ATOMIC_RELAXED);
    (void)xTaskNotify(task, SCREEN_NOTIFY_UPDATE, eSetBits);
    return ESP_OK;
}

static esp_err_t screen_preview_submit_yuv(const uint8_t *pixels,
                                           size_t size,
                                           uint16_t width,
                                           uint16_t height,
                                           uint16_t fps,
                                           ppa_srm_color_mode_t input_format,
                                           size_t required_size)
{
    if (pixels == NULL || width == 0U || height == 0U ||
        (width & 1U) != 0U || (height & 1U) != 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (required_size > size) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_task == NULL || s_screen.preview_mutex == NULL ||
        s_screen.preview_ppa == NULL ||
        s_screen.preview_buffers[0] == NULL ||
        s_screen.preview_buffers[1] == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t now_ms = screen_tick_ms();
#if SCREEN_PREVIEW_INTERVAL_MS > 0
    uint32_t last_attempt_ms = __atomic_load_n(&s_preview_last_attempt_ms,
                                                __ATOMIC_RELAXED);
    if (last_attempt_ms != 0U &&
        now_ms - last_attempt_ms < SCREEN_PREVIEW_INTERVAL_MS) {
        __atomic_fetch_add(&s_preview_frames_dropped, 1U, __ATOMIC_RELAXED);
        return ESP_ERR_NOT_FINISHED;
    }
#endif
    __atomic_store_n(&s_preview_last_attempt_ms, now_ms, __ATOMIC_RELAXED);

    if (xSemaphoreTake(s_screen.preview_mutex, 0) != pdTRUE) {
        __atomic_fetch_add(&s_preview_frames_dropped, 1U, __ATOMIC_RELAXED);
        return ESP_ERR_NOT_FINISHED;
    }

    esp_err_t error = ESP_OK;
    uint32_t preview_epoch = __atomic_load_n(&s_preview_epoch,
                                             __ATOMIC_ACQUIRE);
    if (__atomic_load_n(&s_preview_stop_requested, __ATOMIC_ACQUIRE)) {
        error = ESP_ERR_INVALID_STATE;
        goto out;
    }
    /* PPA scaling has 1/16-pixel ratio precision. Pick the largest common
     * ratio that fits both axes so HDMI geometry is never stretched. */
    uint32_t scale_units_x = SCREEN_PREVIEW_WIDTH * 16U / width;
    uint32_t scale_units_y = SCREEN_PREVIEW_HEIGHT * 16U / height;
    uint32_t scale_units = scale_units_x < scale_units_y ?
                           scale_units_x : scale_units_y;
    if (scale_units == 0U) {
        error = ESP_ERR_NOT_SUPPORTED;
        goto out;
    }
    uint32_t output_width = (uint32_t)width * scale_units / 16U;
    uint32_t output_height = (uint32_t)height * scale_units / 16U;
    if (output_width == 0U || output_width > SCREEN_PREVIEW_WIDTH ||
        output_height == 0U || output_height > SCREEN_PREVIEW_HEIGHT) {
        error = ESP_ERR_NOT_SUPPORTED;
        goto out;
    }
    uint32_t output_x = (SCREEN_PREVIEW_WIDTH - output_width) / 2U;
    uint32_t output_y = (SCREEN_PREVIEW_HEIGHT - output_height) / 2U;

    /* A full-width landscape image is contiguous in the 240-pixel output
     * buffer, so steady frames can update only the rows occupied by video.
     * The first frame (and every layout/OSD change) remains a full refresh to
     * clear the previous UI and establish black letterbox bars. For unusual
     * portrait inputs the active rectangle has a row stride, so retain the
     * correct full-frame fallback instead of sending discontiguous pixels. */
    bool partial_capable = output_width == SCREEN_PREVIEW_WIDTH;
    uint32_t steady_transfer_height = partial_capable ? output_height :
                                                        SCREEN_PREVIEW_HEIGHT;
    bool layout_changed = !s_screen.preview_layout_valid ||
        !__atomic_load_n(&s_screen.preview_direct_mode, __ATOMIC_ACQUIRE) ||
        s_screen.preview_layout_width != width ||
        s_screen.preview_layout_height != height ||
        s_screen.preview_layout_fps != fps;
    bool partial_refresh = !layout_changed && partial_capable;
    uint32_t transfer_y = partial_refresh ? output_y : 0U;
    uint32_t transfer_height = partial_refresh ? output_height :
                                                 SCREEN_PREVIEW_HEIGHT;

    uint8_t output_index = s_screen.preview_next_buffer;
    uint16_t *output_buffer = s_screen.preview_buffers[output_index];
    /* Only touch the buffer not owned by SPI. Steady partial frames do not
     * resend letterbox rows, so they need no full-buffer clear. */
    if (!partial_refresh) {
        memset(output_buffer, 0, SCREEN_PREVIEW_BYTES);
    }

    ppa_srm_oper_config_t operation = {
        .in = {
            .buffer = pixels,
            .pic_w = width,
            .pic_h = height,
            .block_w = width,
            .block_h = height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = input_format,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT709,
        },
        .out = {
            .buffer = output_buffer,
            .buffer_size = SCREEN_PREVIEW_BYTES,
            .pic_w = SCREEN_PREVIEW_WIDTH,
            .pic_h = SCREEN_PREVIEW_HEIGHT,
            .block_offset_x = output_x,
            .block_offset_y = output_y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)scale_units / 16.0f,
        .scale_y = (float)scale_units / 16.0f,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    int64_t ppa_started_us = esp_timer_get_time();
    error = ppa_do_scale_rotate_mirror(s_screen.preview_ppa, &operation);
    if (error != ESP_OK) {
        goto out;
    }
    uint32_t ppa_elapsed_us = (uint32_t)(esp_timer_get_time() - ppa_started_us);
    __atomic_fetch_add(&s_preview_ppa_frames, 1U, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_preview_ppa_total_us, ppa_elapsed_us,
                       __ATOMIC_RELAXED);
    uint32_t ppa_max_us = __atomic_load_n(&s_preview_ppa_max_us,
                                          __ATOMIC_RELAXED);
    while (ppa_elapsed_us > ppa_max_us &&
           !__atomic_compare_exchange_n(&s_preview_ppa_max_us, &ppa_max_us,
                                        ppa_elapsed_us, true,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
    if (preview_epoch != __atomic_load_n(&s_preview_epoch,
                                         __ATOMIC_ACQUIRE)) {
        error = ESP_ERR_INVALID_STATE;
        goto out;
    }

    __atomic_store_n(&s_screen.preview_direct_mode, true,
                     __ATOMIC_RELEASE);
    screen_preview_draw_osd(output_buffer, width, height, fps);
    uint16_t *transfer_buffer = output_buffer +
        transfer_y * SCREEN_PREVIEW_WIDTH;
    size_t transfer_pixels = (size_t)SCREEN_PREVIEW_WIDTH * transfer_height;
    screen_preview_swap_rgb565_bytes(transfer_buffer, transfer_pixels);
    error = screen_transfer_acquire(&s_screen,
                                    pdMS_TO_TICKS(SCREEN_STOP_TIMEOUT_MS));
    if (error != ESP_OK) {
        goto out;
    }
    if (preview_epoch != __atomic_load_n(&s_preview_epoch,
                                         __ATOMIC_ACQUIRE)) {
        error = ESP_ERR_INVALID_STATE;
        goto out;
    }

    __atomic_store_n(&s_screen.transfer_owner, SCREEN_TRANSFER_PREVIEW,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&s_screen.flush_inflight, true, __ATOMIC_RELEASE);
    error = esp_lcd_panel_draw_bitmap(s_screen.panel, 0, transfer_y,
                                      SCREEN_PREVIEW_WIDTH,
                                      transfer_y + transfer_height,
                                      transfer_buffer);
    if (error != ESP_OK) {
        __atomic_store_n(&s_screen.transfer_owner, SCREEN_TRANSFER_NONE,
                         __ATOMIC_RELEASE);
        __atomic_store_n(&s_screen.flush_inflight, false,
                         __ATOMIC_RELEASE);
        xSemaphoreGive(s_screen.flush_done);
        goto out;
    }
    if (layout_changed) {
        ESP_LOGI(TAG,
                 "preview layout input=%ux%u@%u output=%ux%u+%u+%u steady_spi=%ux%u (%u bytes)",
                 (unsigned)width, (unsigned)height, (unsigned)fps,
                 (unsigned)output_width, (unsigned)output_height,
                 (unsigned)output_x, (unsigned)output_y,
                 (unsigned)SCREEN_PREVIEW_WIDTH,
                 (unsigned)steady_transfer_height,
                 (unsigned)(steady_transfer_height * SCREEN_PREVIEW_WIDTH *
                            sizeof(uint16_t)));
    }
    s_screen.preview_layout_valid = true;
    s_screen.preview_layout_width = width;
    s_screen.preview_layout_height = height;
    s_screen.preview_layout_fps = fps;
    s_screen.preview_next_buffer =
        (uint8_t)((output_index + 1U) % SCREEN_PREVIEW_BUFFER_COUNT);
    __atomic_store_n(&s_preview_width, width, __ATOMIC_RELAXED);
    __atomic_store_n(&s_preview_height, height, __ATOMIC_RELAXED);
    __atomic_store_n(&s_preview_fps, fps, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_preview_frames_submitted, 1U, __ATOMIC_RELAXED);

out:
    xSemaphoreGive(s_screen.preview_mutex);
    if (error == ESP_OK) {
        (void)xTaskNotify(s_task, SCREEN_NOTIFY_PREVIEW, eSetBits);
    } else {
        __atomic_fetch_add(&s_preview_frames_dropped, 1U, __ATOMIC_RELAXED);
    }
    return error;
}

esp_err_t jetkvm_screen_preview_submit_ouyy_evyy(const uint8_t *pixels,
                                                 size_t size,
                                                 uint16_t width,
                                                 uint16_t height,
                                                 uint16_t fps)
{
    return screen_preview_submit_yuv(
        pixels, size, width, height, fps,
        PPA_SRM_COLOR_MODE_YUV420,
        (size_t)width * height * 3U / 2U);
}

esp_err_t jetkvm_screen_preview_submit_uyvy(const uint8_t *pixels,
                                            size_t size,
                                            uint16_t width,
                                            uint16_t height,
                                            uint16_t fps)
{
    return screen_preview_submit_yuv(
        pixels, size, width, height, fps,
        PPA_SRM_COLOR_MODE_YUV422_UYVY,
        (size_t)width * height * 2U);
}

esp_err_t jetkvm_screen_preview_stop(void)
{
    __atomic_fetch_add(&s_preview_epoch, 1U, __ATOMIC_ACQ_REL);
    __atomic_store_n(&s_preview_stop_requested, true, __ATOMIC_RELEASE);
    TaskHandle_t task = s_task;
    if (task != NULL) {
        (void)xTaskNotify(task, SCREEN_NOTIFY_PREVIEW, eSetBits);
    }
    return ESP_OK;
}

esp_err_t jetkvm_screen_get_stats(jetkvm_screen_stats_t *stats)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    local_display_state_t display = {0};
    (void)local_display_state(&display);
    EventBits_t bits = s_events != NULL ? xEventGroupGetBits(s_events) : 0;
    *stats = (jetkvm_screen_stats_t) {
        .display_rotation = display.rotation,
        .backlight_brightness = display.brightness,
        .backlight_duty = display.ready ? ledc_get_duty(SCREEN_BACKLIGHT_SPEED_MODE, SCREEN_BACKLIGHT_CHANNEL) : 0,
        .display_settings_error = display.error,
        .initialized = (bits & SCREEN_EVENT_READY) != 0U,
        .active = (bits & SCREEN_EVENT_ACTIVE) != 0U,
        .usb_connected = __atomic_load_n(&s_rendered_usb_connected,
                                         __ATOMIC_ACQUIRE),
        .last_error = (esp_err_t)__atomic_load_n(&s_last_error, __ATOMIC_RELAXED),
        .updates_submitted = __atomic_load_n(&s_updates_submitted, __ATOMIC_RELAXED),
        .updates_applied = __atomic_load_n(&s_updates_applied, __ATOMIC_RELAXED),
        .pending_replacements = __atomic_load_n(&s_pending_replacements, __ATOMIC_RELAXED),
        .fit_policy = JETKVM_SCREEN_FIT_NATIVE_240_SQUARE,
        .source_width = JETKVM_SCREEN_SOURCE_WIDTH,
        .source_height = JETKVM_SCREEN_SOURCE_HEIGHT,
        .panel_width = JETKVM_SCREEN_PANEL_NATIVE_WIDTH,
        .panel_height = JETKVM_SCREEN_PANEL_NATIVE_HEIGHT,
        .preview_active = __atomic_load_n(&s_preview_active, __ATOMIC_ACQUIRE),
        .preview_width = __atomic_load_n(&s_preview_width, __ATOMIC_RELAXED),
        .preview_height = __atomic_load_n(&s_preview_height, __ATOMIC_RELAXED),
        .preview_fps = __atomic_load_n(&s_preview_fps, __ATOMIC_RELAXED),
        .preview_frames_submitted =
            __atomic_load_n(&s_preview_frames_submitted, __ATOMIC_RELAXED),
        .preview_frames_applied =
            __atomic_load_n(&s_preview_frames_applied, __ATOMIC_RELAXED),
        .preview_frames_dropped =
            __atomic_load_n(&s_preview_frames_dropped, __ATOMIC_RELAXED),
        .preview_ppa_frames =
            __atomic_load_n(&s_preview_ppa_frames, __ATOMIC_RELAXED),
        .preview_ppa_total_us =
            __atomic_load_n(&s_preview_ppa_total_us, __ATOMIC_RELAXED),
        .preview_ppa_max_us =
            __atomic_load_n(&s_preview_ppa_max_us, __ATOMIC_RELAXED),
    };
    portENTER_CRITICAL(&s_wifi_failure_shown_lock);
    memcpy(stats->wifi_failure_shown, s_wifi_failure_shown, sizeof(stats->wifi_failure_shown));
    stats->wifi_failure_shows = s_wifi_failure_shows;
    portEXIT_CRITICAL(&s_wifi_failure_shown_lock);
    return ESP_OK;
}

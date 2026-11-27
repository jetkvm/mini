#include "host_ui.h"

#include <stdio.h>
#include <string.h>

#include <lvgl.h>

#include "driver/gpio.h"
#include "jetkvm_board_config.h"
#include "jetkvm_screen_buttons.h"
#include "jetkvm_screen_regulatory.h"
#include "jetkvm_screen_tailscale_only.h"
#include "jetkvm_screen_usb_debugging.h"
#include "local_display_settings.h"
#include "ui.h"
#include "vars.h"

#define RENDER_LINES 20U

uint16_t host_ui_framebuffer[JETKVM_SCREEN_SOURCE_WIDTH * JETKVM_SCREEN_SOURCE_HEIGHT];
static uint16_t s_render_buffer[JETKVM_SCREEN_SOURCE_WIDTH * RENDER_LINES];

/* The buttons pull their pins low. */
static const gpio_num_t s_pins[2] = {JETKVM_BUTTON_SW3_PIN, JETKVM_BUTTON_SW2_PIN};
static bool s_pressed[2];
static void (*s_handler[2])(void *);
static void *s_handler_arg[2];

esp_err_t gpio_config(const gpio_config_t *config)
{
    (void)config;
    return ESP_OK;
}

esp_err_t gpio_install_isr_service(int flags)
{
    (void)flags;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_add(gpio_num_t pin, void (*handler)(void *), void *arg)
{
    for (unsigned i = 0; i < 2; ++i) {
        if (s_pins[i] == pin) {
            s_handler[i] = handler;
            s_handler_arg[i] = arg;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_STATE;
}

int gpio_get_level(gpio_num_t pin)
{
    for (unsigned i = 0; i < 2; ++i) {
        if (s_pins[i] == pin) {
            return s_pressed[i] ? 0 : 1;
        }
    }
    return 1;
}

/* The display is lit, so a press acts. */
bool local_display_state(local_display_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->ready = state->active = true;
    return true;
}

void local_display_wake(void)
{
}

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixel_map)
{
    const size_t width = (size_t)(area->x2 - area->x1 + 1);
    const size_t rows = (size_t)(area->y2 - area->y1 + 1);
    const uint16_t *source = (const uint16_t *)pixel_map;

    for (size_t row = 0; row < rows; ++row) {
        uint16_t *destination = host_ui_framebuffer +
                                ((size_t)area->y1 + row) * JETKVM_SCREEN_SOURCE_WIDTH +
                                (size_t)area->x1;
        memcpy(destination, source + row * width, width * sizeof(uint16_t));
    }
    lv_display_flush_ready(display);
}

bool host_ui_start(void)
{
    lv_init();
    lv_display_t *display =
        lv_display_create(JETKVM_SCREEN_SOURCE_WIDTH, JETKVM_SCREEN_SOURCE_HEIGHT);
    if (display == NULL) {
        return false;
    }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, flush_cb);
    lv_display_set_buffers(display, s_render_buffer, NULL, sizeof(s_render_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    set_var_app_version("v0.1.0-local");
    set_var_system_version("ESP-IDF 6.0.2");
    ui_init();
    jetkvm_screen_usb_debugging_init();
    jetkvm_screen_tailscale_only_init();
    jetkvm_screen_regulatory_init();
    return true;
}

void host_ui_start_buttons(void)
{
    jetkvm_screen_buttons_init(NULL, 0);
}

/* A level change and the two samples that the debounce needs. */
static void set_button(unsigned button, bool pressed)
{
    s_pressed[button] = pressed;
    s_handler[button](s_handler_arg[button]);
    for (unsigned sample = 0; sample < 2; ++sample) {
        lv_tick_inc(10);
        (void)jetkvm_screen_buttons_poll(false);
    }
}

void host_ui_tap(unsigned button)
{
    set_button(button, true);
    set_button(button, false);
}

void host_ui_press(unsigned button)
{
    set_button(button, true);
}

void host_ui_keep(uint32_t milliseconds)
{
    for (uint32_t held = 0; held < milliseconds; held += 10) {
        lv_tick_inc(10);
        (void)jetkvm_screen_buttons_poll(false);
    }
}

void host_ui_release(unsigned button)
{
    set_button(button, false);
}

void host_ui_hold(unsigned button, uint32_t milliseconds)
{
    host_ui_press(button);
    host_ui_keep(milliseconds);
    host_ui_release(button);
}

/* The width that LVGL gives `letter` before `next` (0 at the end of the text). */
static int32_t glyph(const lv_font_t *font, char letter, char next)
{
    return (int32_t)lv_font_get_glyph_width(font, (uint32_t)letter, (uint32_t)next);
}

void host_ui_widest_address(const lv_font_t *font, char address[16])
{
    /* best[k][o]: the widest text of k + 1 octets that ends with octet o,
     * without the width of its last digit; from[k][o]: the octet before o. */
    static int32_t best[4][256];
    static uint8_t from[4][256];
    char text[256][4];
    int32_t inner[256];
    for (unsigned o = 0; o < 256; ++o) {
        snprintf(text[o], sizeof(text[o]), "%u", o);
        inner[o] = 0;
        for (size_t i = 0; text[o][i + 1] != '\0'; ++i) {
            inner[o] += glyph(font, text[o][i], text[o][i + 1]);
        }
        best[0][o] = inner[o];
    }
    for (unsigned k = 1; k < 4; ++k) {
        for (unsigned o = 0; o < 256; ++o) {
            best[k][o] = -1;
            for (unsigned p = 0; p < 256; ++p) {
                char last = text[p][strlen(text[p]) - 1];
                int32_t width = best[k - 1][p] + glyph(font, last, '.') +
                                glyph(font, '.', text[o][0]) + inner[o];
                if (width > best[k][o]) {
                    best[k][o] = width;
                    from[k][o] = (uint8_t)p;
                }
            }
        }
    }
    unsigned octets[4] = {0};
    int32_t widest = -1;
    for (unsigned o = 0; o < 256; ++o) {
        int32_t width = best[3][o] + glyph(font, text[o][strlen(text[o]) - 1], '\0');
        if (width > widest) {
            widest = width;
            octets[3] = o;
        }
    }
    for (unsigned k = 3; k > 0; --k) {
        octets[k - 1] = from[k][octets[k]];
    }
    snprintf(address, 16, "%u.%u.%u.%u", octets[0], octets[1], octets[2], octets[3]);
}

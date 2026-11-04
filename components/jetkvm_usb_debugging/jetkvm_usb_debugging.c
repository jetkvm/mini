/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_usb_debugging.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_private/periph_ctrl.h"
#include "hal/usb_serial_jtag_ll.h"
#include "soc/io_mux_reg.h"
#include "soc/usb_serial_jtag_struct.h"
#include "jetkvm_control.h"

static const char *TAG = "usb_debugging";

/* The USB Serial/JTAG PHY is FSLS PHY 0 on GPIO 24 and 25. The TinyUSB port
 * to the target computer is the HS controller on the UTMI PHY, which does
 * not share it. A restart of the firmware (a CPU reset) keeps the USB
 * Serial/JTAG registers, so a port that a start disconnected stays
 * disconnected, with its lines pulled down, until a start connects it again
 * or the Mini is unplugged and plugged in again. */
static bool s_active;
/* The stored setting, kept by start and set so that callers on a PSRAM
 * stack can read it without flash access. */
static bool s_enabled;

/* Turning the pads off alone stops the data but leaves the D+ pull-up, so
 * the computer keeps the device listed and, after a restart, fails to
 * enumerate it. The PHY's pull override takes the pull-ups off and pulls D+
 * and D- down, gpio_pullup_dis() takes off the weak pull-up of the D+ pin,
 * and the computer sees the device leave. */
static void disconnect(void)
{
    const usb_serial_jtag_pull_override_vals_t pulled_down = {
        .dp_pd = true,
        .dm_pd = true,
    };
    usb_serial_jtag_ll_phy_enable_pull_override(&pulled_down);
    (void)gpio_pullup_dis(USB_USJ_INT_PHY_DP_GPIO_NUM);
    usb_serial_jtag_ll_phy_enable_pad(false);
}

/* What disconnect() leaves: the pads off, or the pull override without the
 * D+ pull-up (gpio_pullup_dis() alone does that). */
static bool disconnected(void)
{
    return !usb_serial_jtag_ll_phy_is_pad_enabled() ||
           (USB_SERIAL_JTAG.conf0.pad_pull_override && !USB_SERIAL_JTAG.conf0.dp_pullup);
}

/* Undoes disconnect() of an earlier start. The peripheral reset puts the USB
 * Serial/JTAG controller and its PHY register back to their power-on state:
 * pads on, no pull override, D+ pull-up from the controller. The computer
 * sees a new device and enumerates it. The secondary console's connection
 * monitor reads the raw SOF bit, which the reset does not need. The IO MUX
 * pull-up of GPIO 24 that gpio_pullup_dis() cleared is off at power-on too. */
static void reconnect(void)
{
    PERIPH_RCC_ATOMIC() {
        usb_serial_jtag_ll_enable_bus_clock(true);
        usb_serial_jtag_ll_reset_register();
    }
}

bool jetkvm_usb_debugging_active(void)
{
    return s_active;
}

bool jetkvm_usb_debugging_enabled(void)
{
    return __atomic_load_n(&s_enabled, __ATOMIC_RELAXED);
}

/* Also builds the reply to a set, so it must not fail after the save. */
static bool rpc_get(bool *enabled, bool *active, void *ctx)
{
    (void)ctx;
    *enabled = jetkvm_usb_debugging_enabled();
    *active = s_active;
    return true;
}

static const char *rpc_set(bool enabled, void *ctx)
{
    (void)ctx;
    esp_err_t error = jetkvm_usb_debugging_save(enabled);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "setting not saved: %s", esp_err_to_name(error));
        return "USB debugging setting not saved";
    }
    __atomic_store_n(&s_enabled, enabled, __ATOMIC_RELAXED);
    ESP_LOGI(TAG, "USB debugging %s from the next restart", enabled ? "on" : "off");
    return NULL;
}

void jetkvm_usb_debugging_start(void)
{
    bool enabled = jetkvm_usb_debugging_build_default();
    esp_err_t error = jetkvm_usb_debugging_load(&enabled);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "setting unreadable, using the build default (%s): %s",
                 enabled ? "on" : "off", esp_err_to_name(error));
    }
    if (!enabled) {
        /* Logged before the cut, so a USB Serial/JTAG reader sees it too. */
        ESP_LOGI(TAG, "USB debugging off: USB Serial/JTAG disconnected; console on UART");
        disconnect();
    } else if (disconnected()) {
        reconnect();
        ESP_LOGI(TAG, "USB debugging on: USB Serial/JTAG connected again");
    } else {
        ESP_LOGI(TAG, "USB debugging on");
    }
    s_active = !disconnected();
    __atomic_store_n(&s_enabled, enabled, __ATOMIC_RELAXED);
    jetkvm_control_set_default_usb_debugging_context(&(jetkvm_usb_debugging_context_t){
        .get = rpc_get,
        .set = rpc_set,
    });
}

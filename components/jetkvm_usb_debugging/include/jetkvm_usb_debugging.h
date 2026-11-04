/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * USB debugging: the ESP32-P4 USB Serial/JTAG port on the Power/EXT
 * connector, which a computer connected there can use to read the console,
 * write flash and debug the chip. The setting is in NVS with the other
 * device settings (namespace jkvm_settings); a factory reset erases it.
 * Without a stored value it is off, and on in a build with
 * CONFIG_JETKVM_USB_DEBUGGING_DEFAULT_ON (bench builds only).
 *
 * A power-on reset connects the port, and it stays connected until
 * jetkvm_usb_debugging_start() reads the setting, about 1.8 s with the ROM,
 * the bootloader and the PSRAM test. A restart keeps the port as the last
 * start left it, and the next start applies the setting again. So turning
 * the setting off or on takes effect at the next restart.
 */

/* The value without a stored setting. */
bool jetkvm_usb_debugging_build_default(void);

/*
 * The stored setting. A missing setting reads as the build default. A
 * setting that cannot be read also reads as the build default, and the
 * function returns the error.
 */
esp_err_t jetkvm_usb_debugging_load(bool *enabled);

/* Stores the setting; see above for when it takes effect. */
esp_err_t jetkvm_usb_debugging_save(bool enabled);

/*
 * Reads the setting and, when it is off, disconnects the USB Serial/JTAG
 * port: the lines are pulled down and the PHY pads are turned off, so the
 * computer sees the device leave. When it is on and an earlier start
 * disconnected the port, resets the USB Serial/JTAG peripheral, which
 * connects the port again. The UART console stays. The OTG port to
 * the target computer (HID and mass storage) is a separate controller and
 * PHY. Also registers the getUsbDebuggingState and setUsbDebuggingState
 * RPCs. Call once, after NVS is ready.
 */
void jetkvm_usb_debugging_start(void);

/* True while the USB Serial/JTAG port is connected (since this start). */
bool jetkvm_usb_debugging_active(void);

/* The stored setting as start read it or the setting RPC stored it, without
 * flash access; false before start. */
bool jetkvm_usb_debugging_enabled(void);

#ifdef __cplusplus
}
#endif

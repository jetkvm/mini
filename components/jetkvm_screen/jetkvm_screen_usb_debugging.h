#pragma once

#include <stdbool.h>

/*
 * Advanced > USB Debugging. The entry reads "USB Debugging: On" or "Off"
 * from the stored setting; accepting it asks the screen's RPC handler for
 * setUsbDebuggingState with the other value, which stores it and restarts
 * the device as Reboot Device does. While the stored setting and the port
 * differ (it was changed from the web UI), a note below the entry says that
 * the setting takes effect at the next restart. All calls run with LVGL
 * owned by the caller.
 */

/* Call once after ui_init(). */
void jetkvm_screen_usb_debugging_init(void);

/* `enabled`: the stored setting; `active`: the port is connected now. */
void jetkvm_screen_usb_debugging_apply(bool enabled, bool active);

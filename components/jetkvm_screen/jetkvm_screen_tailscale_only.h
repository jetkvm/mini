#pragma once

#include <stdbool.h>

#include "lvgl.h"

/*
 * Advanced > Tailscale Only. While "Web interface through Tailscale only" is
 * on, the web interface does not answer on the local network. If Tailscale
 * then stops working, this entry is the way back: it is shown only while the
 * setting is on, reads "Tailscale Only: On", and accepting it asks the
 * screen's RPC handler for setTailscaleOnlyAccess with false. The setting
 * takes effect at once, so the device does not restart, and the entry goes
 * with the next state. All calls run with LVGL owned by the caller.
 */

/* Call once after ui_init(). */
void jetkvm_screen_tailscale_only_init(void);

/* `enabled`: the stored setting. Returns true when the entry appeared or
 * went, so that the caller can tell the button code. */
bool jetkvm_screen_tailscale_only_apply(bool enabled);

/* The entry, also while it is hidden. */
lv_obj_t *jetkvm_screen_tailscale_only_entry(void);

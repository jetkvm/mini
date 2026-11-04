#include "jetkvm_screen.h"

/*
 * Link-only contract. This test app does not start the hardware; the
 * production firmware decides when jetkvm_screen takes SPI2.
 */
void app_main(void)
{
    jetkvm_screen_state_t state = {
        .network_connected = true,
        .cloud_state = JETKVM_SCREEN_CLOUD_CONNECTED,
        .hdmi_connected = true,
        .usb_connected = false,
        .page = JETKVM_SCREEN_PAGE_HOME,
    };
    jetkvm_screen_stats_t stats;
    (void)jetkvm_screen_start();
    (void)jetkvm_screen_update(&state);
    (void)jetkvm_screen_get_stats(&stats);
    (void)jetkvm_screen_stop();
}

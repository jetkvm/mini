#!/usr/bin/env python3
"""Execute the production boot gate for both board variants, without hardware."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
source = (root / 'main/main.c').read_text()
start = source.index('    esp_err_t ret = nvs_flash_init();', source.index('void app_main(void)'))
end = source.index('    p4_device_set_wake_on_lan_available(false);', start)
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static int nvs_error, reset_error, commit_error, continued, radio_calls, progress_calls;
static int screen_error, screen_calls, usb_debugging_starts, started_this_boot;
static bool pending;
static int nvs_flash_init(void) { return nvs_error; }
static void show_reset_progress(void) { ++progress_calls; }
static int jetkvm_factory_reset_boot_begin(void (*show)(void)) {
    if (pending) show();
    return reset_error;
}
static bool jetkvm_factory_reset_pending(void) { return pending; }
static int jetkvm_factory_reset_finish(int error) {
    if (!error) error=commit_error;
    if (!error) pending=false;
    return error;
}
/* USB debugging may turn the debug port off only after NVS and the factory
 * reset succeeded; the early returns keep it for serial recovery. */
static void jetkvm_usb_debugging_start(void) { ++usb_debugging_starts; started_this_boot = 1; }
static int jetkvm_board_network_screen_start(void) {
    ++screen_calls;
    return screen_error;
}
#if JETKVM_BOARD_WIFI
static int radio_error, wifi_setup_handlers;
static int jetkvm_c5_wifi_start(void) {
    assert(screen_calls > 0);
    /* A normal boot turns the port off before the radio start can wait; a
     * boot with a pending reset waits until the reset has finished. */
    assert(started_this_boot == !pending);
    ++radio_calls;
    return radio_error ? radio_error : jetkvm_factory_reset_finish(0);
}
static const char *wifi_setup_from_display(bool start) { (void)start; return NULL; }
static void jetkvm_screen_set_wifi_setup_handler(const char *(*handler)(bool)) {
    assert(handler == wifi_setup_from_display);
    ++wifi_setup_handlers;
}
#endif
static void boot(void) {
    started_this_boot = 0;
'''
suffix = r'''
    ++continued;
}
int main(void) {
    pending=true;nvs_error=1;boot();assert(!progress_calls&&!continued&&!radio_calls&&!screen_calls);
    nvs_error=0;reset_error=1;boot();assert(progress_calls==1&&!continued&&!radio_calls&&!screen_calls);
    assert(!usb_debugging_starts);
    reset_error=0;
#if JETKVM_BOARD_WIFI
    radio_error=1;boot();assert(radio_calls==1&&!continued&&pending&&!wifi_setup_handlers);
    radio_error=0;
#endif
    commit_error=1;boot();assert(!continued&&pending);
    commit_error=0;boot();assert(continued==1&&!pending);
#if JETKVM_BOARD_WIFI
    /* The local "Set up Wi-Fi" entry exists only once the radio has started. */
    assert(wifi_setup_handlers==1);
#endif
    /* Normal boots show the logo too; display failure must not stop boot. */
    int previous_screen_calls=screen_calls;
    boot();assert(continued==2&&screen_calls==previous_screen_calls+1);
    screen_error=1;boot();assert(continued==3&&screen_calls==previous_screen_calls+2);
    pending=true;boot();assert(continued==4&&!pending);
    /* Only the boots that continue apply USB debugging, once each. */
    assert(usb_debugging_starts==continued);
    puts("Boot gate: reset/radio/commit failure blocks normal services; success continues without reboot: PASS");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prefix + source[start:end] + suffix)
    for wifi in (0, 1):
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-DJETKVM_BOARD_WIFI=' + str(wifi),
                        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        subprocess.run([str(path / 'test')], check=True)

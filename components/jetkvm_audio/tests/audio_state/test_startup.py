#!/usr/bin/env python3
"""Exercise the actual main startup block with audio/USB allocation failures."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[4]
main = (root/'main/main.c').read_text()
start = main.index('    ret = jetkvm_audio_init();')
block = main[start:main.index('#else', start)]
prefix = r'''
#include <assert.h>
#include <stddef.h>
#define ESP_OK 0
#define ESP_ERROR_CHECK(expr) assert((expr)==ESP_OK)
#define ESP_LOGW(...) (++warnings)
#define ESP_LOGI(...) ((void)0)
typedef struct { int driver_enabled, hid_input_ready, desired_enabled; } jetkvm_usb_service_snapshot_t;
static int audio_error, usb_error, starts, snapshots, warnings, continued;
static int jetkvm_audio_init(void) { return audio_error; }
static void usb_service_state_changed(void) {}
static int jetkvm_usb_service_start(void (*callback)(void), void *ctx) {
    (void)callback;(void)ctx;++starts;return usb_error;
}
static int jetkvm_usb_service_get_snapshot(jetkvm_usb_service_snapshot_t *s) {
    (void)s;++snapshots;return 0;
}
static void hid_write_recovery_requested(unsigned consecutive_timeouts, void *ctx) {
    (void)consecutive_timeouts;(void)ctx;
}
static int jetkvm_hid_input_set_write_recovery_handler(void (*handler)(unsigned, void *), void *ctx) {
    (void)handler;(void)ctx;return 0;
}
static void webrtc_transport_keyboard_changed(void *ctx) { (void)ctx; }
static int jetkvm_hid_input_set_keyboard_changed_handler(void (*handler)(void *), void *ctx) {
    (void)handler;(void)ctx;return 0;
}
static void startup(void) { int ret;
'''
suffix = r'''
    ++continued;
}
int main(void) {
    audio_error=1;startup();assert(continued==1 && starts==1 && snapshots==1 && warnings==1);
    audio_error=0;usb_error=1;startup();assert(continued==2 && starts==2 && snapshots==1 && warnings==2);
    usb_error=0;startup();assert(continued==3 && starts==3 && snapshots==2 && warnings==2);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)
    (path/'test.c').write_text(prefix+block+suffix)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Audio/USB startup failures preserve remaining startup: PASS')

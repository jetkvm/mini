#!/usr/bin/env python3
"""Exercise the production reset-page gate and completion versus queued AUTO."""
from pathlib import Path
import subprocess
import tempfile
source=(Path(__file__).resolve().parents[2]/'jetkvm_screen.c').read_text()
start=source.index('static bool apply_reset_status(')
end=source.index('static void apply_state(',start)
prefix=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef enum { JETKVM_SCREEN_PAGE_AUTO, JETKVM_SCREEN_PAGE_BOOT,
 JETKVM_SCREEN_PAGE_NO_NETWORK, JETKVM_SCREEN_PAGE_HOME,
 JETKVM_SCREEN_PAGE_MENU, JETKVM_SCREEN_PAGE_REBOOTING } jetkvm_screen_page_t;
enum ScreensEnum { SCREEN_ID_BOOT_SCREEN=JETKVM_SCREEN_PAGE_BOOT,
 SCREEN_ID_REBOOTING_SCREEN=JETKVM_SCREEN_PAGE_REBOOTING };
typedef struct { jetkvm_screen_page_t page;bool network_connected;bool wifi_setup_menu; } jetkvm_screen_state_t;
static struct { int reboot_in_progress_label; } objects;
static bool s_wifi_setup_requested;
static bool pending, failed;
static jetkvm_screen_page_t current;
static char label[128]="Original reboot label";
static bool jetkvm_factory_reset_pending(void) { return pending; }
static const char *jetkvm_factory_reset_error(void) { return failed?"error":NULL; }
static const char *lv_label_get_text(int obj) { (void)obj;return label; }
static void set_label_text(int obj,const char *text) { (void)obj;strcpy(label,text); }
static jetkvm_screen_page_t rendered_page(void) { return current; }
static void loadScreen(enum ScreensEnum page) { current=(jetkvm_screen_page_t)page; }
static void set_var_main_screen(const char *text) { (void)text; }
'''
suffix=r'''
int main(void) {
 const jetkvm_screen_state_t boot={.page=JETKVM_SCREEN_PAGE_BOOT};
 const jetkvm_screen_state_t setup={.page=JETKVM_SCREEN_PAGE_AUTO};
 pending=true;apply_page(&boot);
 assert(current==JETKVM_SCREEN_PAGE_REBOOTING&&strstr(label,"Keep power connected"));
 apply_page(&setup);assert(current==JETKVM_SCREEN_PAGE_REBOOTING);
 failed=true;assert(apply_reset_status()&&strstr(label,"Reset failed"));
 /* Completion before AUTO is drained: the edge must not overwrite AUTO. */
 pending=failed=false;apply_page(&setup);
 assert(current==JETKVM_SCREEN_PAGE_NO_NETWORK&&!strcmp(label,"Original reboot label"));
 assert(!apply_reset_status()&&current==JETKVM_SCREEN_PAGE_NO_NETWORK);
 /* Completion observed without a queued update waits on BOOT. */
 pending=true;apply_page(&boot);pending=false;assert(!apply_reset_status());
 assert(current==JETKVM_SCREEN_PAGE_BOOT);
 apply_page(&setup);assert(current==JETKVM_SCREEN_PAGE_NO_NETWORK);
 current=JETKVM_SCREEN_PAGE_MENU;apply_page(&setup);assert(current==JETKVM_SCREEN_PAGE_MENU);
 current=JETKVM_SCREEN_PAGE_REBOOTING;assert(!apply_reset_status());
 assert(current==JETKVM_SCREEN_PAGE_REBOOTING); /* ordinary reboot unchanged */
 puts("Reset display: first-frame priority, error without network updates, completion/AUTO ordering: PASS");
}
'''
# The initial reset gate runs before the first refresh and READY acknowledgement.
init=source[source.index('static esp_err_t screen_hardware_init('):source.index('static bool screen_set_active(')]
assert init.index('apply_state(&initial_state)') < init.index('lv_refr_now(screen->display)')
assert '!resetting && jetkvm_screen_buttons_poll(false)' in source
assert 'if (!resetting) ui_tick();' in source
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.c').write_text(prefix+source[start:end]+suffix)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

#!/usr/bin/env python3
"""BLE Wi-Fi setup screen: the code shows only while a phone must enter it.

Compiles the production setters of jetkvm_board_network.c, its projection
into the screen state, and apply_wifi_setup of jetkvm_screen.c, then drives
them with the calls jetkvm_c5_wifi.c makes for each setup event.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[4]
network = (root / 'components/jetkvm_board_network/jetkvm_board_network.c').read_text()
screen = (root / 'components/jetkvm_screen/jetkvm_screen.c').read_text()


def cut(source: str, start: str, end: str) -> str:
    first = source.index(start)
    return source[first:source.index(end, first)]


struct = cut(network, 'typedef struct {\n    bool link_up;', 'static const char *ETH_TAG')
copy_text = cut(network, 'static void screen_copy_text(', 'static void screen_network_set_mac(')
setters = cut(network, 'static const char *wifi_setup_name(', 'static void screen_network_apply(')
failure_setter = cut(network, 'void jetkvm_board_network_set_wifi_failure(',
                     'void jetkvm_board_network_set_wifi_setup_menu(')
apply_state = cut(network, 'static void screen_network_apply(', 'static void screen_cloud_domain_from_url(')
apply_screen = cut(screen, 'static void apply_wifi_setup(', 'static bool apply_reset_status(')

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "jetkvm_screen.h"
#define JETKVM_BOARD_WIFI 1
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define ESP_LOGI(tag, ...) ((void)(tag))
static const char *TAG = "board_network";
static size_t strlcpy(char *d, const char *s, size_t n) {
 size_t l = strlen(s); if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; } return l;
}
static void wifi_update_display_mode(void) {}
static char title[64], line[64];
static struct { int no_network_title, home_info_ipv6_addr_1; } objects = {1, 2};
static void set_label_text(int obj, const char *text) { strcpy(obj == 1 ? title : line, text); }
static bool s_wifi_setup_requested;
/* The red reason lines below the text: NULL while hidden. */
static char failure_text[64];
static const char *failure_line;
static void apply_wifi_failure(const char *text) {
 failure_line = text != NULL && text[0] != 0 ? strcpy(failure_text, text) : NULL;
}
'''

suffix = r'''
static jetkvm_screen_state_t shown;
static void show(void) {
 memset(&shown, 0, sizeof(shown));
 screen_network_apply(&shown);
 apply_wifi_setup(&shown);
}
static void expect(const char *t, const char *l) {
 show();
 if (strcmp(title, t) || strcmp(line, l)) {
  fprintf(stderr, "screen \"%s\" / \"%s\", expected \"%s\" / \"%s\"\n", title, line, t, l);
  assert(0);
 }
}
int main(void) {
 /* The service starts; a phone connects and gets the code. */
 jetkvm_board_network_set_wifi_setup(true, NULL);
 expect("Set up Wi-Fi", "Visit jetkvm.com/wireless");
 jetkvm_board_network_set_wifi_setup(true, "246810");
 expect("246810", "Enter this setup code");

 /* A wrong code keeps the code up for the next try. */
 jetkvm_board_network_set_wifi_setup_phone(JETKVM_SCREEN_WIFI_SETUP_CODE, NULL);
 expect("246810", "Enter this setup code");

 /* The right code: the code leaves the screen and the screen state. */
 jetkvm_board_network_set_wifi_setup_phone(JETKVM_SCREEN_WIFI_SETUP_ACCEPTED, NULL);
 expect("Paired", "Continue in your browser");
 assert(shown.wifi_setup_code[0] == '\0');

 /* The phone sends a network, the join fails (back to ACCEPTED, not the
  * code), the phone sends it again. */
 jetkvm_board_network_set_wifi_setup_phone(JETKVM_SCREEN_WIFI_SETUP_JOINING, "Office");
 expect("Connecting", "Joining Office");
 jetkvm_board_network_set_wifi_setup_phone(JETKVM_SCREEN_WIFI_SETUP_ACCEPTED, NULL);
 expect("Paired", "Continue in your browser");
 assert(shown.wifi_setup_code[0] == '\0');
 jetkvm_board_network_set_wifi_setup_phone(JETKVM_SCREEN_WIFI_SETUP_JOINING, "Office");
 expect("Connecting", "Joining Office");

 /* A failed join with a reason: the Paired page says "Try again" with
  * the reason; the next join and the code page hide it. */
 jetkvm_board_network_set_wifi_failure("Wrong password\nOffice");
 jetkvm_board_network_set_wifi_setup_phone(JETKVM_SCREEN_WIFI_SETUP_ACCEPTED, NULL);
 expect("Try again", "Continue in your browser");
 assert(failure_line != NULL && strcmp(failure_line, "Wrong password\nOffice") == 0);
 jetkvm_board_network_set_wifi_setup_phone(JETKVM_SCREEN_WIFI_SETUP_JOINING, "Office");
 expect("Connecting", "Joining Office");
 assert(failure_line == NULL);
 jetkvm_board_network_set_wifi_setup_phone(JETKVM_SCREEN_WIFI_SETUP_CODE, NULL);
 expect("246810", "Enter this setup code");
 assert(failure_line == NULL);
 jetkvm_board_network_set_wifi_failure(NULL);
 jetkvm_board_network_set_wifi_setup_phone(JETKVM_SCREEN_WIFI_SETUP_ACCEPTED, NULL);
 expect("Paired", "Continue in your browser");
 jetkvm_board_network_set_wifi_setup_phone(JETKVM_SCREEN_WIFI_SETUP_JOINING, "Office");

 /* The phone leaves mid-setup: back to the instructions, no code. A phone
  * step that arrives after that (a queued event) changes nothing. */
 jetkvm_board_network_set_wifi_setup(true, NULL);
 expect("Set up Wi-Fi", "Visit jetkvm.com/wireless");
 jetkvm_board_network_set_wifi_setup_phone(JETKVM_SCREEN_WIFI_SETUP_ACCEPTED, NULL);
 expect("Set up Wi-Fi", "Visit jetkvm.com/wireless");
 jetkvm_board_network_set_wifi_setup_phone(JETKVM_SCREEN_WIFI_SETUP_CODE, NULL);
 expect("Set up Wi-Fi", "Visit jetkvm.com/wireless");

 /* A failed session after an accepted one on the same link shows the code
  * again; the service stopping clears everything. */
 jetkvm_board_network_set_wifi_setup(true, "246810");
 jetkvm_board_network_set_wifi_setup_phone(JETKVM_SCREEN_WIFI_SETUP_ACCEPTED, NULL);
 jetkvm_board_network_set_wifi_setup_phone(JETKVM_SCREEN_WIFI_SETUP_CODE, NULL);
 expect("246810", "Enter this setup code");
 jetkvm_board_network_set_wifi_setup_phone(JETKVM_SCREEN_WIFI_SETUP_JOINING, "Office");
 jetkvm_board_network_set_wifi_setup(false, NULL);
 expect("Connecting", "Looking for saved Wi-Fi");
 jetkvm_board_network_set_wifi_setup_phone(JETKVM_SCREEN_WIFI_SETUP_JOINING, "Office");
 expect("Connecting", "Looking for saved Wi-Fi");
 puts("Wi-Fi setup screen: code until accepted, join and failed join with its reason, phone gone: PASS");
}
'''

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(prefix + struct + copy_text + failure_setter + setters + apply_state +
                                 apply_screen + suffix)
    (path / 'esp_err.h').write_text('typedef int esp_err_t;\n')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-function', '-Wno-unused-variable',
                    '-fsanitize=address,undefined', '-I', str(path),
                    '-I', str(root / 'components/jetkvm_screen/include'),
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)

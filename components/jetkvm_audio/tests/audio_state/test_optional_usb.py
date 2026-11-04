#!/usr/bin/env python3
"""Exercise the actual optional-audio descriptor fallback before USB install."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[4]
source = (root / "components/jetkvm_usb_hid/jetkvm_usb_hid.c").read_text()
start = source.index("    esp_err_t audio_error = ESP_OK;", source.index("esp_err_t jetkvm_usb_hid_init(void)"))
block = source[start:source.index("    error = msc_reader_install(&config);", start)]
prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN 118
#define HID_FS_POLL_INTERVAL 1
#define HID_HS_POLL_INTERVAL 4
#define portENTER_CRITICAL(p) ((void)0)
#define portEXIT_CRITICAL(p) ((void)0)
#define ESP_LOGW(...) ((void)0)
static struct { bool initializing; struct { int last_error; } state; } s_ctx;
typedef struct { bool audio, os_services; unsigned interface_count, configuration_length;
    uint8_t os_services_interface;
    uint8_t full_speed_configuration[256], high_speed_configuration[256]; } hid_descriptor_bank_t;
static unsigned prepares, builds;
static int prepare_error, build_error;
static unsigned expected_interfaces = 5, expected_length = 120;
static int jetkvm_audio_prepare_usb(void) { ++prepares;return prepare_error; }
static int build_configuration_descriptor(const hid_descriptor_bank_t *bank, unsigned interval, uint8_t *destination) {
    assert(!bank->audio && bank->interface_count == expected_interfaces && bank->configuration_length == expected_length);
    assert(interval==1 || interval==4);destination[0]=bank->interface_count;++builds;return build_error;
}
static int prepare(hid_descriptor_bank_t *descriptor_bank) { int error=0;
"""
suffix = r"""
    return error;
}
int main(void) {
    hid_descriptor_bank_t bank={.audio=true,.interface_count=7,.configuration_length=238};
    assert(!prepare(&bank) && bank.audio && prepares==1 && !builds);
    prepare_error=12;
    assert(!prepare(&bank) && !bank.audio && prepares==2 && builds==2);
    assert(bank.full_speed_configuration[0]==5 && bank.high_speed_configuration[0]==5);
    assert(!prepare(&bank) && prepares==2); /* Audio already absent. */
    bank.audio=true;bank.interface_count=7;bank.configuration_length=238;
    build_error=1;s_ctx.initializing=true;
    assert(prepare(&bank) && !s_ctx.initializing && s_ctx.state.last_error==1);
    /* OS Services sits after audio (interfaces 7-8 of 9); dropping audio's
     * two interfaces must move it down to 5-6, or the descriptor would name
     * interfaces that no longer exist. */
    hid_descriptor_bank_t services={.audio=true,.os_services=true,.interface_count=9,
        .configuration_length=238+23,.os_services_interface=7};
    build_error=0;prepare_error=12;builds=0;expected_interfaces=7;expected_length=120+23;
    assert(!prepare(&services) && !services.audio && builds==2);
    assert(services.os_services_interface==5 && services.full_speed_configuration[0]==7);
    hid_descriptor_bank_t no_services={.audio=true,.interface_count=7,.configuration_length=238,
        .os_services_interface=0};
    expected_interfaces=5;expected_length=120;
    assert(!prepare(&no_services) && no_services.os_services_interface==0);
    puts("Optional audio allocation failure preserves HID/MSC/OS Services descriptors: PASS");
}
"""
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)
    (path/'test.c').write_text(prefix+block+suffix)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)

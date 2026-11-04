#!/usr/bin/env python3
"""Execute the radio-cleanup/marker-commit gate before provisioning."""
from pathlib import Path
import subprocess
import tempfile
source = (Path(__file__).resolve().parents[1] / 'jetkvm_c5_wifi.c').read_text()
start = source.index('    error = c5_clear_legacy_credentials_once();')
end = source.index('    /* KVM traffic', start)
prefix = r'''
#include <assert.h>
#include <stdio.h>
#define ESP_OK 0
#define ESP_LOGE(...) ((void)0)
static int radio_error, commit_error, radio_calls, finish_calls, provision_calls, cleanups;
static int jetkvm_factory_reset_finish(int error) {
    assert(!error && radio_calls && !radio_error);++finish_calls;return commit_error;
}
static int c5_clear_legacy_credentials_once(void) { ++radio_calls;return radio_error; }
static int start(void) {
    int error;
'''
suffix = r'''
    ++provision_calls;return 0;
cleanup:
    ++cleanups;return error;
}
int main(void) {
    radio_error=8;assert(start()==8&&!finish_calls&&!provision_calls);
    radio_error=0;commit_error=9;assert(start()==9&&finish_calls==1&&!provision_calls);
    commit_error=0;assert(start()==0&&finish_calls==2&&provision_calls==1&&cleanups==2);
    puts("C5 reset completion: radio cleanup and marker commit precede credential acceptance: PASS");
}
'''
# The executed gate must precede both provisioning and ordinary connections.
assert end < source.index('    error = c5_load_networks();', end)
assert end < source.index('    error = esp_wifi_start();', end)
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)
    (path/'test.c').write_text(prefix+source[start:end]+suffix)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)

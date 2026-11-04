#!/usr/bin/env python3
"""Fault-inject the production USB emulation completion/rollback function."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "jetkvm_usb_service.c").read_text()
start = source.index("static void apply_emulation_request(")
end = source.index("\nstatic void apply_pending_configuration", start)
function = source[start:end]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef int esp_err_t;
typedef int jetkvm_usb_config_t;
typedef int jetkvm_usb_config_store_state_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 2
#define JETKVM_USB_SERVICE_CONFIGURATION_FAILED 1
static bool driver, cancelled, failed, rollback_failed, fail_before_change;
static bool expected_rollback;
static int completed, rejected, rollback_calls;
static bool usb_driver_is_enabled(void) { return driver; }
static bool hid_input_usb_is_available(void) { return driver; }
static int enable_usb_stack(bool *ready) { if (fail_before_change) return 1; driver = true; *ready = !failed; return failed ? 1 : 0; }
static int disable_usb_stack(void) { if (fail_before_change) return 1; driver = false; return failed ? 1 : 0; }
static bool complete_configuration_success(const int *config, const int *store, uint32_t revision) {
    assert(*config == 3 && *store == 4 && revision == 5);
    if (cancelled) return false;
    ++completed; return true;
}
static int restore_configuration(const int *config, bool previous) {
    assert(*config == 3); ++rollback_calls;
    if (!rollback_failed) driver = previous;
    return rollback_failed ? 1 : 0;
}
static void complete_configuration_request(int result, int error, bool success,
    bool rolled_back, bool rollback_error, uint32_t revision) {
    assert(result == 1 && error != 0 && !success && rolled_back == expected_rollback);
    assert(rollback_error == rollback_failed && revision == 5); ++rejected;
}
'''
main = r'''
int main(void) {
    const int config = 3, store = 4;
    for (int enabled = 0; enabled < 2; ++enabled) {
        for (int failure = 0; failure < 5; ++failure) {
            driver = !enabled; cancelled = failure == 2;
            fail_before_change = failure == 4; expected_rollback = !fail_before_change;
            failed = failure == 1 || failure == 3; rollback_failed = failure == 3;
            completed = rejected = rollback_calls = 0;
            apply_emulation_request(&config, &store, 5, enabled);
            if (!failure) {
                assert(driver == (bool)enabled && completed == 1 && !rejected && !rollback_calls);
            } else {
                assert(!completed && rejected == 1 && rollback_calls == (int)expected_rollback);
                if (!rollback_failed) assert(driver == !enabled);
            }
        }
    }
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / "test.c").write_text(prefix + function + main)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("USB enable/disable completion, driver failure, cancellation and rollback failure: PASS")

#!/usr/bin/env python3
"""The fixed BLE setup code and its serial log exist only in bench builds.

Release images must not log the code, because the log is readable by anyone
at the USB console, and must not use the fixed code, which is public in this
repository. release_r2.sh refuses an image that contains the log format
string, so that string must stay identical to the one the firmware uses.
"""
from pathlib import Path
import re


component = Path(__file__).resolve().parents[1]
repo = component.parents[1]
source = (component / "jetkvm_c5_wifi.c").read_text()
cmake = (component / "CMakeLists.txt").read_text()
root_cmake = (repo / "CMakeLists.txt").read_text()
release = (repo / "tools/release_r2.sh").read_text()
reprovision = (repo / "tools/e2e/wifi_reprovision.py").read_text()

GUARD = "#if JETKVM_WIFI_SETUP_LOG_CODE"

# The bench-only part of each guard ends at its #else or #endif.
guarded_spans = []
for match in re.finditer(re.escape(GUARD) + r"\n(.*?)\n#(?:else|endif)", source, re.S):
    guarded_spans.append((match.start(1), match.end(1)))


def guarded(position: int) -> bool:
    return any(start <= position < end for start, end in guarded_spans)


# Every log call that formats the code sits inside the bench-only guard.
log_calls = list(re.finditer(r"ESP_LOG[EWIDV]\((.*?)\);", source, re.S))
code_logs = [call for call in log_calls if re.search(r"\bs_pop\b", call.group(1))]
assert code_logs, "the bench build no longer logs the setup code"
for call in code_logs:
    assert guarded(call.start()), f"unguarded log of the setup code: {call.group(0)!r}"

# The fixed code is defined and used only inside the guard, and it is the
# code that the bench script sends.
bench_code = re.search(r'#define C5_BENCH_POP "(\d{6})"', source)
assert bench_code, "the bench build no longer defines its fixed setup code"
# Comments are blanked out, keeping the offsets of the code.
code = re.sub(r"/\*.*?\*/", lambda comment: " " * len(comment.group(0)), source, flags=re.S)
uses = list(re.finditer(r"\bC5_BENCH_POP\b", code))
assert len(uses) >= 2, "the bench build no longer uses its fixed setup code"
for use in uses:
    assert guarded(use.start()), "C5_BENCH_POP outside the bench-only guard"
assert f'BENCH_SETUP_CODE = "{bench_code.group(1)}"' in reprovision

# The default is off, both in CMake and for sources compiled without it.
assert re.search(r'set\(JETKVM_WIFI_SETUP_LOG_CODE "0" CACHE STRING', root_cmake)
assert "JETKVM_WIFI_SETUP_LOG_CODE=${JETKVM_WIFI_SETUP_LOG_CODE}" in cmake
# A build with the log on says so in its version; the e2e checks for it.
suffix = re.search(r'set\(PROJECT_VER "\$\{PROJECT_VER\}(\+[a-z-]+)"\)', root_cmake)
assert suffix, "the bench build no longer marks its version"
assert root_cmake.index("if(JETKVM_WIFI_SETUP_LOG_CODE)") > root_cmake.index(
    'set(PROJECT_VER "'), "the suffix must follow the cached PROJECT_VER"
assert root_cmake.index("if(JETKVM_WIFI_SETUP_LOG_CODE)") < root_cmake.index(
    "project(jetkvm-mini)"), "the suffix must precede project()"
e2e = (repo / "tools/e2e/p4_wifi_ble_setup_e2e.py").read_text()
assert f'BENCH_VERSION_SUFFIX = "{suffix.group(1)}"' in e2e
assert "#ifndef JETKVM_WIFI_SETUP_LOG_CODE\n#define JETKVM_WIFI_SETUP_LOG_CODE 0\n#endif" in source

# The release script builds with the option off and greps the image for the
# exact format string the firmware logs.
assert "-DJETKVM_WIFI_SETUP_LOG_CODE=0" in release
(format_string,) = [re.search(r'TAG,\s*"([^"]*)"', call.group(1)).group(1) for call in code_logs]
assert f'grep -aqF "{format_string}"' in release, (
    f"release_r2.sh does not look for {format_string!r}")

print("The fixed Wi-Fi setup code and its log exist only in bench builds")

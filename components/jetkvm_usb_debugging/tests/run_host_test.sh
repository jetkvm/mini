#!/usr/bin/env bash
# The USB debugging setting as a release builds it (default off) and as a
# bench build with CONFIG_JETKVM_USB_DEBUGGING_DEFAULT_ON builds it (default on).
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
component="$repo_root/components/jetkvm_usb_debugging"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT

build_and_run() {
  cc -std=c11 -Wall -Wextra -Werror "$@" \
    -I"$component/tests/compat" \
    -I"$component/include" \
    "$component/jetkvm_usb_debugging_setting.c" \
    "$component/tests/test_usb_debugging_setting.c" \
    -o "$binary"
  "$binary"
}

build_and_run -DBENCH_BUILD=0
build_and_run -DBENCH_BUILD=1 -DCONFIG_JETKVM_USB_DEBUGGING_DEFAULT_ON=1

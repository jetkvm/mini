#!/usr/bin/env bash
set -euo pipefail

component_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/jetkvm_usb_hid_host_tests"
mkdir -p "$build_dir"

cc -std=c11 -Wall -Wextra -Werror -pedantic \
    -I"$component_dir" \
    "$component_dir/keyboard_rollover.c" \
    "$component_dir/tests/test_keyboard_rollover.c" \
    -o "$build_dir/test_keyboard_rollover"

"$build_dir/test_keyboard_rollover"

cc -std=c11 -Wall -Wextra -Werror -pedantic \
    -I"$component_dir" \
    "$component_dir/cdrom_toc.c" \
    "$component_dir/tests/test_cdrom_toc.c" \
    -o "$build_dir/test_cdrom_toc"

"$build_dir/test_cdrom_toc"
echo "jetkvm_usb_hid host tests: OK"

#!/usr/bin/env bash
set -euo pipefail

component_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

cc -std=c11 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$component_dir" \
    "$component_dir/jetkvm_screen_fit.c" \
    "$component_dir/tests/test_jetkvm_screen_fit.c" \
    -o "$build_dir/test_jetkvm_screen_fit"

ASAN_OPTIONS=detect_leaks=1 "$build_dir/test_jetkvm_screen_fit"

cc -std=c11 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$component_dir" \
    "$component_dir/jetkvm_screen_button.c" \
    "$component_dir/tests/test_jetkvm_screen_button.c" \
    -o "$build_dir/test_jetkvm_screen_button"

ASAN_OPTIONS=detect_leaks=1 "$build_dir/test_jetkvm_screen_button"
cc -std=c11 -Wall -Wextra -Werror -pedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$component_dir" \
    "$component_dir/jetkvm_screen_orientation.c" \
    "$component_dir/tests/test_jetkvm_screen_orientation.c" \
    -o "$build_dir/test_jetkvm_screen_orientation"

ASAN_OPTIONS=detect_leaks=1 "$build_dir/test_jetkvm_screen_orientation"
python3 "$component_dir/tests/test_lvgl_memory_domain_static.py"
python3 "$component_dir/tests/test_direct_preview_static.py"
"$component_dir/tests/check_ui_contract.sh"

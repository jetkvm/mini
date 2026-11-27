#!/usr/bin/env bash
# Lays out the home screen with LVGL on this machine, for both boards, and
# checks that each IPv4 address stays inside its line and each IPv6 address
# on its two lines, without the button strip and with the strip at each edge
# (host_renderer/test_home_address_fit.c).
# Needs cmake, ninja and managed_components/lvgl__lvgl (idf.py reconfigure).
set -euo pipefail

component_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

# One build directory: LVGL is compiled once and only the screens again for
# the second board.
for variant in wifi ethernet; do
    cmake -S "$component_dir/tests/host_renderer" -B "$build_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DJETKVM_BOARD_VARIANT="$variant" >/dev/null
    cmake --build "$build_dir" --target jetkvm_home_address_fit_test >/dev/null
    echo "$variant board:"
    "$build_dir/jetkvm_home_address_fit_test"
done

#!/usr/bin/env bash
set -euo pipefail

component="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT

cc -std=c11 -Wall -Wextra -Werror \
  -I"$component/tests/compat" \
  -I"$component/include" \
  -I"$component" \
  "$component/jetkvm_flash_read_mode.c" \
  "$component/flash_read_switch.c" \
  "$component/tests/test_flash_read_mode.c" \
  -o "$binary"
"$binary"

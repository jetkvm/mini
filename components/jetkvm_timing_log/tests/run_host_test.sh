#!/usr/bin/env bash
set -euo pipefail

component="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT

cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$component/tests/compat" -I"$component/include" \
  "$component/jetkvm_timing_log.c" "$component/tests/test_jetkvm_timing_log.c" \
  -o "$binary"
"$binary"

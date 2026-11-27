#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/components/jetkvm_timezone/include" \
  -I"$root/components/jetkvm_timezone" \
  "$root/components/jetkvm_timezone/jetkvm_timezone.c" \
  "$root/components/jetkvm_timezone/tests/test_jetkvm_timezone.c" \
  -o "$binary"
"$binary"

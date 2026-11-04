#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
tests="$root/components/jetkvm_tailscale/tests"
binary="$(mktemp)"
generated="$(mktemp --suffix=.h)"
trap 'rm -f "$binary" "$generated"' EXIT
python3 "$tests/extract_functions.py" \
  "$root/third_party/microlink/components/microlink/src/ml_coord.c" coord_read_msg2 > "$generated"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -DCOORD_FUNCTIONS=\"$generated\" "$tests/test_coord_msg2.c" -o "$binary"
"$binary"

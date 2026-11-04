#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
tests="$root/components/jetkvm_tailscale/tests"
cjson="$root/managed_components/espressif__cjson/cJSON"
binary="$(mktemp)"
generated="$(mktemp --suffix=.h)"
trap 'rm -f "$binary" "$generated"' EXIT
python3 "$tests/extract_functions.py" \
  "$root/third_party/microlink/components/microlink/src/ml_coord.c" self_ipv4 > "$generated"
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I"$cjson" \
  -DCOORD_FUNCTIONS=\"$generated\" "$tests/test_coord_self_address.c" "$cjson/cJSON.c" -o "$binary"
"$binary"

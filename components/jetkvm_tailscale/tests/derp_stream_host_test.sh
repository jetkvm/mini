#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I"$root/third_party/microlink/components/microlink/include" \
  "$root/components/jetkvm_tailscale/tests/test_derp_stream.c" \
  "$root/third_party/microlink/components/microlink/src/ml_derp_stream.c" -o "$binary"
"$binary"

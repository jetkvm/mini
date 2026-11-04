#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
tests="$root/components/jetkvm_tailscale/tests"
ml="$root/third_party/microlink/components/microlink/src"
binary="$(mktemp)"
generated="$(mktemp --suffix=.h)"
trap 'rm -f "$binary" "$generated"' EXIT
python3 "$tests/extract_functions.py" "$ml/microlink.c" \
  generate_keypair load_or_generate_keys microlink_factory_reset > "$generated"
cc -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function -g -fsanitize=address,undefined \
  -I"$tests/compat" -I"$ml" -DIDENTITY_FUNCTIONS=\"$generated\" \
  "$tests/test_node_identity.c" "$ml/x25519.c" -o "$binary"
"$binary"

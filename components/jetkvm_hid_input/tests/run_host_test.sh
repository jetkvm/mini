#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT

cc -std=c11 -Wall -Wextra -Werror -pthread \
  -I"$repo_root/components/jetkvm_hid_input/tests/compat" \
  -I"$repo_root/components/jetkvm_hid_input/tests" \
  -I"$repo_root/components/jetkvm_hid_input/include" \
  -I"$repo_root/components/control_protocol/include" \
  "$repo_root/components/jetkvm_hid_input/jetkvm_hid_input.c" \
  "$repo_root/components/jetkvm_hid_input/tests/fake_runtime.c" \
  "$repo_root/components/jetkvm_hid_input/tests/test_jetkvm_hid_input.c" \
  -o "$binary"
"$binary"

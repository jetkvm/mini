#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT

cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/components/jetkvm_identity/tests/compat" \
  -I"$repo_root/components/jetkvm_identity/include" \
  "$repo_root/components/jetkvm_identity/jetkvm_identity_format.c" \
  "$repo_root/components/jetkvm_identity/tests/test_jetkvm_identity.c" \
  -o "$binary"
"$binary"

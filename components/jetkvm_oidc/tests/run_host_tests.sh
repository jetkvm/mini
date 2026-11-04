#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT

cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$repo_root/components/jetkvm_oidc/include" \
  -I"$repo_root/components/jetkvm_oidc/tests" \
  -I"$repo_root/managed_components/espressif__cjson/cJSON" \
  "$repo_root/components/jetkvm_oidc/jetkvm_oidc_core.c" \
  "$repo_root/components/jetkvm_oidc/tests/test_jetkvm_oidc.c" \
  "$repo_root/managed_components/espressif__cjson/cJSON/cJSON.c" \
  -lcrypto -lm -o "$binary"

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
  "$binary"

python3 "$repo_root/components/jetkvm_oidc/tests/test_memory_policy_static.py"

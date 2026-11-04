#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/components/jetkvm_time_sync/include" \
  "$root/components/jetkvm_time_sync/jetkvm_time_sync_servers.c" \
  "$root/components/jetkvm_time_sync/tests/test_time_sync.c" \
  -o "$binary"
"$binary"

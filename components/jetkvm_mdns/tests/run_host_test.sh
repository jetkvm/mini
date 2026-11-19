#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$repo_root/components/jetkvm_mdns/include" \
  -I"$repo_root/components/control_protocol/include" \
  -I"$repo_root/components/jetkvm_virtual_media/include" \
  -I"$repo_root/managed_components/espressif__cjson/cJSON" \
  "$repo_root/components/jetkvm_mdns/jetkvm_mdns_core.c" \
  "$repo_root/components/jetkvm_mdns/tests/test_jetkvm_mdns.c" \
  -o "$binary"
"$binary"

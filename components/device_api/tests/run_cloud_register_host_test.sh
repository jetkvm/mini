#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT

cc -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$repo_root/components/device_api/tests/compat" \
  -I"$repo_root/components/device_api/include" \
  -I"$repo_root/components/jetkvm_factory_reset/include" \
  -I"$repo_root/components/jetkvm_board_config/include" \
  -DJETKVM_BOARD_WIFI=0 -DJETKVM_BOARD_ETHERNET=1 \
  -I"$repo_root/components/jetkvm_access/include" \
  -I"$repo_root/components/jetkvm_cloud_registration/include" \
  -I"$repo_root/components/cloud_signaling/include" \
  -I"$repo_root/components/jetkvm_cloud_config/include" \
  -I"$repo_root/components/jetkvm_oidc/include" \
  -I"$repo_root/managed_components/espressif__cjson/cJSON" \
  "$repo_root/components/device_api/device_api.c" \
  "$repo_root/components/device_api/device_contract.c" \
  "$repo_root/components/device_api/tests/test_cloud_register_api.c" \
  "$repo_root/managed_components/espressif__cjson/cJSON/cJSON.c" \
  -lm -o "$binary"

"$binary"

#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
core_binary="$(mktemp)"
nvs_binary="$(mktemp)"
trap 'rm -f "$core_binary" "$nvs_binary"' EXIT

cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/components/jetkvm_cloud_config/tests/compat" \
  -I"$repo_root/components/jetkvm_cloud_config/include" \
  "$repo_root/components/jetkvm_cloud_config/jetkvm_cloud_config_core.c" \
  "$repo_root/components/jetkvm_cloud_config/tests/test_jetkvm_cloud_config.c" \
  -o "$core_binary"
"$core_binary"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/components/jetkvm_cloud_config/tests/compat" \
  -I"$repo_root/components/jetkvm_cloud_config/tests" \
  -I"$repo_root/components/jetkvm_cloud_config/include" \
  "$repo_root/components/jetkvm_cloud_config/jetkvm_cloud_config_core.c" \
  "$repo_root/components/jetkvm_cloud_config/jetkvm_cloud_config_nvs.c" \
  "$repo_root/components/jetkvm_cloud_config/tests/mock_nvs.c" \
  "$repo_root/components/jetkvm_cloud_config/tests/test_jetkvm_cloud_nvs.c" \
  -o "$nvs_binary"
"$nvs_binary"

#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
core_binary="$(mktemp)"
nvs_binary="$(mktemp)"
trap 'rm -f "$core_binary" "$nvs_binary"' EXIT

common_flags=(
  -std=c11
  -Wall
  -Wextra
  -Werror
  -pedantic
  -I"$repo_root/components/jetkvm_usb_config/tests/compat"
  -I"$repo_root/components/jetkvm_usb_config/include"
)

cc "${common_flags[@]}" \
  "$repo_root/components/jetkvm_usb_config/jetkvm_usb_config_core.c" \
  "$repo_root/components/jetkvm_usb_config/tests/test_jetkvm_usb_config_core.c" \
  -o "$core_binary"
"$core_binary"

cc "${common_flags[@]}" \
  -I"$repo_root/components/jetkvm_usb_config/tests" \
  "$repo_root/components/jetkvm_usb_config/jetkvm_usb_config_core.c" \
  "$repo_root/components/jetkvm_usb_config/jetkvm_usb_config_nvs.c" \
  "$repo_root/components/jetkvm_usb_config/tests/mock_nvs.c" \
  "$repo_root/components/jetkvm_usb_config/tests/test_jetkvm_usb_config_nvs.c" \
  -o "$nvs_binary"
"$nvs_binary"

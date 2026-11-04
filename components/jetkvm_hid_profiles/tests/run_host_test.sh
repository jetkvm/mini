#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT

cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/components/jetkvm_settings/tests/compat" \
  -I"$repo_root/components/jetkvm_hid_profiles/tests" \
  -I"$repo_root/components/jetkvm_hid_profiles/include" \
  -I"$repo_root/components/control_protocol/include" \
  "$repo_root/components/jetkvm_hid_profiles/jetkvm_hid_profiles_core.c" \
  "$repo_root/components/jetkvm_hid_profiles/jetkvm_hid_profiles_nvs.c" \
  "$repo_root/components/jetkvm_hid_profiles/tests/mock_nvs.c" \
  "$repo_root/components/jetkvm_hid_profiles/tests/test_hid_profiles.c" \
  -o "$binary"
"$binary"
python3 "$repo_root/components/jetkvm_hid_profiles/tests/test_key_table.py"
python3 "$repo_root/components/jetkvm_hid_profiles/tests/test_partition.py"

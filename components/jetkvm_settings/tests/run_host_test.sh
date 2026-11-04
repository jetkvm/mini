#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
binary="$(mktemp)"
migration_binary="$(mktemp)"
trap 'rm -f "$binary" "$migration_binary"' EXIT

cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/components/jetkvm_settings/tests/compat" \
  -I"$repo_root/components/jetkvm_settings/tests" \
  -I"$repo_root/components/jetkvm_settings/include" \
  "$repo_root/components/jetkvm_settings/jetkvm_settings_core.c" \
  "$repo_root/components/jetkvm_settings/jetkvm_settings_nvs.c" \
  "$repo_root/components/jetkvm_settings/jetkvm_settings_runtime.c" \
  "$repo_root/components/jetkvm_settings/tests/mock_nvs.c" \
  "$repo_root/components/jetkvm_settings/tests/test_jetkvm_settings.c" \
  -o "$binary"
"$binary"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/components/jetkvm_settings/tests/compat" \
  -I"$repo_root/components/jetkvm_settings/tests" \
  -I"$repo_root/components/jetkvm_settings/include" \
  "$repo_root/components/jetkvm_settings/jetkvm_settings_core.c" \
  "$repo_root/components/jetkvm_settings/jetkvm_settings_nvs.c" \
  "$repo_root/components/jetkvm_settings/jetkvm_settings_runtime.c" \
  "$repo_root/components/jetkvm_settings/tests/mock_nvs.c" \
  "$repo_root/components/jetkvm_settings/tests/test_jetkvm_settings_migration.c" \
  -o "$migration_binary"
"$migration_binary"

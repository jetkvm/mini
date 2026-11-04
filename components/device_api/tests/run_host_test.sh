#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT

cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/components/device_api/include" \
  -I"$repo_root/managed_components/espressif__cjson/cJSON" \
  "$repo_root/components/device_api/device_contract.c" \
  "$repo_root/components/device_api/tests/test_device_contract.c" \
  "$repo_root/managed_components/espressif__cjson/cJSON/cJSON.c" \
  -lm -o "$binary"
"$binary"

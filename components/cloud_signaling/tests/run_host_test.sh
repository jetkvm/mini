#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT

cjson_dir="$repo_root/managed_components/espressif__cjson/cJSON"
if [[ ! -f "$cjson_dir/cJSON.c" ]]; then
  idf_root="${IDF_PATH:-$HOME/esp/esp-idf}"
  cjson_dir="$idf_root/components/json/cJSON"
fi
if [[ ! -f "$cjson_dir/cJSON.c" ]]; then
  echo "cJSON source not found; run the ESP-IDF export script first" >&2
  exit 1
fi

cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -I"$repo_root/components/cloud_signaling/include" \
  -I"$repo_root/components/cloud_signaling/tests/compat" \
  -I"$cjson_dir" \
  "$repo_root/components/cloud_signaling/jetkvm_cloud_signal.c" \
  "$repo_root/components/cloud_signaling/tests/mbedtls_base64_compat.c" \
  "$repo_root/components/cloud_signaling/tests/test_jetkvm_cloud_signal.c" \
  "$cjson_dir/cJSON.c" \
  -lm -o "$binary"
"$binary"

#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/jetkvm_cloud_registration_host_tests"
mkdir -p "$build_dir"

cjson_dir="$repo_root/managed_components/espressif__cjson/cJSON"
if [[ ! -f "$cjson_dir/cJSON.c" ]]; then
  idf_root="${IDF_PATH:-$HOME/esp/esp-idf}"
  cjson_dir="$idf_root/components/json/cJSON"
fi
if [[ ! -f "$cjson_dir/cJSON.c" ]]; then
  echo "cJSON source not found; run the ESP-IDF export script first" >&2
  exit 1
fi

# The release build and a mock cloud build (-DJETKVM_MOCK_CLOUD_JWKS_URL in
# the top-level CMakeLists.txt) differ in the cloud URLs they accept.
for mock_cloud in "" -DJETKVM_MOCK_CLOUD=1; do
  suffix="${mock_cloud:+-mock-cloud}"
  cc -std=c11 -Wall -Wextra -Werror -pedantic ${mock_cloud:+"$mock_cloud"} \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$repo_root/components/jetkvm_cloud_registration/include" \
    -I"$repo_root/components/jetkvm_cloud_registration/tests/compat" \
    -I"$repo_root/components/jetkvm_cloud_config/include" \
    -I"$repo_root/components/jetkvm_cloud_config/tests/compat" \
    -I"$repo_root/components/jetkvm_cloud_config/tests" \
    -I"$cjson_dir" \
    "$repo_root/components/jetkvm_cloud_registration/jetkvm_cloud_registration_core.c" \
    "$repo_root/components/jetkvm_cloud_registration/tests/test_jetkvm_cloud_registration.c" \
    "$repo_root/components/jetkvm_cloud_config/jetkvm_cloud_config_core.c" \
    "$repo_root/components/jetkvm_cloud_config/jetkvm_cloud_config_nvs.c" \
    "$repo_root/components/jetkvm_cloud_config/tests/mock_nvs.c" \
    "$cjson_dir/cJSON.c" -lm \
    -o "$build_dir/test_jetkvm_cloud_registration$suffix"

  "$build_dir/test_jetkvm_cloud_registration$suffix"

  cc -std=c11 -Wall -Wextra -Werror -pedantic ${mock_cloud:+"$mock_cloud"} \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I"$repo_root/components/jetkvm_cloud_registration/include" \
    -I"$repo_root/components/jetkvm_cloud_registration/tests/compat" \
    -I"$repo_root/components/jetkvm_cloud_config/include" \
    -I"$cjson_dir" \
    "$repo_root/components/jetkvm_cloud_registration/jetkvm_cloud_registration_esp.c" \
    "$repo_root/components/jetkvm_cloud_registration/tests/test_deregistration_http.c" \
    "$repo_root/components/jetkvm_cloud_config/jetkvm_cloud_config_core.c" \
    -o "$build_dir/test_deregistration_http$suffix"
  "$build_dir/test_deregistration_http$suffix"
done

python3 \
  "$repo_root/components/jetkvm_cloud_registration/tests/test_memory_layout_static.py"

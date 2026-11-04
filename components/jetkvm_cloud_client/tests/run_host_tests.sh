#!/usr/bin/env bash
set -euo pipefail

component_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/jetkvm_cloud_client_host_tests"
mkdir -p "$build_dir"

cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -I"$component_dir/include" \
  -I"$component_dir/../cloud_signaling/include" \
  "$component_dir/jetkvm_cloud_core.c" \
  "$component_dir/tests/test_jetkvm_cloud_core.c" \
  -o "$build_dir/test_jetkvm_cloud_core"

"$build_dir/test_jetkvm_cloud_core"
python3 "$component_dir/tests/test_memory_policy_static.py"

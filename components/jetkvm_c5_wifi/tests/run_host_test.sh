#!/usr/bin/env bash
set -euo pipefail
component_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output="$(mktemp -d)"
trap 'rm -rf "$output"' EXIT
for test in wifi_network_list wifi_join_failure; do
  cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$component_dir/include" \
    "$component_dir/$test.c" \
    "$component_dir/tests/test_$test.c" \
    -o "$output/test_$test"
  "$output/test_$test"
done

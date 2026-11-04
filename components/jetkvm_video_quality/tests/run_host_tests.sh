#!/usr/bin/env bash
set -euo pipefail

component_dir="$(cd "$(dirname "$0")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/jetkvm_video_quality_host_tests"
mkdir -p "$build_dir"

cc -std=c11 -Wall -Wextra -Werror -pedantic \
    -I"$component_dir/include" \
    "$component_dir/jetkvm_video_quality.c" \
    "$component_dir/tests/test_jetkvm_video_quality.c" \
    -o "$build_dir/test_jetkvm_video_quality"

"$build_dir/test_jetkvm_video_quality"
echo "jetkvm_video_quality host tests: OK"

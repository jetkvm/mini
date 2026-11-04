#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${TMPDIR:-/tmp}/kvm-p4-web-app-host-test"
mkdir -p "$build_dir"

python3 -m unittest -v "$root/tools/web_app/test_pack_web_app.py"

cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -I"$root/components/web_app/private_include" \
  "$root/components/web_app/web_app_pack.c" \
  "$root/components/web_app/tests/test_web_app_pack.c" \
  -o "$build_dir/test_web_app_pack"
"$build_dir/test_web_app_pack"

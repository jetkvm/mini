#!/usr/bin/env bash
set -euo pipefail

component_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${TMPDIR:-/tmp}/kvm-p4-test-http-upload-lifecycle"
discard_binary="${TMPDIR:-/tmp}/kvm-p4-test-http-discard-metrics"

cc -std=c11 -Wall -Wextra -Werror \
  -I"${component_dir}" \
  "${component_dir}/tests/test_upload_lifecycle.c" \
  -o "${binary}"
"${binary}"
cc -std=c11 -Wall -Wextra -Werror \
  -I"${component_dir}" \
  "${component_dir}/tests/test_http_discard_metrics.c" \
  -o "${discard_binary}"
"${discard_binary}"
gate_binary="${TMPDIR:-/tmp}/kvm-p4-test-http-upload-gate"
cc -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -pthread \
  -fsanitize=address,undefined \
  -I"${component_dir}" -I"${component_dir}/tests/compat" \
  "${component_dir}/http_upload_gate.c" \
  "${component_dir}/tests/test_upload_gate.c" \
  -o "${gate_binary}"
"${gate_binary}"
python3 "${component_dir}/tests/test_upload_buffering_static.py"
python3 "${component_dir}/tests/test_upload_allocation.py"

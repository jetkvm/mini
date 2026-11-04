#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="${TMPDIR:-/tmp}/jetkvm_cloud_service_tests"
LIFECYCLE_OUT="${TMPDIR:-/tmp}/jetkvm_cloud_service_lifecycle_tests"

cc -std=c11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$ROOT/components/jetkvm_cloud_service" \
  "$ROOT/components/jetkvm_cloud_service/jetkvm_cloud_service_policy.c" \
  "$ROOT/components/jetkvm_cloud_service/tests/test_jetkvm_cloud_service_policy.c" \
  -o "$OUT"

ASAN_OPTIONS=detect_leaks=1 "$OUT"

cc -std=c11 -Wall -Wextra -Werror -pedantic -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -DJETKVM_BOARD_WIFI=0 -DJETKVM_BOARD_ETHERNET=1 \
  -I"$ROOT/components/jetkvm_board_config/include" \
  -I"$ROOT/components/jetkvm_cloud_service/tests/mocks" \
  -I"$ROOT/components/jetkvm_cloud_service" \
  "$ROOT/components/jetkvm_cloud_service/jetkvm_cloud_service_policy.c" \
  "$ROOT/components/jetkvm_cloud_service/jetkvm_cloud_service.c" \
  "$ROOT/components/jetkvm_cloud_service/tests/mocks/host_fakes.c" \
  "$ROOT/components/jetkvm_cloud_service/tests/test_jetkvm_cloud_service_lifecycle.c" \
  -o "$LIFECYCLE_OUT"

# Production intentionally retains three tiny RTOS synchronization objects so
# an observer callback captured just before unsubscribe can never hit freed
# memory. They are process-lifetime allocations, so omit leak classification.
ASAN_OPTIONS=detect_leaks=0 "$LIFECYCLE_OUT"

python3 \
  "$ROOT/components/jetkvm_cloud_service/tests/test_memory_policy_static.py"

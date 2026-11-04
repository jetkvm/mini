#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/components/jetkvm_device_state/tests/compat" \
  -I"$root/components/jetkvm_cloud_config/tests/compat" \
  -I"$root/components/jetkvm_device_state/include" \
  -I"$root/components/jetkvm_cloud_config/include" \
  -I"$root/components/jetkvm_identity/include" \
  "$root/components/jetkvm_device_state/jetkvm_device_state.c" \
  "$root/components/jetkvm_cloud_config/jetkvm_cloud_config_core.c" \
  "$root/components/jetkvm_device_state/tests/test_deregistration.c" \
  -o /tmp/kvm-mini-cloud-state-test
/tmp/kvm-mini-cloud-state-test

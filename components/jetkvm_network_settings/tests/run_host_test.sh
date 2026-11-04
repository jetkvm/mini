#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
cjson="$root/managed_components/espressif__cjson/cJSON"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/components/jetkvm_network_settings/tests/compat" \
  -I"$root/components/jetkvm_network_settings/include" \
  -I"$root/components/control_protocol/include" \
  -I"$root/components/jetkvm_virtual_media/include" -I"$cjson" \
  "$root"/components/control_protocol/*.c \
  "$root/components/jetkvm_network_settings/network_settings_core.c" \
  "$root/components/jetkvm_network_settings/tests/test_network.c" \
  "$cjson/cJSON.c" -lm -o /tmp/kvm-mini-network-test
/tmp/kvm-mini-network-test

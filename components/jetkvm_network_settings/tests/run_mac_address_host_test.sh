#!/usr/bin/env bash
# The MAC address setting, once as a release build has it and once with the
# bench option CONFIG_JETKVM_BENCH_CUSTOM_MAC.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
cjson="$root/managed_components/espressif__cjson/cJSON"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT
for bench in 0 1; do
    cc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined \
      -DCONFIG_JETKVM_BENCH_CUSTOM_MAC="$bench" \
      -I"$root/components/jetkvm_network_settings/tests/mac_compat" \
      -I"$root/components/jetkvm_network_settings/tests/compat" \
      -I"$root/components/jetkvm_network_settings/include" \
      -I"$root/components/jetkvm_identity/include" \
      -I"$root/components/control_protocol/include" \
      -I"$root/components/jetkvm_virtual_media/include" -I"$cjson" \
      "$root"/components/control_protocol/*.c \
      "$root/components/jetkvm_network_settings/network_settings_core.c" \
      "$root/components/jetkvm_identity/jetkvm_identity_format.c" \
      "$root/components/jetkvm_identity/jetkvm_identity_esp.c" \
      "$root/components/jetkvm_network_settings/tests/test_mac_address.c" \
      "$cjson/cJSON.c" -lm -o "$binary"
    "$binary"
done

#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT

cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/components/jetkvm_mqtt/tests/compat" \
  -I"$repo_root/components/jetkvm_mqtt/include" \
  -I"$repo_root/components/control_protocol/include" \
  "$repo_root/components/jetkvm_mqtt/jetkvm_mqtt_config.c" \
  "$repo_root/components/jetkvm_mqtt/tests/test_mqtt_config.c" \
  -o "$binary"
"$binary"
python3 "$repo_root/components/jetkvm_mqtt/tests/test_mqtt_publish_drain.py"
python3 "$repo_root/components/jetkvm_mqtt/tests/test_mqtt_values.py"
python3 "$repo_root/components/jetkvm_mqtt/tests/test_mqtt_virtual_media.py"

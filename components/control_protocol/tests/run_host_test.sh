#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT

cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/components/control_protocol/include" \
  -I"$repo_root/components/jetkvm_virtual_media/include" \
  -I"$repo_root/managed_components/espressif__cjson/cJSON" \
  "$repo_root/components/control_protocol/jetkvm_control.c" \
  "$repo_root/components/control_protocol/rpc_video.c" \
  "$repo_root/components/control_protocol/rpc_network.c" \
  "$repo_root/components/control_protocol/rpc_display_settings.c" \
  "$repo_root/components/control_protocol/rpc_usb.c" \
  "$repo_root/components/control_protocol/rpc_wol.c" \
  "$repo_root/components/control_protocol/rpc_system.c" \
  "$repo_root/components/control_protocol/rpc_ota.c" \
  "$repo_root/components/control_protocol/rpc_tls.c" \
  "$repo_root/components/control_protocol/rpc_extra_hostnames.c" \
  "$repo_root/components/control_protocol/rpc_tailscale.c" \
  "$repo_root/components/control_protocol/rpc_mqtt.c" \
  "$repo_root/components/control_protocol/rpc_macros.c" \
  "$repo_root/components/control_protocol/rpc_virtual_media.c" \
  "$repo_root/components/control_protocol/rpc_os_services.c" \
  "$repo_root/components/control_protocol/jetkvm_hidrpc.c" \
  "$repo_root/components/control_protocol/tests/test_jetkvm_control.c" \
  "$repo_root/components/control_protocol/tests/test_os_services.c" \
  "$repo_root/managed_components/espressif__cjson/cJSON/cJSON.c" \
  -lm -o "$binary"
"$binary"

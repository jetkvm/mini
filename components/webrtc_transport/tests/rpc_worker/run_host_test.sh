#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../../.." && pwd)"
cjson="$root/managed_components/espressif__cjson/cJSON"
out="${TMPDIR:-/tmp}"
includes=(
  -I"$root/components/webrtc_transport/tests/rpc_worker/compat"
  -I"$root/components/jetkvm_device_state/tests/compat"
  -I"$root/components/jetkvm_cloud_config/tests/compat"
  -I"$root/components/webrtc_transport"
  -I"$root/components/control_protocol/include"
  -I"$root/components/jetkvm_cloud_registration/include"
  -I"$root/components/jetkvm_device_state/include"
  -I"$root/components/jetkvm_identity/include"
  -I"$root/components/jetkvm_cloud_config/include" -I"$cjson"
)
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined "${includes[@]}" \
  "$root/components/webrtc_transport/rpc_worker.c" \
  "$root/components/webrtc_transport/tests/rpc_worker/test_job.c" \
  "$root/components/webrtc_transport/tests/rpc_worker/control_arena.c" \
  "$root/components/jetkvm_cloud_config/jetkvm_cloud_config_core.c" \
  "$cjson/cJSON.c" -lm -o "$out/kvm-mini-rpc-worker-job-test"
"$out/kvm-mini-rpc-worker-job-test"

# The media test runs the real JSON-RPC dispatcher, so a deferred reply is
# compared byte for byte with the one the peer task would have sent.
control="$root/components/control_protocol"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined "${includes[@]}" \
  -I"$root/components/jetkvm_virtual_media/include" \
  "$root/components/webrtc_transport/rpc_worker.c" \
  "$root/components/webrtc_transport/tests/rpc_worker/test_media.c" \
  "$root/components/webrtc_transport/tests/rpc_worker/test_guest.c" \
  "$root/components/webrtc_transport/tests/rpc_worker/test_mqtt.c" \
  "$root/components/webrtc_transport/tests/rpc_worker/test_update.c" \
  "$root/components/webrtc_transport/tests/rpc_worker/control_arena.c" \
  "$control/jetkvm_control.c" "$control/jetkvm_hidrpc.c" "$control/rpc_video.c" \
  "$control/rpc_network.c" "$control/rpc_display_settings.c" "$control/rpc_usb.c" \
  "$control/rpc_wol.c" "$control/rpc_system.c" "$control/rpc_ota.c" \
  "$control/rpc_tls.c" "$control/rpc_extra_hostnames.c" "$control/rpc_tailscale.c" "$control/rpc_mqtt.c" "$control/rpc_macros.c" \
  "$control/rpc_virtual_media.c" "$control/rpc_os_services.c" \
  "$root/components/jetkvm_cloud_config/jetkvm_cloud_config_core.c" \
  "$cjson/cJSON.c" -lm -o "$out/kvm-mini-rpc-worker-media-test"
"$out/kvm-mini-rpc-worker-media-test"

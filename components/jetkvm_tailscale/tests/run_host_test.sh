#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
cjson="$root/managed_components/espressif__cjson/cJSON"
binary="$(mktemp)"
private_cjson="$(mktemp --suffix=.c)"
trap 'rm -f "$binary" "$private_cjson"' EXIT
python3 "$root/third_party/microlink/components/microlink/generate_cjson.py" \
  "$cjson/cJSON.c" "$private_cjson"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/components/jetkvm_tailscale/tests/compat" \
  -I"$root/components/jetkvm_tailscale/include" \
  -I"$root/components/control_protocol/include" \
  -I"$root/components/jetkvm_virtual_media/include" -I"$cjson" \
  -I"$root/third_party/microlink/components/microlink/include" \
  "$root"/components/control_protocol/*.c \
  "$root/components/jetkvm_tailscale/tailscale_config.c" \
  "$root/components/jetkvm_tailscale/tests/test_tailscale.c" \
  "$root/components/jetkvm_tailscale/tests/test_packet_filter.c" \
  "$root/components/jetkvm_tailscale/tests/test_h2_message.c" \
  "$root/components/jetkvm_tailscale/tests/test_peer_map.c" \
  "$root/components/jetkvm_tailscale/tests/test_peer_select.c" \
  "$root/components/jetkvm_tailscale/tests/test_key_expiry.c" \
  "$root/third_party/microlink/components/microlink/src/ml_peer_map.c" \
  "$root/third_party/microlink/components/microlink/src/ml_peer_select.c" \
  "$root/third_party/microlink/components/microlink/src/ml_key_expiry.c" \
  "$root/third_party/microlink/components/microlink/src/ml_timestamp.c" \
  "$root/third_party/microlink/components/microlink/src/ml_h2_message.c" \
  "$root/third_party/microlink/components/microlink/src/ml_packet_filter.c" \
  "$private_cjson" -lm -o "$binary"
"$binary"
cc -std=c11 -Wall -Wextra -Werror -Wno-sign-compare -fsanitize=address,undefined \
  -I"$root/components/jetkvm_tailscale/tests/settings_host" \
  -I"$root/components/jetkvm_tailscale/include" \
  -I"$root/components/control_protocol/include" \
  -I"$root/components/jetkvm_network_settings/include" \
  -I"$root/third_party/microlink/components/microlink/include" \
  "$root/components/jetkvm_tailscale/tailscale.c" \
  "$root/components/jetkvm_tailscale/tailscale_config.c" \
  "$root/components/jetkvm_tailscale/tests/test_tailscale_settings.c" -o "$binary"
"$binary"
cc -std=c11 -Wall -Wextra -Werror -Wno-sign-compare -fsanitize=address,undefined \
  -I"$root/components/jetkvm_tailscale/tests/settings_host" \
  -I"$root/components/jetkvm_tailscale/include" \
  -I"$root/components/control_protocol/include" \
  -I"$root/components/jetkvm_network_settings/include" \
  -I"$root/third_party/microlink/components/microlink/include" \
  "$root/components/jetkvm_tailscale/tailscale_config.c" \
  "$root/components/jetkvm_tailscale/tests/test_tailscale_worker.c" -o "$binary"
"$binary"
wg="$root/third_party/microlink/components/wireguard_lwip/src"
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -I"$wg" -I"$wg/crypto/refc" \
  "$root/components/jetkvm_tailscale/tests/test_chacha20poly1305.c" "$wg/crypto.c" \
  "$wg"/crypto/refc/{chacha20,chacha20poly1305,poly1305-donna}.c -o "$binary"
"$binary"
ml="$root/third_party/microlink/components/microlink/src"
cc -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined \
  -I"$root/components/jetkvm_tailscale/tests/compat" -I"$ml" \
  "$root/components/jetkvm_tailscale/tests/test_nacl_box_cache.c" "$ml/nacl_box.c" "$ml/x25519.c" -o "$binary"
"$binary"
bash "$root/components/jetkvm_tailscale/tests/wireguard_host_test.sh"
bash "$root/components/jetkvm_tailscale/tests/derp_stream_host_test.sh"
bash "$root/components/jetkvm_tailscale/tests/coord_msg2_host_test.sh"
bash "$root/components/jetkvm_tailscale/tests/node_identity_host_test.sh"
python3 "$root/components/jetkvm_tailscale/tests/test_derp_queue.py"

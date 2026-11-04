#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
lwip="${IDF_PATH:-/nonexistent}/components/lwip/lwip/src"
if [ ! -d "$lwip/core" ]; then
    echo "SKIP encrypted WireGuard regression: needs IDF_PATH"
    exit 0
fi
binary="$(mktemp)"
generated="$(mktemp -d)"
trap 'rm -f "$binary"; rm -rf "$generated"' EXIT
tests="$root/components/jetkvm_tailscale/tests"
wg="$root/third_party/microlink/components/wireguard_lwip/src"
ml="$root/third_party/microlink/components/microlink/src"
cjson="$root/managed_components/espressif__cjson/cJSON"
python3 "$tests/extract_peer_roaming.py" \
  "${WIREGUARD_TEST_MANAGER_SOURCE:-$ml/ml_wg_mgr.c}" \
  "$generated/types.h" "$generated/functions.h" "$generated/fill.h"
for test_source in test_wireguard_input.c test_peer_roaming.c test_peer_fill.c; do
cc -std=c11 -g -fsanitize=address,undefined -DWIREGUARD_CRYPTO_REFC=1 \
  -DPEER_ROAMING_TYPES=\"$generated/types.h\" -DPEER_ROAMING_FUNCTIONS=\"$generated/functions.h\" \
  -DPEER_FILL_FUNCTIONS=\"$generated/fill.h\" \
  -I"$tests/lwip_host" -I"$lwip/include" -I"$wg" -I"$wg/crypto/refc" \
  -I"$root/third_party/microlink/components/microlink/include" -I"$cjson" \
  "$tests/$test_source" "$wg/wireguard.c" "${WIREGUARD_TEST_INPUT_SOURCE:-$wg/wireguardif.c}" "$wg/crypto.c" \
  "$wg"/crypto/refc/{blake2s,chacha20,chacha20poly1305,poly1305-donna,x25519}.c \
  "$lwip"/core/*.c "$lwip"/core/ipv4/*.c "$lwip"/core/ipv6/*.c \
  "$ml/ml_packet_filter.c" "$ml/ml_peer_map.c" "$ml/ml_peer_select.c" "$ml/ml_timestamp.c" \
  "$cjson/cJSON.c" -lm -o "$binary"
"$binary"
done
cc -std=c11 -g -fsanitize=address,undefined -DWIREGUARD_CRYPTO_REFC=1 \
  -I"$tests/lwip_host" -I"$lwip/include" -I"$wg" -I"$wg/crypto/refc" \
  "$tests/test_wireguard_initiation.c" "$wg/wireguard.c" "$wg/crypto.c" \
  "$wg"/crypto/refc/{blake2s,chacha20,chacha20poly1305,poly1305-donna,x25519}.c \
  "$lwip"/core/*.c "$lwip"/core/ipv4/*.c "$lwip"/core/ipv6/*.c -lm -o "$binary"
"$binary"

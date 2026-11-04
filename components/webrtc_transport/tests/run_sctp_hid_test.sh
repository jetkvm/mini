#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
dir="$(mktemp -d)"
trap 'rm -rf "$dir"' EXIT
flags=(-std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined
       -I"$root/components/webrtc_transport")
cc "${flags[@]}" \
 "$root/components/webrtc_transport/peer_sctp_hid.c" \
 "$root/components/webrtc_transport/tests/test_peer_sctp_hid.c" -o "$dir/hid"
cc "${flags[@]}" \
 "$root/components/webrtc_transport/peer_sctp_reconfig.c" \
 "$root/components/webrtc_transport/tests/test_peer_sctp_reconfig.c" -o "$dir/reconfig"
cc "${flags[@]}" \
 "$root/components/webrtc_transport/peer_sctp_receive.c" \
 "$root/components/webrtc_transport/tests/test_peer_sctp_receive.c" -o "$dir/receive"
"$dir/hid"
"$dir/reconfig"
"$dir/receive"

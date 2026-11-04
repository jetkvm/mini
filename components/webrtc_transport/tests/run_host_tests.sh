#!/usr/bin/env bash
set -euo pipefail

component_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_dir="$(cd "${component_dir}/../.." && pwd)"
binary="${TMPDIR:-/tmp}/kvm-p4-test-playout-delay"
policy_binary="${TMPDIR:-/tmp}/kvm-p4-test-controlled-session-policy"
rpc_event_binary="${TMPDIR:-/tmp}/kvm-p4-test-rpc-events"
reboot_flow_binary="${TMPDIR:-/tmp}/kvm-p4-test-reboot-flow"
signaling_budget_binary="${TMPDIR:-/tmp}/kvm-p4-test-signaling-budget"
http_candidate_policy_binary="${TMPDIR:-/tmp}/kvm-p4-test-http-candidate-policy"
websocket_origin_binary="${TMPDIR:-/tmp}/kvm-p4-test-websocket-origin"
rtcp_feedback_binary="${TMPDIR:-/tmp}/kvm-p4-test-rtcp-feedback"
h264_profile_binary="${TMPDIR:-/tmp}/kvm-p4-test-h264-profile"
data_channel_queue_binary="${TMPDIR:-/tmp}/kvm-p4-test-data-channel-queue"

cc -std=c11 -Wall -Wextra -Werror \
  -I"${component_dir}" \
  "${component_dir}/websocket_origin.c" \
  "${component_dir}/tests/test_websocket_origin.c" \
  -o "${websocket_origin_binary}"
"${websocket_origin_binary}"

cc -std=c11 -Wall -Wextra -Werror \
  -I"${component_dir}" \
  "${component_dir}/rtcp_feedback.c" \
  "${component_dir}/tests/test_rtcp_feedback.c" \
  -o "${rtcp_feedback_binary}"
"${rtcp_feedback_binary}"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"${component_dir}" \
  "${component_dir}/h264_profile.c" \
  "${component_dir}/tests/test_h264_profile.c" \
  -o "${h264_profile_binary}"
"${h264_profile_binary}"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"${component_dir}" \
  "${component_dir}/data_channel_queue.c" \
  "${component_dir}/tests/test_data_channel_queue.c" \
  -o "${data_channel_queue_binary}"
"${data_channel_queue_binary}"
python3 "${component_dir}/tests/test_h264_au_queue.py"
python3 "${component_dir}/tests/test_au_reservation.py"
python3 "${component_dir}/tests/test_slot_wake.py"
python3 "${component_dir}/tests/test_guest_mode.py"
python3 "${component_dir}/tests/test_peer_rx_slice.py"
python3 "${component_dir}/tests/test_guest_buffers.py"
python3 "${component_dir}/tests/test_no_signal_peer.py"

cc -std=c11 -Wall -Wextra -Werror \
  -I"${component_dir}" \
  "${component_dir}/playout_delay.c" \
  "${component_dir}/tests/test_playout_delay.c" \
  -o "${binary}"
"${binary}"
cc -std=c11 -Wall -Wextra -Werror \
  -I"${component_dir}" \
  "${component_dir}/controlled_session_policy.c" \
  "${component_dir}/tests/test_controlled_session_policy.c" \
  -o "${policy_binary}"
"${policy_binary}"
cc -std=c11 -Wall -Wextra -Werror \
  -I"${component_dir}" \
  "${component_dir}/rpc_events.c" \
  "${component_dir}/tests/test_rpc_events.c" \
  -o "${rpc_event_binary}"
"${rpc_event_binary}"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"${component_dir}" \
  "${component_dir}/data_channel_queue.c" \
  "${component_dir}/reboot_flow.c" \
  "${component_dir}/tests/test_reboot_flow.c" \
  -o "${reboot_flow_binary}"
"${reboot_flow_binary}"
cc -std=c11 -Wall -Wextra -Werror \
  -I"${component_dir}" \
  "${component_dir}/tests/test_signaling_budget.c" \
  -o "${signaling_budget_binary}"
"${signaling_budget_binary}"
cc -std=c11 -Wall -Wextra -Werror \
  -I"${component_dir}" \
  "${component_dir}/http_candidate_policy.c" \
  "${component_dir}/tests/test_http_candidate_policy.c" \
  -o "${http_candidate_policy_binary}"
"${http_candidate_policy_binary}"
guest_channels_binary="${TMPDIR:-/tmp}/kvm-p4-test-guest-channels"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"${component_dir}" -I"${component_dir}/include" \
  "${component_dir}/guest_channels.c" \
  "${component_dir}/tests/test_guest_channels.c" \
  -o "${guest_channels_binary}"
"${guest_channels_binary}"
node "${component_dir}/tests/check_controlled_session.mjs"
node "${component_dir}/tests/check_reboot_ordering.mjs"
node "${component_dir}/tests/check_acceptance_page.mjs"
node "${component_dir}/tests/check_virtual_media_upload.mjs"
node "${component_dir}/tests/check_signaling_memory.mjs"
node "${component_dir}/tests/check_rpc_buffer_lifetime.mjs"
node "${component_dir}/tests/check_tls_memory.mjs"
node "${component_dir}/tests/check_http_task_stack.mjs"
node "${component_dir}/tests/check_peer_memory_arena.mjs"
python3 "${component_dir}/tests/test_peer_memory_snapshot.py"
python3 "${component_dir}/tests/test_task_stack_snapshot.py"
python3 "${component_dir}/tests/test_stats_response_lifetime.py"
python3 "${component_dir}/tests/test_stats_video_encode.py"
python3 "${component_dir}/tests/test_stats_video_diagnostics.py"
python3 "${component_dir}/tests/test_stats_hdmi_receiver.py"
python3 "${component_dir}/tests/test_audio_pause.py"
python3 "${component_dir}/tests/test_stats_audio.py"
node "${component_dir}/tests/check_peer_udp_timeout.mjs"

echo "webrtc_transport host tests: OK (${repo_dir})"
python3 "$component_dir/tests/test_websocket_takeover.py"
python3 "$component_dir/tests/test_websocket_close_reuse.py"
python3 "$component_dir/tests/test_peer_ice_reflexive.py"
adaptive_binary="${TMPDIR:-/tmp}/kvm-p4-test-adaptive-bitrate"
cc -std=c11 -Wall -Wextra -Werror -I"$component_dir" \
  "$component_dir/adaptive_bitrate.c" "$component_dir/tests/test_adaptive_bitrate.c" \
  -o "$adaptive_binary"
"$adaptive_binary"
python3 "$component_dir/tests/test_bitrate_replay_traces.py"

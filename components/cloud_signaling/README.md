# JetKVM cloud signaling codec

This component parses and encodes JetKVM cloud WebRTC signaling messages: `offer`, `answer`, and `new-ice-candidate`. It opens no socket and owns no peer connection. `jetkvm_cloud_client` and the local WebSocket in `webrtc_transport` both use it. The wire format, including `OidcGoogle`, the string-array `iceServers`, and base64 JSON session descriptions, matches upstream JetKVM commit `b3c29a44d9e2862b8ff7530830781803ce27b060`; vectors are in `tests/upstream_fixtures.h`.

- `jetkvm_cloud_signal_parse_offer()` and `jetkvm_cloud_signal_parse_candidate()` fill a zero-initialized owning result. Release it with the matching `_free()` function. On error, the result stays all zero.
- The `encode` functions write into a caller-owned buffer and never leave a truncated message. `JETKVM_CLOUD_SIGNAL_MAX_WIRE_BUFFER_SIZE` fits any message.
- The parser rejects malformed or oversized input, including duplicate fields, invalid UTF-8, and non-canonical base64, before it reaches WebRTC.

Host tests: `tests/run_host_test.sh`.

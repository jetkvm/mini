# SCTP data channels

The local `third_party/esp_peer` source component implements SCTP for WebRTC data
channels. Its channel capacity, stream reset support, receive window and SDP
message limit are part of the sources and build with the firmware toolchain.

The Mini uses reliable `rpc` and `hidrpc` channels, two partially reliable HID
channels, and OS Services terminal and file channels. The association provides
ten slots and announces twenty stream IDs per direction because browsers use
one parity of stream IDs. INIT and INIT ACK advertise both FORWARD-TSN and
RE-CONFIG support.

## Receive and channel lifetime

`peer_sctp_receive.c` reserves enough receive-cache space for the longest
permitted message, so out-of-order traffic cannot prevent the next cumulative
TSN from completing. The SDP limit is 163840 bytes (160 KiB); keep it below
`PEER_SCTP_RECEIVE_RESERVE` in `peer_sctp_hid_internal.h`.

`peer_sctp_reconfig.c` implements channel close with RFC 6525 outgoing stream
resets. It acknowledges the complete request sequence, waits for DATA to be
acknowledged before resetting, batches queued streams and retransmits requests
with backoff. A slot is released only after both directions reset. Stream
lookups find an existing channel before choosing a free slot.

`peer_sctp_hid.c` handles FORWARD-TSN, delivery of partially reliable data and
DCEP acknowledgements. The shared integration layout has compile-time size and
offset checks in both the core and adapters.

## Sending

The core checks SACKs every millisecond. DATA retransmission retains its own
300 ms timer. `dtls_srtp_write()` returns zero on a complete successful write,
matching the SCTP writer contract. Failed or partial DTLS writes cannot be
recorded as successful sends.

Only reliable `hidrpc` input changes mouse buttons. Movement on partially
reliable channels reuses the buttons accepted in the current input epoch. Queue
overflow on a reliable button transition starts a new epoch and releases input.

## Tests

- `python3 -m unittest tools.tests.test_esp_peer_source_transport` checks SCTP
  announcements and the SDP message limit from the production sources.
- `components/webrtc_transport/tests/run_sctp_hid_test.sh` tests receive,
  partially reliable delivery and stream reset behavior.
- `components/jetkvm_hid_input/tests/run_host_test.sh` tests button ownership.

Browser and USB behavior also require testing on a device.

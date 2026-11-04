# esp_peer

The peer core is a locally maintained fork of
[sepfy/libpeer](https://github.com/sepfy/libpeer), reconstructed and extended to
reproduce the peer functionality and public API of Espressif `esp_peer` 1.5.5
on ESP32-P4.

Espressif's published API, DTLS/SRTP and transport wrappers are retained, with JetKVM adaptations included
directly in the sources. Public headers are in `include/`; peer, ICE, RTP and
SCTP internals are in `src/core/`. ESP-IDF builds the complete component with
the firmware toolchain.

The JetKVM SCTP channel, receive-window and ICE integration is included directly
in the sources. DTLS fingerprint validation, fail-closed SRTP, UDP lifetime and
memory allocation fixes are included as well. The application provides the
`media_lib_*` allocator overrides and `jetkvm_*` transport hooks.

Dependencies: ESP-IDF, Mbed TLS, ESP-TLS and libSRTP. The project registers this
folder through `EXTRA_COMPONENT_DIRS` and depends on the `esp_peer` component.

Copyright and licensing notices are preserved in `LICENSE` and `LICENSES/`.

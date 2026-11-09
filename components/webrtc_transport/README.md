# WebRTC transport

This component sends H.264 Annex-B access units from the ESP32-P4 hardware encoder to one browser over WebRTC (H.264 over RTP, ICE, DTLS-SRTP). It also carries the UI's JSON-RPC and HID data channels and serves the local signaling and diagnostic HTTP endpoints. It supports three source modes: 1920×1080 at 30 FPS, 1280×720 at 30 FPS, and 1280×720 at 60 FPS.

`main` calls `webrtc_transport_start()` after network startup; it also starts the HTTP server. Data channels and the patched SCTP stack are described in [README-sctp-hid.md](README-sctp-hid.md). Virtual-media uploads use local HTTP; see the [virtual-media contract](../jetkvm_virtual_media/README.md).

## Video pipeline

The encoder delivers access units through `app_h264_frame_sink_submit()`, which derives the active mode from the encoder metadata. The callback copies each access unit into a preallocated PSRAM slot (`CONFIG_WEBRTC_TRANSPORT_AU_SLOTS`) and never runs ICE, DTLS, SRTP, RTP, or socket work. When all slots are busy, the access unit is dropped and the sender requests an IDR before it sends dependent frames. The peer task sends the newest ready access unit before its network poll.

Timestamps cross the public boundary at 90 kHz. `esp_peer` takes milliseconds, so the component passes `pts_90khz / 90` and the RTP layer converts back. A new peer waits for an access unit with SPS, PPS, and IDR before it transmits.

A new browser session closes the old peer and creates a new SDP and track generation. A mode change or HDMI signal loss keeps the peer, as upstream does: the transport starts a new video epoch, drops access units from the old one, sends `videoInputState`, and waits for a sync IDR in the new mode.

Before the video owner deletes the encoder, it must call `webrtc_transport_clear_mode_and_wait()`, which returns once no access unit is being filled or sent. If a pipeline rebuild runs out of memory, the owner calls `webrtc_transport_release_peer_and_wait()` to free the peer's ICE, DTLS, SRTP, and RTP memory, then retries. The browser reconnects.

## Signaling

The device UI at `/` uses upstream JetKVM WebSocket signaling at `/webrtc/signaling/client`. Messages are parsed and encoded with the [`cloud_signaling`](../cloud_signaling/README.md) codec:

- The device sends `device-metadata` on connect.
- The browser sends an `offer`; the device returns an `answer`. Both carry base64 JSON session descriptions.
- Both sides trickle `new-ice-candidate` messages. A text `ping` receives `pong`.

Constraints on the WebSocket:

- The upgrade requires an HTTP(S) `Origin` whose host and port match `Host`. Other requests get HTTP 403 and do not replace the active client. Non-browser clients must send the device UI origin.
- The newest connection replaces the previous one. A peer failure or memory release closes the socket so the UI reconnects with a fresh offer.
- Messages are limited to `JETKVM_CLOUD_SIGNAL_MAX_MESSAGE_SIZE` (64 KiB). A socket closes after 90 seconds without a browser message.

The local WebSocket session is separate from cloud sessions and from diagnostic HTTP polling.

## Local ICE policy

Local ICE accepts only component-1 UDP host candidates with an IPv4 or mDNS address. When the browser and device share a private network or `/24` (see `webrtc_http_candidate_policy_begin()`), candidates are rewritten to the WebSocket peer's IPv4 address. This handles browser mDNS candidates and browsers on another subnet or VLAN. A rejected candidate is dropped without closing signaling. STUN and TURN apply only to the cloud path.

DTLS setup waits at least 100 ms per receive (`CONFIG_WEBRTC_TRANSPORT_AGENT_RECV_TIMEOUT_MS`), because the vendor DTLS adapter treats eleven empty receives as a closed connection. Connected sessions use bounded non-blocking receive slices.

## JSON-RPC workers

The peer task runs JSON-RPC handlers inside `esp_peer_main_loop`, so a blocking handler also stalls video and HID input. `rpc_worker.c` runs slow methods on worker tasks with separate bounded queues: control (`factoryReset`, `deregisterDevice`, `setUsbEmulationState`), media (virtual media and USB configuration), guest (OS Services clipboard and file drop), MQTT, and update checks. Media requests run in arrival order, so an `unmountImage` followed by `mountWithStorage` swaps the image without a gap.

- When a queue is full, the peer task returns the owning layer's BUSY error and does not run the method itself.
- A request is admitted only for the current session and access epoch; the worker checks the epoch again before it starts.
- The reply goes to the originating peer, generation, and stream, and is dropped if that session ended.

## Adaptive bitrate

The `esp_peer` `on_remb` callback supplies the receiver's estimated bandwidth (REMB). The controller in `adaptive_bitrate.c` runs only while stream quality is Auto (factor 0), matching upstream. Auto's ceiling is the High rate for the current mode. High, Medium, and Low are fixed rates. The encoder worker applies a new target by changing the hardware rate-control bitrate in place, without rebuilding capture, the encoder, or the peer.

The controller evaluates at most once per second, consumes each REMB report once, and ignores reports older than five seconds. It combines REMB, the achieved send rate, socket backpressure, and RTCP receiver-report loss:

- It cuts to 90% of the achieved rate or of REMB, whichever is lower, after two consecutive intervals with at least 2% loss. One lossy interval never cuts.
- It cuts to 90% of REMB on socket backpressure while the sender is saturating. Slot drops (`accessUnitsNoSlot`) count only within 30 seconds of a loss cut; large frames fill both slots on any network.
- Without recent loss, a falling REMB never cuts, because Chrome reports large frame bursts as overuse.
- Otherwise it holds, then recovers toward the ceiling: it doubles the target when the sender is application-limited, or adds 10% after a three-second delay.

Targets stay between 256 kbit/s and the ceiling. A new peer starts at the ceiling. TWCC (transport-wide congestion control) is not used. `tests/bitrate_replay.c` replays recorded traces through the controller on the host.

## Diagnostics

`http://<device-ip>/webrtc` serves a diagnostic page that signals over HTTP and exposes `window.kvmAcceptance.start()`, `stop()`, and `snapshot()`. Its endpoints:

- `POST /webrtc/session` creates or replaces the single browser session.
- `GET /webrtc/events?session=...` polls device offer and ICE events.
- `POST /webrtc/signal` submits the browser answer, ICE, and bye messages.
- `GET /webrtc/stats` returns access-unit, RTP, drop, heap, and peer counters.

`/webrtc/stats` has three views. The default view is cheap and safe during streaming; heap and task-stack details in it are cached samples. `?details=1` takes fresh heap and stack samples with interrupts masked, which disturbs video timing, so do not use it for frame-rate or latency measurements. `?bitrate=1` returns the controller inputs (`videoBitrateStep*`), receiver loss, peer-loop timing, frame-interval histograms (reset with `&resetVideoIntervals=1`), and the `guestVideo`, `videoEncode`, and `hdmiReceiver` objects. Field meanings are documented in `webrtc_stats_json.c` and `include/webrtc_transport.h`. Stats requests read cached copies and never touch the HDMI receiver chip.

`/webrtc/stats?timing=<action>` returns the timing log in place of the stats: when each keyboard report left on USB and when each OS Services video frame was complete, both on the Mini's clock (`jetkvm_timing_log.h`). A tool that measures the time from a key to the frame that shows it reads both times here, so the network is not part of its result. `timing=start` starts an empty log, `timing=read` reads it, and `timing=stop` reads it and frees its memory.

## esp_peer dependency

The local `esp_peer` source component is in `third_party/esp_peer`. Managed dependencies include `espressif/cjson` and `espressif/esp_libsrtp`. Required DTLS-SRTP and X.509 options are in `sdkconfig.defaults`.

Without a hook, the esp_peer ICE agent rejects an authenticated Binding Request whose source endpoint is not in the signaled checklist, which can stall the first connection. After its MESSAGE-INTEGRITY check, `third_party/esp_peer/src/core/agent.c` calls `jetkvm_ice_authenticated_pair()` in `peer_ice_reflexive.c`, which adds the observed IPv4 endpoint. Learned endpoints use esp_peer's internal `host` type and are not signaled. A change to the esp_peer source must keep this call site, the hook's arguments, and the STUN message layout that the hook reads.

Host tests run from `tests/run_host_tests.sh`.

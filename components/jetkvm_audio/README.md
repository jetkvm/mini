# Mini USB audio

This component exposes an optional USB Audio Class 2 speaker to the host and streams its sound to the browser as mono Opus. The host sends 48 kHz stereo 16-bit PCM. There is no HDMI audio and no microphone path.

`getAudioConfig` and `setAudioConfig` follow the upstream JetKVM UI contract and store the preference in NVS. The USB audio class is a separate USB setting, on by default. Audio flows only when the class is active, the preference is on, the host is streaming, and a peer is connected. `jetkvm_usb_hid` drives `jetkvm_audio_prepare_usb()` and the USB state calls; `webrtc_transport` pulls packets with `jetkvm_audio_pop()` and reports counters in `/webrtc/stats`.

A worker averages each 20 ms stereo frame to mono and encodes it at 64 kbit/s. The USB function stays stereo, so one-channel sound arrives at half amplitude. The SDP `rtpmap` stays `opus/48000/2`, as RFC 7587 requires. The packet queue drops its oldest packet when full. USB, preference, peer, or video-hold changes flush it.

## Constraints

- The worker runs on CPU0 above HTTP and below TCP/IP, because the USB FIFO holds only about 42 ms. CPU1 belongs to the H.264 encoder.
- The codec, buffers, and 64 KiB worker stack are allocated once in PSRAM and never freed. If allocation fails, USB enumerates without audio.
- Preference writes release the mutex during the NVS commit so flash writes cannot block FIFO reads.
- The endpoint is synchronous, with no feedback endpoint and no hardware volume or mute.

The encoder is pinned to [esp_audio_codec 2.6.2](https://components.espressif.com/components/espressif/esp_audio_codec/versions/2.6.2).

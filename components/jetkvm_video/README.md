# JetKVM video pipeline

This component captures HDMI through the TC358743 bridge and encodes it with the ESP32-P4 hardware H.264 encoder for `webrtc_transport`. Its public functions are in `include/jetkvm_video.h`.

```text
HDMI -> TC358743 (YUV422) -> MIPI CSI-2 -> P4 CSI_CM (O_UYY_E_VYY) -> H.264 -> WebRTC
```

The CPU never touches pixels. On P4 revision 3, the CSI 8-bit swap and a reversed input-packing declaration together keep chroma and pixel order correct, and the CSI color-management block (CSI_CM) emits the encoder's native YUV420. The EDID offers 1920x1080p30 (preferred), 1280x720p30, and 1280x720p60; there is no 1080p60 or scaling. The encoder sends two keyframes per second on Ethernet and one on Wi-Fi.

## Constraints

- The encoder takes a buffer only after CSI completes the frame, and the buffer stays locked until encoding ends. Encoding a partly written frame corrupts a reference picture and the P frames after it.
- Only the newest completed frame waits for the encoder. CSI may reclaim it if all buffers are busy, but never a DMA-owned buffer or the one being encoded.
- H.264 output goes straight into a transport slot. With no free slot, including with no viewer, the frame is not encoded.
- After a lost reference picture, the transport sends nothing until a fresh IDR. RTCP Picture Loss Indication (PLI) requests an IDR at most once per group of pictures.
- An access unit that overflows its slot raises the encoder's minimum QP by six steps; IDRs using at most a quarter of the slot lower it one step (`patches/esp_h264-1.4.1-overflow-qp.patch`).
- The per-frame path runs from IRAM (`linker.lf`) to hold 1080p30. Cold paths stay in flash to leave internal RAM for DTLS.
- The bridge's 27 MHz oscillator shares `VDD_O3` with the P4 MIPI PHY. Firmware enables LDO channel 3 at 2.5 V with the bridge in reset and waits for the oscillator before any I²C access. See [`../jetkvm_hdmi_bridge/README.md`](../jetkvm_hdmi_bridge/README.md).

## HDMI recovery

The capture task runs two recovery policies, each with tests in `tests/`.

- HPD (hot-plug detect) retry, `hdmi_hpd_retry.h`: some sources, such as a Mac behind a DP-to-HDMI converter, stay dark after a hot plug. After each HPD rise of the Mini, if the cable has +5 V but no signal, the task pulses HPD after 10 s, 30 s, then every 60 s, five times at most. The first signal disarms it, so a host that later sleeps is not woken.
- Bridge reset, `hdmi_health.h`: the TC358743 can get stuck with a wrong width or a few frames a second until it is reset. While video is expected, two bad 10 s windows in a row trigger a full bridge reset as at boot, at most three in a row. A failed reset restarts the Mini.

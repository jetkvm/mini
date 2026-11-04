# JetKVM screen component

This component drives the 240x240 ST7789P3 status display. It renders a direct port of the generated JetKVM LVGL UI, without the EEZ runtime, and shows a live HDMI preview when capture is locked. There is no touch input.

## Interface

- `jetkvm_screen_start()` and `jetkvm_screen_stop()` control the display task.
- `jetkvm_screen_update()` hands the latest UI state, including the USB status, to the screen task without blocking. Only the screen task calls LVGL.
- `jetkvm_screen_preview_submit_ouyy_evyy()`, `jetkvm_screen_preview_submit_uyvy()` and `jetkvm_screen_preview_stop()` drive the HDMI preview.
- `jetkvm_screen_get_stats()` reports display state, published in `/webrtc/stats` as `localDisplay`.
- `jetkvm_screen_set_wifi_setup_handler()` lets the local menu start and stop Wi-Fi setup.
- Kconfig `Component config > JetKVM screen > Display hardware rotation` selects 0, 90, 180 or 270 degrees. `JETKVM_SCREEN_BUTTONS` lets SW2 and SW3 navigate the UI; SW3 then no longer toggles the video mode.

## Hardware

The panel uses SPI2 at 20 MHz, four-wire SPI mode 0, with MOSI on GPIO3, SCLK on GPIO13 and backlight PWM on GPIO4. The connector has no MISO line, so `miso_io_num` is `-1`. The control pins differ by board:

| Board | DC | Reset | CS |
| --- | --- | --- | --- |
| Ethernet | GPIO20 | GPIO21 | GPIO22 |
| Wi-Fi | GPIO14 | GPIO15 | GPIO16 |

The panel runs big-endian RGB565 with color inversion on. Rotation uses the ST7789 MADCTL register, not software. LVGL stores pixels little-endian, so they are byte-swapped before each SPI DMA transfer.

## UI

The LVGL canvas and all thirteen pages are native 240x240 and map one-to-one onto the panel. In AUTO mode, network changes switch only among the boot, home and no-network pages and never leave the menu, status, about or regulatory pages.

The menu reads Set up Wi-Fi (Wi-Fi board only), Regulatory, Advanced, Status, About. Menu > Regulatory shows the FCC e-label: from the home screen, Menu, Down and a hold open it on the Wi-Fi board, and Menu and a hold on the Ethernet board. Back returns to the menu. `jetkvm_screen_regulatory.c` builds the page with the About page's styles, since the generated UI has no such page. The text is in `regulatory.c`, fixed per board variant at build time; no setting, RPC or web UI changes it. A new marking is one more entry there.

Without a network, the Wi-Fi board points the user to `jetkvm.com/wireless` and the Ethernet board shows `Connect Ethernet cable`. After a failed Wi-Fi join, two red lines show the reason and the network name. The texts come from `components/jetkvm_c5_wifi/wifi_join_failure.c`, shared with the web UI's `connect_error`. They stay until the device has an address again. `localDisplay.wifiFailureShown` and `wifiFailureShows` record the last text and how often one appeared.

## HDMI preview

While capture is locked, the display shows an aspect-preserving preview with an on-screen label of the input mode (`width x height @ refresh Hz`). Signal loss or a mode rebuild returns to the status UI.

The preview bypasses LVGL, and the CPU never reads the source image. The pixel-processing accelerator (PPA) converts the packed YUV420 capture buffer to RGB565 and scales it into one of two PSRAM DMA buffers while SPI sends the other. A preview task in `jetkvm_video` holds the capture buffer during the PPA read, so capture never waits for the display. Only the newest frame is kept.

Firmware boots in H.264-only mode. SW3 (active low; GPIO17 on the Wi-Fi board, GPIO23 on the Ethernet board) toggles between H.264-only and screen-only modes. The two are exclusive: a mode change rebuilds the capture pipeline so no buffer is owned by both consumers.

## Constraints

- The screen task is pinned to core 0 so it never adds jitter to the core-1 capture, H.264 and WebRTC path. Its LVGL wait is clamped to 10–100 ms so a 100 Hz tick cannot make it busy-loop.
- LVGL allocates only from a dedicated 256 KiB PSRAM heap carved out by `jetkvm_memory_policy`. There is no fallback to any other heap.
- LVGL's flush wait blocks on the SPI idle semaphore with no timeout, because LVGL must not reuse the single viewport DMA buffer before the transfer completes. The preview shares that semaphore; a transfer-owner tag keeps a preview completion from being reported to LVGL.
- The display starts after NVS and the factory-reset boot check, before Wi-Fi and TF-card initialization. The boot logo stays up until the status task starts.
- The backlight stays off until the first full LVGL frame has reached the panel, so uninitialized GRAM is never visible.

## Tests

`components/jetkvm_screen/tests/run_host_tests.sh` runs the host tests. `components/jetkvm_screen/tests/render_ui_screens.sh <dir> wifi|ethernet` renders all twelve generated pages and the Regulatory page to PPM (and PNG with ImageMagick) using the LVGL software renderer, with no hardware.

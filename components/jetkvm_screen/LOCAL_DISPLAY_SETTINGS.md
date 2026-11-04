# Local display settings

`local_display_settings.c` implements the upstream JetKVM backlight and rotation RPCs, so the upstream hardware settings page works unchanged. Settings persist in the `jk_display` NVS namespace.

## Interface

- `getBacklightSettings` and `setBacklightSettings` use `max_brightness` (0–255), `dim_after` and `off_after` (seconds, 0 means never), wrapped in a `params` object. Dimming halves the brightness. The default is 35 with both timers off.
- `getDisplayRotation` and `setDisplayRotation` carry `rotation` as the string `"0"`, `"90"`, `"180"` or `"270"`. Upstream's Normal (`"270"`) is the Kconfig mounting; each other value turns the picture a further quarter turn clockwise. The `display_quarter_turns` capability makes the UI offer all four turns.
- With `JETKVM_SCREEN_BUTTONS`, the `display_buttons` capability makes the UI say a button press, not a touch, wakes the display.

## Behavior

A setter succeeds only after both the hardware change and the NVS write succeed; otherwise it restores the previous hardware state and returns an RPC error.

The display wakes on start, on a settings change, on a network, HDMI or session state change, and on a button press. Timer expiry only changes the backlight. A press on a dark display only wakes it; a press on a dimmed display acts as usual.

The button pillar follows the buttons to whichever canvas edge they are on. Back is the button at the reader's top or left: SW3 at 0° and 90°, SW2 at 180° and 270°.

## Constraints

- Take the settings mutex before the panel mutex.
- Rotation drains in-flight SPI transfers, then sets the ST7789 swap, mirror and gap registers. Pixels are never rotated in software.
- The screen task runs the timers before it checks for HDMI preview, so they work during preview.

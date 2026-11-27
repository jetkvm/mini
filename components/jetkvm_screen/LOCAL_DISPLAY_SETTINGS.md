# Local display settings

`local_display_settings.c` implements the upstream JetKVM backlight and rotation RPCs, so the upstream hardware settings page works unchanged. Settings persist in the `jk_display` NVS namespace.

## Interface

- `getBacklightSettings` and `setBacklightSettings` use `max_brightness` (0–255), `dim_after` and `off_after` (seconds, 0 means never), wrapped in a `params` object. Dimming halves the brightness. The default is 35 with both timers off.
- `getDisplayRotation` and `setDisplayRotation` carry `rotation` as the string `"0"`, `"90"`, `"180"` or `"270"`. Upstream's Normal (`"270"`) is the Kconfig mounting; each other value turns the picture a further quarter turn clockwise. The `display_quarter_turns` capability makes the UI offer all four turns.
- With `JETKVM_SCREEN_BUTTONS`, the `display_buttons` capability makes the UI say a button press, not a touch, wakes the display.
- `getDisplaySchedule` and `setDisplaySchedule` carry two time windows of the day, `dim` and `off`, each with `enabled`, `start` and `end` (`"HH:MM"`, not equal; an end before the start is on the next day), and one `timezone` for both, wrapped in a `params` object for the setter. `getDisplayScheduleTimezones` lists the zone names that the setter takes. The `display_schedule` capability makes the UI show the windows. The default is no window, with dim from 20:00 to 23:00, off from 22:00 to 06:00 and no time zone (`""`). The setter takes `""` only when neither window is enabled, so the UI can tell a schedule that was never turned on from one that is in UTC.

## Behavior

A setter succeeds only after both the hardware change and the NVS write succeed; otherwise it restores the previous hardware state and returns an RPC error.

The display wakes on start, on a settings change, on a network, HDMI or session state change, and on a button press. Timer expiry only changes the backlight. A press on a dark display only wakes it; a press on a dimmed display acts as usual.

In the dim window the backlight is at most half the set brightness, and in the off window it is off. The darkest rule wins: the off window, then the dim window, then what the two idle timers give. In a window a state change does not wake the display. A button press or a changed backlight or rotation setting lights it until the first idle timer that is set would act (the dim timer, else the off timer), or for 30 s when both timers are off. The windows do nothing until the clock has the network time.

The windows are in the local time of the schedule's zone, with daylight time. `components/jetkvm_timezone` has the table from zone name to POSIX TZ rule, which `tools/timezones/generate.py` writes from the build machine's tzdata, and applies a rule to a UTC time without the process's `TZ`. A rule gives the zone's time from its last listed change on; the header of the table names the zones for which that is not yet so. The schedule has its own NVS key, `schedule`, so a firmware without it still reads the `settings` record. A record of a build before the two windows (one window with an action) is read as no window.

The button pillar follows the buttons to whichever canvas edge they are on. Back is the button at the reader's top or left: SW3 at 0° and 90°, SW2 at 180° and 270°.

## Constraints

- Take the settings mutex before the panel mutex.
- Rotation drains in-flight SPI transfers, then sets the ST7789 swap, mirror and gap registers. Pixels are never rotated in software.
- The screen task runs the timers before it checks for HDMI preview, so they work during preview.

# JetKVM ESP32-P4 USB HID

This component owns the target-facing USB device on the ESP32-P4 USB 2.0 high-speed (UTMI) controller. It presents JetKVM-compatible HID functions and the virtual-media mass-storage (MSC) drive, with optional audio and OS Services interfaces. The separate USB Serial/JTAG controller stays reserved for the debug console and flashing.

## Interface

- `jetkvm_usb_hid_set_descriptor_configuration()` selects IDs, strings and enabled functions for the next `jetkvm_usb_hid_init()`. It is rejected unless the driver is stopped.
- `jetkvm_usb_hid_keyboard_report()`, `jetkvm_usb_hid_keypress()`, `jetkvm_usb_hid_absolute_mouse_report()`, `jetkvm_usb_hid_relative_mouse_report()`, `jetkvm_usb_hid_wheel_report()` and `jetkvm_usb_hid_release_all()` send input.
- `jetkvm_usb_hid_remote_wakeup()` wakes a suspended host.
- `jetkvm_usb_hid_get_state()` and `jetkvm_usb_hid_get_configuration_snapshot()` report status.
- `jetkvm_usb_hid_cdrom_drive_takes_medium()` tells `jetkvm_usb_service_present_medium()` whether a mount needs re-enumeration.

## HID functions

The device exposes four HID functions in this order: boot keyboard (with an LED OUT endpoint), absolute mouse with wheel, boot relative mouse, and a vendor-defined remote-wake interface. The wake interface is always present and its report descriptor is byte-identical to JetKVM's Linux `wake_hid` gadget. The other three can each be disabled.

Interface and TinyUSB instance numbers are assigned densely in that order, and every report path resolves the saved function-to-instance map, so a disabled function never redirects reports into the next one. Wheel input uses the absolute-mouse report when that function is enabled, otherwise the relative-mouse report.

`jetkvm_usb_hid_keypress()` maps usages `0xE0`–`0xE7` to modifier bits and tracks all held keys separately from the six-key boot report. Seven or more held keys send six `ErrorRollOver` (`0x01`) usages.

## Constraints

- The build requires `CONFIG_TINYUSB_HID_COUNT=4`, the custom image-backed MSC backend, TinyUSB CDC off and the USB Serial/JTAG console on.
- Descriptors live in two static banks. Reconfiguration builds the inactive bank, then selects it, so TinyUSB never points at caller-owned or rewritten storage.
- USB strings longer than 31 printable ASCII characters are rejected, because ESP TinyUSB would silently truncate them.
- The TinyUSB task is pinned to CPU0. The LED callback runs on it and must not block or send reports.
- Each report has a 20 ms deadline. A failed state change is not committed, so the caller can retry without losing a key-up or button-up.
- `jetkvm_usb_hid_release_all()` sends only for interfaces that still have something pressed. Linux may not poll an interrupt-IN endpoint without an open consumer, so a redundant all-up report could stay busy indefinitely.
- `connected` means TinyUSB has seen a setup packet since bus reset; `mounted` means the host selected the configuration.
- Remote wake works only if the host enabled it before suspending. Concurrent wake calls do not queue. Success means the controller accepted the signal; a later `resume_count` increment confirms the host resumed.

## Virtual-media drive

The MSC interface has one logical unit that exposes only the mounted image, never the whole TF card. Local disk images whose size is a multiple of 512 bytes are writable; CD-ROM and HTTP images are read-only.

The host reads the drive type from `INQUIRY` at enumeration and never asks again. The type is therefore fixed at each driver start: the type of the mounted image, or CD-ROM if nothing is mounted. Unmounting leaves the drive present without a medium.

A CD-ROM image can replace the medium of an enumerated CD-ROM drive without re-enumeration, but only if the host polls the drive: it must have sent `TEST UNIT READY` or `GET EVENT STATUS NOTIFICATION` within the last 3 s and since the drive was last emptied. macOS stops polling a drive once it has held a medium, and hosts in boot firmware or suspend do not poll. Every other mount re-enumerates the device, which briefly removes all interfaces and ends any OS Services session.

The drive reports each medium change once with UNIT ATTENTION 28h. Each mount and unmount advances the medium generation in `jetkvm_virtual_media`. Until the host has seen the current generation, `READ(10)`, `WRITE(10)` and `READ CAPACITY` fail, so a command meant for a previous image never touches the new one.

Writable media advertise a write cache through the Caching mode page. The host then sends `SYNCHRONIZE CACHE` at fsync and eject, which succeeds only after the image syncs. MSC telemetry appears in `/webrtc/stats` under `usbHid` (`mscWrite`, `mediaWrite`, `mediaRead`).

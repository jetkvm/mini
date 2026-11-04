#!/usr/bin/env python3
"""Watches one USB port on macOS as UsbPortWatch in
frontend/jetkvm-ui/e2e/mini/os-services.ts watches sysfs on Linux.

  python3 os_services_usb_port_watch_macos.py <port> <triple> <seconds> <log> <marks>

<port> is the device's locationID in hex. Every 50 ms, on a change, prints
"<ms> <vid:pid> <USB address> <owner>" or "<ms> - - -" for no device, dated
at the previous poll. A device without a USB address yet counts as no device,
as on Linux. <owner> is the state of the interface matching <triple>: none,
free, usbfs (the sender) or other (a driver). Prints "tick" after 20 quiet
polls, so it ends when its reader is gone; ends after <seconds>.

When the sender's <log> grows, appends "<ms> <size>" to <marks> so that
TargetHost.senderLog can date its lines.
"""

import os
import plistlib
import subprocess
import sys
import time


def now_ms():
    return int(time.time() * 1000)


def registry_devices(node):
    """Every IOUSBHostDevice in an ioreg -a tree."""
    if node.get("IOObjectClass") == "IOUSBHostDevice":
        yield node
    for child in node.get("IORegistryEntryChildren", []):
        yield from registry_devices(child)


def interface_owner(device, triple):
    owner = "none"
    for child in device.get("IORegistryEntryChildren", []):
        fields = (child.get(key, -1) for key in
                  ("bInterfaceClass", "bInterfaceSubClass", "bInterfaceProtocol"))
        if "".join(f"{field:02x}" for field in fields) != triple:
            continue
        holder = child.get("UsbExclusiveOwner", "")
        owner = "usbfs" if holder.startswith("pid ") else "other" if holder else "free"
    return owner


def listing_state(listing, location, triple):
    """The port's state in an `ioreg -a -r -c IOUSBHostDevice -l` listing."""
    # ioreg prints nothing when no USB device is attached.
    for root in plistlib.loads(listing) if listing else []:
        for device in registry_devices(root):
            if device.get("locationID") != location or not device.get("USB Address"):
                continue
            return (f"{device.get('idVendor', 0):04x}:{device.get('idProduct', 0):04x} "
                    f"{device['USB Address']} {interface_owner(device, triple)}")
    return "- - -"


def port_state(location, triple):
    listing = subprocess.run(["ioreg", "-a", "-r", "-c", "IOUSBHostDevice", "-l"],
                             capture_output=True, check=True).stdout
    return listing_state(listing, location, triple)


def log_size(log):
    try:
        return os.stat(log).st_size
    except OSError:
        return 0


def main():
    location, triple = int(sys.argv[1], 16), sys.argv[2]
    deadline = time.monotonic() + float(sys.argv[3])
    log, marks = sys.argv[4], sys.argv[5]
    previous, polls = None, 0
    sized_at, size = now_ms(), log_size(log)
    while time.monotonic() < deadline:
        try:
            state = port_state(location, triple)
        except Exception:  # A failed or truncated ioreg dump keeps the last state.
            state = previous
        at, current = now_ms(), log_size(log)
        if current != size:
            with open(marks, "a") as out:
                out.write(f"{sized_at} {size}\n")
        polls += 1
        if state != previous:
            print(f"{sized_at} {state}", flush=True)
            previous, polls = state, 0
        elif polls >= 20:
            print("tick", flush=True)
            polls = 0
        sized_at, size = at, current
        time.sleep(0.05)


if __name__ == "__main__":
    main()

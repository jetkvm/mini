"""The macOS USB port watch's reading of an I/O Registry listing.

UsbPortWatch.reenumeration (frontend/jetkvm-ui/e2e/mini/os-services.ts)
counts a departure when the port's device goes away or its USB address
changes, so the watch must not report the address 0 that macOS shows for a
few milliseconds while it enumerates the device.
"""

from __future__ import annotations

import plistlib
import unittest

from tools.e2e import os_services_usb_port_watch_macos as watch

LOCATION = 0x02400000
OS_SERVICES_TRIPLE = "ff4a01"


def device(address: int | None, owner: str | None = None, location: int = LOCATION) -> dict:
    node = {
        "IOObjectClass": "IOUSBHostDevice",
        "locationID": location,
        "idVendor": 0x399B,
        "idProduct": 0x0001,
        "IORegistryEntryChildren": [
            {
                "bInterfaceClass": 0xFF,
                "bInterfaceSubClass": 0x4A,
                "bInterfaceProtocol": 0x01,
                **({"UsbExclusiveOwner": owner} if owner is not None else {}),
            }
        ],
    }
    if address is not None:
        node["USB Address"] = address
    return node


def listing(*devices: dict) -> bytes:
    # ioreg -a -r prints an array with one root per matching device.
    return plistlib.dumps(list(devices)) if devices else b""


def state(raw: bytes) -> str:
    return watch.listing_state(raw, LOCATION, OS_SERVICES_TRIPLE)


class ListingStateTest(unittest.TestCase):
    def test_a_device_with_its_address_is_reported(self) -> None:
        self.assertEqual(state(listing(device(1, "pid 812, ipkvm-os-services"))), "399b:0001 1 usbfs")
        self.assertEqual(state(listing(device(7, ""))), "399b:0001 7 free")

    def test_a_device_without_an_address_yet_is_no_device(self) -> None:
        self.assertEqual(state(listing(device(0))), "- - -")
        self.assertEqual(state(listing(device(None))), "- - -")

    def test_an_empty_listing_or_another_port_is_no_device(self) -> None:
        self.assertEqual(state(b""), "- - -")
        self.assertEqual(state(listing(device(3, location=0x01100000))), "- - -")

    def test_a_reboot_is_one_departure(self) -> None:
        # The device leaves, comes back without an address, then gets
        # address 1 and the sender claims it again.
        states = [
            state(listing(device(1, ""))),
            state(listing()),
            state(listing(device(0))),
            state(listing(device(1, ""))),
            state(listing(device(1, "pid 812, ipkvm-os-services"))),
        ]
        present = [s != "- - -" for s in states]
        self.assertEqual(present, [True, False, False, True, True])
        addresses = {s.split()[1] for s in states if s != "- - -"}
        self.assertEqual(addresses, {"1"})


if __name__ == "__main__":
    unittest.main()

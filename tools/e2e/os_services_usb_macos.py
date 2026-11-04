#!/usr/bin/env python3
"""Lists a USB device on macOS in the lines the e2e helper reads from sysfs.

  python3 os_services_usb_macos.py <directory of os_services_usb_probe.py> <vid:pid>

For each device with that ID it prints one "device" line, then one
"interface" line per interface with one "endpoint" line per endpoint, the
fields tab-separated as TargetHost.usbDevices in
frontend/jetkvm-ui/e2e/mini/os-services.ts parses them. The descriptors come
from libusb through the probe's bindings; the strings and the owner of each
interface from the I/O Registry. An owner is named by its role on Linux:
usbfs for a process that opened the interface (the sender), usbhid for the
HID driver.
"""

import ctypes
import plistlib
import subprocess
import sys

sys.path.insert(0, sys.argv[1])
import os_services_usb_probe as probe  # noqa: E402

SPEEDS = {1: "1.5", 2: "12", 3: "480", 4: "5000", 5: "10000"}
TYPES = ("Control", "Isoc", "Bulk", "Interrupt")


def registry_devices(node):
    """Every IOUSBHostDevice in an ioreg -a tree."""
    if node.get("IOObjectClass") == "IOUSBHostDevice":
        yield node
    for child in node.get("IORegistryEntryChildren", []):
        yield from registry_devices(child)


def owner_role(owner):
    if not owner:
        return "-"
    if owner.startswith("pid "):
        return "usbfs"
    return "usbhid" if "HID" in owner else owner


def registry_entry(bus, address):
    tree = plistlib.loads(
        subprocess.run(["ioreg", "-a", "-r", "-c", "IOUSBHostDevice", "-l"],
                       capture_output=True, check=True).stdout
    )
    for root in tree:
        for device in registry_devices(root):
            if device.get("locationID", 0) >> 24 == bus and device.get("USB Address") == address:
                return device
    return {}


def owners(entry):
    return {
        child.get("bInterfaceNumber"): child.get("UsbExclusiveOwner")
        for child in entry.get("IORegistryEntryChildren", [])
        if child.get("IOObjectClass") == "IOUSBHostInterface"
    }


def print_device(lib, device, descriptor):
    bus = lib.libusb_get_bus_number(device)
    address = lib.libusb_get_device_address(device)
    entry = registry_entry(bus, address)
    name = f"{entry.get('locationID', 0):08x}"
    usb = descriptor.bcdUSB
    fields = [
        name, bus, address, SPEEDS.get(lib.libusb_get_device_speed(device), "?"),
        f"{usb >> 8:x}.{usb & 0xff:02x}", f"{descriptor.bcdDevice:04x}",
        entry.get("USB Vendor Name", ""), entry.get("USB Product Name", ""),
        entry.get("USB Serial Number", ""),
    ]
    print("\t".join(["device", *map(str, fields)]))
    config = ctypes.POINTER(probe._ConfigDescriptor)()
    if lib.libusb_get_active_config_descriptor(device, ctypes.byref(config)) != 0:
        return
    held = owners(entry)
    try:
        for i in range(config.contents.bNumInterfaces):
            alt = config.contents.interface[i].altsetting[0]
            triple = f"{alt.bInterfaceClass:02x}{alt.bInterfaceSubClass:02x}{alt.bInterfaceProtocol:02x}"
            print("\t".join(["interface", f"{name}:1.{alt.bInterfaceNumber}",
                             f"{alt.bInterfaceNumber:02x}", triple, f"{alt.bNumEndpoints:02x}",
                             owner_role(held.get(alt.bInterfaceNumber))]))
            for e in range(alt.bNumEndpoints):
                endpoint = alt.endpoint[e]
                address = endpoint.bEndpointAddress
                print("\t".join(["endpoint", f"{address:02x}", TYPES[endpoint.bmAttributes & 3],
                                 "in" if address & 0x80 else "out",
                                 f"{endpoint.wMaxPacketSize:04x}"]))
    finally:
        lib.libusb_free_config_descriptor(config)


def main():
    vendor, product = (int(part, 16) for part in sys.argv[2].split(":"))
    lib = probe._load_libusb()
    lib.libusb_get_device_speed.argtypes = [ctypes.c_void_p]
    context = ctypes.c_void_p()
    if lib.libusb_init(ctypes.byref(context)) != 0:
        sys.exit("libusb_init failed")
    devices = ctypes.POINTER(ctypes.c_void_p)()
    try:
        count = lib.libusb_get_device_list(context, ctypes.byref(devices))
        for index in range(max(count, 0)):
            descriptor = probe._DeviceDescriptor()
            if lib.libusb_get_device_descriptor(devices[index], ctypes.byref(descriptor)) != 0:
                continue
            if (descriptor.idVendor, descriptor.idProduct) == (vendor, product):
                print_device(lib, devices[index], descriptor)
        if count >= 0:
            lib.libusb_free_device_list(devices, 1)
    finally:
        lib.libusb_exit(context)


if __name__ == "__main__":
    main()

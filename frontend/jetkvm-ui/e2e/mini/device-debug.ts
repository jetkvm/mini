import { expect, type Page } from "@playwright/test";
import { callJsonRpc } from "../helpers";
import { remoteHostExec } from "../remote-agent/shared";

// Evidence about the Mini that is not in its RPC state: its USB Serial/JTAG
// debug interface (303a:1001) on the remote host.

// The debug interface's serial is the P4's MAC address; the device ID is that
// address without colons. (A Wi-Fi Mini's network MAC is the radio's.)
export async function debugUsbSerial(page: Page): Promise<string> {
  const deviceId = (await callJsonRpc(page, "getDeviceID")) as string;
  expect(deviceId).toMatch(/^[0-9a-f]{12}$/i);
  return deviceId.match(/../g)!.join(":");
}

export interface DebugUsb {
  path: string;
  busnum: string;
  devnum: string;
}

// macOS has no sysfs. The same device in the I/O Registry's USB plane: its
// locationID stands for the sysfs path, the bus is the locationID's top byte
// and the device number is its USB address. Prints the line the sysfs loop
// prints; argv[1] is the serial in upper case.
const MAC_DEBUG_USB = `
import plistlib, subprocess, sys
def devices(node):
    if node.get("IOObjectClass") == "IOUSBHostDevice":
        yield node
    for child in node.get("IORegistryEntryChildren", []):
        yield from devices(child)
tree = plistlib.loads(subprocess.run(["ioreg", "-a", "-l", "-p", "IOUSB"], capture_output=True, check=True).stdout)
for d in (d for root in (tree if isinstance(tree, list) else [tree]) for d in devices(root)):
    if (d.get("idVendor"), d.get("idProduct")) == (0x303A, 0x1001) and str(d.get("USB Serial Number", "")).upper() == sys.argv[1]:
        loc = d.get("locationID", 0)
        print(f"IOUSB:{loc:08x} {loc >> 24} {d.get('USB Address', 0)}")
`;

// The 303a:1001 device on the remote host whose serial is `serial`, or null.
export function debugUsb(serial: string): DebugUsb | null {
  const wanted = serial.toUpperCase();
  const lines = remoteHostExec(
    `if [ "$(uname -s)" = Darwin ]; then ` +
      `python3 -c '${MAC_DEBUG_USB.replace(/'/g, "'\\''")}' "${wanted}"; ` +
      `else for d in /sys/bus/usb/devices/*; do ` +
      `[ -f "$d/idVendor" ] && [ -f "$d/serial" ] || continue; ` +
      `[ "$(cat "$d/idVendor"):$(cat "$d/idProduct")" = 303a:1001 ] || continue; ` +
      `[ "$(tr a-f A-F < "$d/serial")" = "${wanted}" ] || continue; ` +
      `echo "$d $(cat "$d/busnum") $(cat "$d/devnum")"; done; fi`,
  )
    .trim()
    .split("\n")
    .filter(Boolean);
  expect(lines.length, `303a:1001 devices with serial ${serial}`).toBeLessThanOrEqual(1);
  if (lines.length === 0) return null;
  const [path, busnum, devnum] = lines[0].split(" ");
  return { path, busnum, devnum };
}

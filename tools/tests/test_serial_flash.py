"""tools/e2e/serial_flash.sh against a host and a Mini that are scripts.

ssh runs the host's part on this machine in a test home directory, with
uname giving the host's OS. Each OS's list of serial ports is a test
directory (Linux's /dev/serial/by-id/), a plist (macOS's I/O Registry, read
by the script's own parser) or the lines that its PowerShell prints (Windows).
Each host has the debugger of another Mini as well. esptool and curl write
down their arguments.
"""
import json
import os
from pathlib import Path
import plistlib
import subprocess
import tempfile
import unittest

from tools.tests.test_mini_image import synthetic_image

SCRIPT = Path(__file__).parents[1] / "e2e/serial_flash.sh"
MAC = "80:F1:B2:D8:82:63"
OTHER_MAC = "80:F1:B2:D6:31:D9"
# flasher_args.json as ESP-IDF writes it, with offsets that are not the
# Mini's, so that the test shows where the script takes them from.
FLASHER_ARGS = {
    "write_flash_args": ["--flash-mode", "dio", "--flash-size", "16MB", "--flash-freq", "80m"],
    "flash_files": {
        "0x3000": "bootloader/bootloader.bin",
        "0x9000": "partition_table/partition-table.bin",
        "0xd10000": "ota_data_initial.bin",
        "0x20000": "jetkvm-mini.bin",
    },
    "extra_esptool_args": {"after": "hard-reset", "before": "default-reset", "stub": True, "chip": "esp32p4"},
}
FILES = ["0x3000", "bootloader.bin", "0x9000", "partition-table.bin",
         "0xd10000", "ota_data_initial.bin", "0x20000", "jetkvm-mini.bin"]


def usb_device(vendor, serial, port):
    """An IOUSBHostDevice as ioreg -a shows it, with its serial port two levels down."""
    return {"idVendor": vendor, "USB Serial Number": serial, "IORegistryEntryChildren": [
        {"IORegistryEntryChildren": [{"IOCalloutDevice": port}]}]}


class SerialFlashTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(subprocess.run, ["rm", "-rf", str(self.tmp)])
        self.build = self.tmp / "build"
        self.build.mkdir()
        (self.build / "flasher_args.json").write_text(json.dumps(FLASHER_ARGS))
        (self.build / "jetkvm-mini.bin").write_bytes(synthetic_image(b"0.5.13-gabcde+codelog", b"jetkvm-mini-wireless"))
        for name in ("bootloader.bin", "partition-table.bin", "ota_data_initial.bin"):
            (self.build / name).write_bytes(name.encode())
        by_id = self.tmp / "by-id"
        by_id.mkdir()
        for mac in (MAC, OTHER_MAC):
            (by_id / f"usb-Espressif_USB_JTAG_serial_debug_unit_{mac}-if00").touch()
        (self.tmp / "ioreg.plist").write_bytes(plistlib.dumps([
            usb_device(0x1050, "0001", "/dev/cu.usbmodem1101"),
            usb_device(0x303A, OTHER_MAC, "/dev/cu.usbmodem2201"),
            usb_device(0x303A, MAC, "/dev/cu.usbmodem2401")]))
        self.home = self.tmp / "home"
        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        self.log = self.tmp / "log"
        # esptool in the venv of Linux and of Windows, and Espressif's build of it on macOS.
        esptool = 'echo "esptool $(basename "$(dirname "$0")") $*" >> "$LOG"; ls > "$LOG.files"'
        tools = {
            self.home / "esptool-venv/bin/esptool": esptool,
            self.home / "esptool-venv/Scripts/esptool": esptool,
            self.home / "esptool-5.4.0/esptool": esptool,
            bin_dir / "ssh": f'''echo "ssh $1" >> "$LOG"; shift; cd "{self.home}"
case "$1" in
    mktemp|rm) "$@" ;;
    *) sed "s#/dev/serial/by-id#{by_id}#g" | HOME="{self.home}" sh -c "$1" ;;
esac''',
            # scp -q <files> <host>:<directory in the home>/
            bin_dir / "scp": f'shift; eval "dest=\\${{$#}}"; '
                             f'while [ $# -gt 1 ]; do cp "$1" "{self.home}/${{dest#*:}}"; shift; done',
            bin_dir / "uname": 'echo "$HOST_OS"',
            bin_dir / "ioreg": f'cat "{self.tmp}/ioreg.plist"',
            bin_dir / "powershell": f"printf '{OTHER_MAC} COM3\\r\\n{MAC} COM5\\r\\n'",
            # The stats as the Mini answers them, then the status that -w asks for.
            bin_dir / "curl": 'echo "curl $*" >> "$LOG"; case "$*" in '
                              '*/webrtc/stats*) [ "$STATS_STATUS" = 200 ] && printf \'{"firmware":{"version":"%s"}}\' "$MINI_VERSION"; '
                              'printf "\\n%s" "$STATS_STATUS" ;; esac',
        }
        for path, body in tools.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"#!/bin/sh\n{body}\n")
            path.chmod(0o755)
        self.env = {**os.environ, "PATH": f"{bin_dir}:{os.environ['PATH']}", "LOG": str(self.log)}

    def flash(self, bench, mini_version="0.5.13-gabcde+codelog", stats_status="200", host_os="Linux"):
        path = self.tmp / "bench.env"
        path.write_text(bench)
        self.log.unlink(missing_ok=True)
        result = subprocess.run(["bash", str(SCRIPT), str(path), str(self.build)], capture_output=True, text=True,
                                env={**self.env, "MINI_VERSION": mini_version, "STATS_STATUS": stats_status,
                                     "HOST_OS": host_os}, timeout=30)
        log = self.log.read_text().splitlines() if self.log.exists() else []
        return result, log

    def bench(self, host="JETKVM_REMOTE_HOST=user@192.168.1.135",
              comment=f"# The Wi-Fi Mini's debugger (USB JTAG/serial, {MAC}) is on the PC.\n"):
        return f"{comment}JETKVM_URL=http://192.168.1.113\n{host}\nJETKVM_SKU=jetkvm-mini-wireless\n"

    def esptool(self, log, directory="bin"):
        """The arguments of the one esptool run, which must be the one in directory."""
        [line] = [line for line in log if line.startswith("esptool ")]
        self.assertEqual(line.split()[1], directory)
        return line.split()[2:]

    def test_on_linux_writes_the_four_files_at_the_build_offsets_through_the_debugger_with_the_benchs_mac(self):
        result, log = self.flash(self.bench())
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "0.5.13-gabcde+codelog\n")
        port = f"{self.tmp}/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_{MAC}-if00"
        self.assertEqual(self.esptool(log), [
            "--port", port, "--chip", "esp32p4", "--before", "default-reset", "--after", "hard-reset",
            "write-flash", "--flash-mode", "dio", "--flash-size", "16MB", "--flash-freq", "80m", *FILES])
        # esptool ran where the four files were copied, and the copy is gone afterwards.
        self.assertEqual(Path(f"{self.log}.files").read_text().split(),
                         ["bootloader.bin", "jetkvm-mini.bin", "ota_data_initial.bin", "partition-table.bin"])
        self.assertEqual(list(self.home.glob("serial-flash.*")), [])
        self.assertTrue(any("http://192.168.1.113/device/status" in line for line in log))

    def test_on_macos_espressifs_esptool_disconnects_the_roms_usb_device_then_writes_and_resets_by_the_watchdog(self):
        result, log = self.flash(self.bench(), host_os="Darwin")
        self.assertEqual(result.returncode, 0, result.stderr)
        port = "/dev/cu.usbmodem2401"
        self.assertEqual([line.split()[1:] for line in log if line.startswith("esptool ")], [
            ["esptool-5.4.0", "--port", port, "--chip", "esp32p4", "--before", "default-reset",
             "--after", "no-reset", "--no-stub", "write-mem", "0x50000804", "0x2", "0x2"],
            ["esptool-5.4.0", "--port", port, "--chip", "esp32p4", "--before", "no-reset",
             "--after", "watchdog-reset", "write-flash", "--flash-mode", "dio", "--flash-size", "16MB", "--flash-freq", "80m", *FILES]])

    def test_on_windows_the_combined_load_host_writes_through_the_com_port_of_the_mac(self):
        result, log = self.flash(self.bench(host="JETKVM_COMBINED_LOAD_HOST=runner@win-pc"),
                                 host_os="MINGW64_NT-10.0-26300")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ssh runner@win-pc", log)
        self.assertEqual(self.esptool(log, "Scripts")[:2], ["--port", "COM5"])

    def test_a_bench_without_a_host_has_no_serial_flash(self):
        result, log = self.flash(self.bench(host=""))
        self.assertEqual(result.returncode, 2)
        self.assertIn("no serial flash", result.stderr)
        self.assertEqual(log, [])

    def test_without_a_mac_the_host_must_have_one_espressif_serial_port(self):
        for host_os in ("Linux", "Darwin", "MINGW64_NT-10.0-26300"):
            result, log = self.flash(self.bench(comment=""), host_os=host_os)
            self.assertEqual(result.returncode, 1, host_os)
            self.assertIn("not one Espressif serial port", result.stderr)
            self.assertFalse(any(line.startswith("esptool ") for line in log))

    def test_an_image_of_the_other_board_is_refused_before_the_host_is_reached(self):
        (self.build / "jetkvm-mini.bin").write_bytes(synthetic_image(b"0.5.13-gabcde", b"jetkvm-mini-ethernet"))
        result, log = self.flash(self.bench())
        self.assertEqual(result.returncode, 2)
        self.assertIn("built for jetkvm-mini-ethernet", result.stderr)
        self.assertEqual(log, [])

    def test_a_mini_that_starts_another_version_fails_the_run(self):
        result, _ = self.flash(self.bench(), mini_version="0.5.12")
        self.assertEqual(result.returncode, 1)
        self.assertIn("the Mini runs 0.5.12, the image is 0.5.13-gabcde+codelog", result.stderr)

    def test_a_mini_with_a_password_fails_the_run_with_the_reason(self):
        result, log = self.flash(self.bench(), stats_status="401")
        self.assertEqual(result.returncode, 1)
        self.assertIn("the Mini has a password", result.stderr)
        self.assertTrue(any(line.startswith("esptool ") for line in log))


if __name__ == "__main__":
    unittest.main()

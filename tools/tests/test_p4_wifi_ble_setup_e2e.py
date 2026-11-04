"""The BLE setup e2e reads serial lines that the Wi-Fi firmware writes.

These tests take the log format strings from jetkvm_c5_wifi.c, render them
the way ESP-IDF prints them on the console, and check that the e2e patterns
extract the right values. A reworded log line fails here, not on the bench.
"""

from __future__ import annotations

from pathlib import Path
import re
import tempfile
import unittest

from tools.e2e import p4_wifi_ble_setup_e2e as e2e

REPO = Path(__file__).resolve().parents[2]
FIRMWARE = (REPO / "components/jetkvm_c5_wifi/jetkvm_c5_wifi.c").read_text()

LOG_CALL = re.compile(r'ESP_LOG[EWIDV]\(\s*TAG,\s*((?:"(?:[^"\\]|\\.)*"\s*|IPSTR\s*)+)')


def firmware_formats(prefix: str) -> list[str]:
    """The format strings of the firmware log calls that start with ``prefix``."""
    formats = []
    for call in LOG_CALL.finditer(FIRMWARE):
        parts = re.findall(r'"((?:[^"\\]|\\.)*)"|(IPSTR)', call.group(1))
        text = "".join(literal or "%d.%d.%d.%d" for literal, _ in parts)
        if text.startswith(prefix):
            formats.append(re.sub(r"%l?[ud]", "%d", text))
    if not formats:
        raise AssertionError(f"no firmware log call starts with {prefix!r}")
    return formats


def firmware_format(prefix: str) -> str:
    (text,) = firmware_formats(prefix)
    return text


def console_line(message: str) -> str:
    """One line as SerialCapture stores it: timestamp, then the colored log."""
    return f"2026-09-23T10:00:00.000Z \x1b[0;32mI (123456) c5_wifi: {message}\x1b[0m"


def read(pattern: re.Pattern[str], line: str) -> re.Match[str] | None:
    return pattern.search(e2e.strip_ansi(line))


class FirmwareLogLines(unittest.TestCase):
    def test_ble_name(self) -> None:
        line = console_line(firmware_format("Wi-Fi setup: connect to BLE device") % "JetKVM_E83AB8")
        self.assertEqual(read(e2e.BLE_NAME, line).group(1), "JetKVM_E83AB8")

    def test_setup_code_keeps_leading_zeros(self) -> None:
        line = console_line(firmware_format("Wi-Fi setup code:") % "004217")
        self.assertEqual(read(e2e.SETUP_CODE, line).group(1), "004217")

    def test_wrong_code_count(self) -> None:
        line = console_line(firmware_format("Wrong Wi-Fi setup code") % (2, 3))
        self.assertEqual(read(e2e.WRONG_CODE, line).groups(), ("2", "3"))

    def test_connected_address_and_gateway(self) -> None:
        # With a DNS server, and the warning variant without one.
        addresses = (10, 42, 0, 17, 10, 42, 0, 1)
        templates = firmware_formats("Wi-Fi connected:")
        self.assertEqual(len(templates), 2)
        for template in templates:
            extra = (10, 42, 0, 1) if template.count("%d") == 12 else ("ESP_FAIL",)
            with self.subTest(template=template):
                line = console_line(template % (addresses + extra))
                self.assertEqual(read(e2e.CONNECTED, line).groups(), ("10.42.0.17", "10.42.0.1"))

    def test_join_failure_reason(self) -> None:
        template = firmware_format("Provisioning connection failed:")
        self.assertIsNotNone(read(e2e.JOIN_AUTH_FAILED, console_line(template % "authentication failed")))
        self.assertIsNone(read(e2e.JOIN_AUTH_FAILED, console_line(template % "access point not found")))

    def test_joining_names_the_network(self) -> None:
        # The firmware prints the name with %.*s; render it as %s.
        template = firmware_format("Joining Wi-Fi").replace("%.*s", "%s")
        self.assertEqual(read(e2e.JOINING, console_line(template % "mini-e2e-aaaaaa")).group(1),
                         "mini-e2e-aaaaaa")

    def test_failed_join_line(self) -> None:
        template = firmware_format("Wi-Fi %.*s failed:").replace("%.*s", "%s")
        line = console_line(template % ("mini-e2e-aaaaaa", "authentication"))
        self.assertEqual(read(e2e.JOIN_FAILED_AUTH, line).group(1), "mini-e2e-aaaaaa")
        self.assertIsNone(read(e2e.JOIN_FAILED_AUTH,
                               console_line(template % ("mini-e2e-aaaaaa", "not found"))))

    def test_skipped_network_line(self) -> None:
        template = firmware_format("Wi-Fi %.*s: skipped").replace("%.*s", "%s")
        line = console_line(template % "mini-e2e-aaaaaa")
        self.assertEqual(read(e2e.SKIPPED_WRONG_PASSWORD, line).group(1), "mini-e2e-aaaaaa")

    def test_setup_start_and_end_lines(self) -> None:
        for pattern, prefix in ((e2e.PROVISIONED, "Provisioned Wi-Fi network saved"),
                                (e2e.SETUP_STARTED, "BLE Wi-Fi provisioning started"),
                                (e2e.SETUP_STOPPED, "BLE Wi-Fi provisioning stopped")):
            with self.subTest(prefix=prefix):
                self.assertIsNotNone(read(pattern, console_line(firmware_format(prefix))))


class DisplayStepLines(unittest.TestCase):
    """The display step names come from jetkvm_board_network.c."""

    NETWORK = (REPO / "components/jetkvm_board_network/jetkvm_board_network.c").read_text()

    def test_step_line_and_names(self) -> None:
        self.assertIn('"Wi-Fi setup screen: %s"', self.NETWORK)
        names = self.NETWORK[self.NETWORK.index("static const char *wifi_setup_name("):]
        names = names[:names.index("\n}\n")]
        for step in ("advertising", "code", "code accepted", "joining"):
            with self.subTest(step=step):
                self.assertIn(f'return "{step}";', names)
                line = console_line(f"Wi-Fi setup screen: {step}")
                self.assertEqual(read(e2e.SETUP_SCREEN, line).group(1), step)


class DisplaySteps(unittest.TestCase):
    def run_with(self, steps: list[str]) -> "e2e.Run":
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "serial-000.log"
        path.write_text("".join(console_line(f"Wi-Fi setup screen: {step}") + "\n"
                                for step in steps), encoding="utf-8")
        run = object.__new__(e2e.Run)
        run.serial = e2e.SerialLog("/dev/null", Path(directory.name), history=True)
        return run

    def test_order_with_other_steps_between(self) -> None:
        run = self.run_with(["code", "code accepted", "joining", "code accepted", "advertising"])
        run.expect_screens(0, ["code", "code accepted", "joining", "code accepted", "advertising"])
        run.expect_screens(0, ["code", "advertising"])

    def test_missing_or_reordered_step_fails(self) -> None:
        run = self.run_with(["code", "joining", "code accepted"])
        with self.assertRaisesRegex(RuntimeError, "expected"):
            run.expect_screens(0, ["code", "code accepted", "joining"])

    def test_code_after_acceptance_fails(self) -> None:
        run = self.run_with(["code", "code accepted", "code", "joining"])
        with self.assertRaisesRegex(RuntimeError, "came back"):
            run.expect_screens(0, ["code", "code accepted", "joining"])


class Helpers(unittest.TestCase):
    def test_wrong_code_wraps_and_keeps_six_digits(self) -> None:
        self.assertEqual(e2e.wrong_code("999999"), "000000")
        self.assertEqual(e2e.wrong_code("000041"), "000042")

    def test_restore_reads_the_serial_log_of_the_failed_run(self) -> None:
        # --restore-only reuses the failed run's output directory; the
        # segments there come first and line indexes continue across them.
        with tempfile.TemporaryDirectory() as directory:
            for index, lines in enumerate((["a", console_line("Wi-Fi setup code: 111111")],
                                           [console_line("Wi-Fi setup code: 222222")])):
                path = Path(directory) / f"serial-{index:03d}.log"
                path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            log = e2e.SerialLog("/dev/null", Path(directory), history=True)
            hits = log.find(e2e.SETUP_CODE, since=2)
            self.assertEqual([(index, match.group(1)) for index, match in hits], [(2, "222222")])
            self.assertEqual(log.last(e2e.SETUP_CODE)[1].group(1), "222222")
            # The failed run's lines do not count as this run's evidence.
            self.assertEqual(log.lines(own=True), [])
            # A full run in the same directory starts without them.
            self.assertEqual(e2e.SerialLog("/dev/null", Path(directory)).lines(), [])

class FakeAccessPoint:
    def __init__(self, ssid: str | None, device: str | None) -> None:
        self.ssid = ssid
        self.device = device
        self.probes = 0

    def address(self) -> str:
        return "10.42.0.1"

    def answers(self, host: str) -> bool:
        return host == self.device

    def find_device(self) -> str | None:
        self.probes += 1
        return self.device


class RestoreLocate(unittest.TestCase):
    """Where the restore looks for the Mini, from the log and the access point."""

    SETUP = [console_line("Wi-Fi setup: connect to BLE device JetKVM_E83AB8"),
             console_line("Wi-Fi setup code: 123456")]
    JOINED_AP = [console_line("Wi-Fi connected: 10.42.0.17 gateway 10.42.0.1 DNS 10.42.0.1")]

    def locate(self, lines: list[str], ap: FakeAccessPoint) -> tuple[str, str | None]:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "serial-000.log"
            path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            run = e2e.Run.__new__(e2e.Run)
            run.serial = e2e.SerialLog("/dev/null", Path(directory), history=True)
            run.serial.keep_open = lambda: None
            run.ap = ap
            run.ble_name = None
            return run.locate()

    def test_a_mini_on_the_access_point_wins_over_a_stale_setup_code(self) -> None:
        ap = FakeAccessPoint("mini-e2e-aaaaaa", "10.42.0.23")
        self.assertEqual(self.locate(self.SETUP, ap), ("ap", "http://10.42.0.23/"))

    def test_setup_code_without_a_reachable_mini(self) -> None:
        for ap in (FakeAccessPoint(None, None), FakeAccessPoint("mini-e2e-aaaaaa", None)):
            with self.subTest(ap=ap.ssid):
                self.assertEqual(self.locate(self.SETUP, ap), ("ble", "123456"))

    def test_a_logged_join_that_answers_needs_no_subnet_search(self) -> None:
        ap = FakeAccessPoint("mini-e2e-aaaaaa", "10.42.0.17")
        self.assertEqual(self.locate(self.SETUP + self.JOINED_AP, ap),
                         ("ap", "http://10.42.0.17/"))
        self.assertEqual(ap.probes, 0)

    def test_a_stale_logged_address_falls_back_to_the_subnet_search(self) -> None:
        # The access point came back and DHCP gave the Mini another lease.
        ap = FakeAccessPoint("mini-e2e-aaaaaa", "10.42.0.99")
        self.assertEqual(self.locate(self.SETUP + self.JOINED_AP, ap),
                         ("ap", "http://10.42.0.99/"))
        self.assertEqual(ap.probes, 1)

    def test_no_access_point_and_no_setup(self) -> None:
        ap = FakeAccessPoint(None, None)
        self.assertEqual(self.locate(self.SETUP + self.JOINED_AP, ap), ("elsewhere", None))



class AccessPointRecord(unittest.TestCase):
    def test_access_point_a_stays_recorded_after_b_starts(self) -> None:
        # The Mini keeps A saved until B is set up; losing A's password
        # would leave --restore-only no network to bring back.
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "access-point.json"
            ap = e2e.AccessPoint("wlan9", "bg", None, False, path)
            ap.record("mini-e2e-aaaaaa", "password-a")
            ap.record("mini-e2e-bbbbbb", "password-b")
            later = e2e.AccessPoint("wlan9", "bg", None, False, path, history=True)
            self.assertEqual(later.first(), {"ssid": "mini-e2e-aaaaaa", "password": "password-a"})
            self.assertEqual([item["ssid"] for item in later.recorded],
                             ["mini-e2e-aaaaaa", "mini-e2e-bbbbbb"])
            # A full run starts a new record.
            self.assertIsNone(e2e.AccessPoint("wlan9", "bg", None, False, path).first())

    def test_prefer_moves_the_saved_network_first_and_keeps_it(self) -> None:
        # Once the test removes A from the Mini, the restore must bring back B.
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "access-point.json"
            ap = e2e.AccessPoint("wlan9", "bg", None, False, path)
            ap.record("mini-e2e-aaaaaa", "password-a")
            ap.record("mini-e2e-bbbbbb", "password-b")
            ap.prefer("mini-e2e-bbbbbb")
            later = e2e.AccessPoint("wlan9", "bg", None, False, path, history=True)
            self.assertEqual(later.first()["ssid"], "mini-e2e-bbbbbb")
            self.assertEqual(len(later.recorded), 2)

if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""BLE Wi-Fi setup acceptance test for the Wi-Fi Mini.

Runs on the host with the Mini's USB console, a Bluetooth adapter and a spare
Wi-Fi interface for NetworkManager access points with random credentials.
Needs a build that logs the setup code (version ends in ``+codelog``):

    node tools/combined_load/runner/build.mjs <bench> <directory>

Walks the Mini through BLE setup, wrong codes and passwords, rejoins,
roaming between two test access points and removing the connected network,
checking the display at each step. It always ends by putting the Mini back
on the home network and removing every ``mini-e2e-*`` network.

``--restore-only --output-dir <failed run>`` runs only that restore.

Exit status: 0 pass, 1 fail, 77 when the host cannot run the test (reasons in
result.json).
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import time
from typing import Any, Callable

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.e2e.serial_capture import SerialCapture  # noqa: E402
from tools.e2e.webdriver import (  # noqa: E402
    DEFAULT_CHROMEDRIVER,
    ChromiumWebDriver,
    WebDriverError,
    XvfbDisplay,
)
from tools.e2e.common import (  # noqa: E402
    call_rpc,
    fetch_json,
    require_rpc_result,
    utc_now,
)
from tools.e2e.serial_assert import fatal_serial_markers  # noqa: E402

EXIT_SKIP = 77
# The version suffix of a -DJETKVM_WIFI_SETUP_LOG_CODE=1 build (CMakeLists.txt).
BENCH_VERSION_SUFFIX = "+codelog"
RESTORE_RETRY_S = 30.0
# The Mini rescans for saved networks every 30 s while in BLE setup; allow
# three rescans to rejoin a saved test access point before the restore uses BLE.
FIRST_REJOIN_S = 90.0
SSID_PREFIX = "mini-e2e-"
HOME_PASSWORD_ENV = "MINI_E2E_HOME_PASSWORD"
BLE_CLIENT = Path(__file__).resolve().with_name("ble_setup_client.py")

# Serial log lines from components/jetkvm_c5_wifi/jetkvm_c5_wifi.c. The
# tools/tests contract test renders the firmware's format strings through
# these patterns.
BLE_NAME = re.compile(r"Wi-Fi setup: connect to BLE device (JetKVM_[0-9A-F]{6})\b")
SETUP_CODE = re.compile(r"Wi-Fi setup code: (\d{6})\b")
WRONG_CODE = re.compile(r"Wrong Wi-Fi setup code \((\d+) of (\d+)\)")
CONNECTED = re.compile(
    r"Wi-Fi connected: (\d+\.\d+\.\d+\.\d+) gateway (\d+\.\d+\.\d+\.\d+)")
JOIN_AUTH_FAILED = re.compile(r"Provisioning connection failed: authentication failed")
PROVISIONED = re.compile(r"Provisioned Wi-Fi network saved in P4 NVS")
SETUP_STOPPED = re.compile(r"BLE Wi-Fi provisioning stopped")
SETUP_STARTED = re.compile(r"BLE Wi-Fi provisioning started")
JOINING = re.compile(r"Joining Wi-Fi (.+)$")
# components/jetkvm_board_network/jetkvm_board_network.c: each change of the
# BLE setup step that the display shows.
SETUP_SCREEN = re.compile(r"Wi-Fi setup screen: (.+)$")
# The access point is down this long in the join-retry step; the firmware
# retries the same network after 2, 5 and 10 s before it scans again.
RETRY_OUTAGE_S = 4.0
# How long the stable-code step watches BLE setup: two offline rescans (30 s
# apart) and some margin.
STABLE_CODE_WATCH_S = 75.0
SKIPPED_WRONG_PASSWORD = re.compile(r"Wi-Fi (.+): skipped, its password failed recently")
JOIN_FAILED_AUTH = re.compile(r"Wi-Fi (.+) failed: authentication$")

ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")


# A serial log line index and the match on that line.
Hit = tuple[int, re.Match[str]]


class Skip(Exception):
    """The host lacks something the test needs."""


def strip_ansi(line: str) -> str:
    return ANSI_ESCAPE.sub("", line)


def wrong_code(code: str) -> str:
    return f"{(int(code) + 1) % 1_000_000:06d}"


def random_network() -> tuple[str, str]:
    return SSID_PREFIX + secrets.token_hex(3), secrets.token_urlsafe(12)


class SerialLog:
    """Read-only serial capture that reopens the port after a drop.

    The USB Serial/JTAG console sometimes disappears for a while; each reopen
    writes a new segment file, and the lines of all segments count.
    """

    def __init__(self, device: str, output_dir: Path, history: bool = False) -> None:
        self.device = device
        self.output_dir = output_dir
        # With history, segments that an earlier run left in this directory
        # come first; they count for finding lines, not for fatal markers.
        self.segments: list[Path] = (
            sorted(output_dir.glob("serial-*.log")) if history else [])
        self.first_own_segment = len(self.segments)
        self.capture: SerialCapture | None = None

    def _open(self) -> None:
        index = len(self.segments)
        path = self.output_dir / f"serial-{index:03d}.log"
        while path.exists():  # an earlier run's segment that history skipped
            index += 1
            path = self.output_dir / f"serial-{index:03d}.log"
        capture = SerialCapture(self.device, path)
        capture.start()
        self.capture = capture
        self.segments.append(path)

    def start(self) -> None:
        self._open()

    def stop(self) -> None:
        if self.capture is not None:
            self.capture.stop()
            self.capture = None

    def lines(self, own: bool = False) -> list[str]:
        lines: list[str] = []
        for path in self.segments[self.first_own_segment if own else 0:]:
            if path.exists():
                text = path.read_text(encoding="utf-8", errors="replace")
                lines.extend(strip_ansi(line) for line in text.splitlines())
        return lines

    def mark(self) -> int:
        return len(self.lines())

    def keep_open(self) -> None:
        if self.capture is not None and self.capture.status["error"] is None:
            return
        self.stop()
        try:
            self._open()
        except Exception:
            self.capture = None  # try again on the next poll

    def find(self, pattern: re.Pattern[str], since: int = 0) -> list[Hit]:
        return [(index, match) for index, line in enumerate(self.lines())
                if index >= since and (match := pattern.search(line))]

    def last(self, pattern: re.Pattern[str]) -> Hit | None:
        hits = self.find(pattern)
        return hits[-1] if hits else None

    def wait(
        self, pattern: re.Pattern[str], since: int, timeout: float, count: int = 1
    ) -> list[Hit]:
        deadline = time.monotonic() + timeout
        while True:
            matches = self.find(pattern, since)
            if len(matches) >= count:
                return matches
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    f"serial: {pattern.pattern!r} seen {len(matches)} of {count} "
                    f"times in {timeout:g}s")
            self.keep_open()
            time.sleep(0.25)


class AccessPoint:
    """A WPA2 access point run by NetworkManager in shared (DHCP + NAT) mode."""

    CONNECTION = "mini-e2e-ap"

    def __init__(self, iface: str, band: str, channel: int | None, sudo: bool,
                 state_path: Path, history: bool = False) -> None:
        # Every access point this run (or, with history, the failed run)
        # started, oldest first. The first one is the network the Mini keeps
        # saved for the whole test.
        self.state_path = state_path
        self.recorded: list[dict[str, str]] = []
        if history:
            try:
                self.recorded = json.loads(
                    state_path.read_text(encoding="utf-8"))["accessPoints"]
            except (OSError, ValueError, KeyError, TypeError):
                self.recorded = []
        self.iface = iface
        self.band = band
        self.channel = channel
        self.prefix = ["sudo", "-n"] if sudo else []
        self.ssid: str | None = None

    def _nmcli(self, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [*self.prefix, "nmcli", *args], capture_output=True, text=True, check=check,
            timeout=60)

    def start(self, ssid: str, password: str) -> None:
        self.stop()
        settings = [
            "802-11-wireless.mode", "ap", "802-11-wireless.band", self.band,
            "ipv4.method", "shared", "ipv6.method", "disabled",
            "wifi-sec.key-mgmt", "wpa-psk", "wifi-sec.proto", "rsn",
            "wifi-sec.pairwise", "ccmp", "wifi-sec.group", "ccmp",
            "wifi-sec.psk", password,
        ]
        if self.channel is not None:
            settings += ["802-11-wireless.channel", str(self.channel)]
        self._nmcli("connection", "add", "type", "wifi", "ifname", self.iface,
                    "con-name", self.CONNECTION, "autoconnect", "no", "ssid", ssid,
                    *settings)
        # Recorded before the Mini can depend on this network, so a later
        # --restore-only can bring it back.
        self.record(ssid, password)
        self._nmcli("connection", "up", self.CONNECTION)
        self.ssid = ssid

    def record(self, ssid: str, password: str) -> None:
        if all(item["ssid"] != ssid for item in self.recorded):
            self.recorded.append({"ssid": ssid, "password": password})
        self.state_path.write_text(
            json.dumps({"accessPoints": self.recorded}, indent=2) + "\n", encoding="utf-8")

    def first(self) -> dict[str, str] | None:
        """The recorded access point whose network the Mini has saved."""
        return self.recorded[0] if self.recorded else None

    def prefer(self, ssid: str) -> None:
        """Record ``ssid`` first: call it before the test removes the first
        network from the Mini, so the restore brings back one it has saved."""
        self.recorded.sort(key=lambda item: item["ssid"] != ssid)
        self.state_path.write_text(
            json.dumps({"accessPoints": self.recorded}, indent=2) + "\n", encoding="utf-8")

    def restart_first(self) -> bool:
        """Start the first recorded access point, which the Mini has saved."""
        first = self.first()
        if first is None:
            return False
        self.start(first["ssid"], first["password"])
        return True

    def adopt(self) -> None:
        """Take over an access point that an earlier run left up."""
        result = self._nmcli("-g", "GENERAL.STATE,802-11-wireless.ssid", "connection",
                             "show", self.CONNECTION, check=False)
        lines = result.stdout.splitlines()
        if result.returncode == 0 and len(lines) == 2 and lines[0] == "activated":
            self.ssid = lines[1]

    def stop(self) -> None:
        self._nmcli("connection", "down", self.CONNECTION, check=False)
        self._nmcli("connection", "delete", self.CONNECTION, check=False)
        self.ssid = None

    def interface(self) -> ipaddress.IPv4Interface:
        output = subprocess.run(
            ["ip", "-4", "-o", "addr", "show", "dev", self.iface],
            capture_output=True, text=True, check=True).stdout
        match = re.search(r"inet (\d+\.\d+\.\d+\.\d+/\d+)", output)
        if not match:
            raise RuntimeError(f"{self.iface} has no IPv4 address")
        return ipaddress.IPv4Interface(match.group(1))

    def address(self) -> str:
        return str(self.interface().ip)

    @staticmethod
    def answers(host: str) -> bool:
        try:
            fetch_json(f"http://{host}/device/status", timeout=1.0)
            return True
        except Exception:
            return False

    def find_device(self) -> str | None:
        """The address on this access point's subnet that answers as a Mini."""
        interface = self.interface()
        hosts = [str(host) for host in interface.network.hosts() if host != interface.ip]
        with ThreadPoolExecutor(max_workers=32) as pool:
            return next((host for host, ok in zip(hosts[:1022], pool.map(self.answers, hosts[:1022]))
                         if ok), None)


class DeviceRpc:
    """JSON-RPC over the device UI's WebRTC channel in one headed Chrome."""

    def __init__(self, args: argparse.Namespace, output_dir: Path) -> None:
        self.args = args
        self.output_dir = output_dir
        self.xvfb: XvfbDisplay | None = None
        self.driver: ChromiumWebDriver | None = None
        self.client: Any = None
        self.url: str | None = None

    def check(self) -> str | None:
        """Why this host cannot run the browser, or None."""
        try:
            self.xvfb = XvfbDisplay(self.output_dir / "xvfb.log", display=self.args.display)
            self.driver = ChromiumWebDriver(
                self.output_dir / "chromedriver.log", display=self.args.display,
                browser=self.args.browser, chromedriver=self.args.chromedriver)
        except WebDriverError as exc:
            return str(exc)
        return None

    def start(self) -> None:
        if self.xvfb is None or self.driver is None:
            raise RuntimeError("DeviceRpc.check() must pass first")
        self.xvfb.start()
        self.client = self.driver.start()

    def stop(self) -> None:
        if self.driver is not None:
            self.driver.stop()
        if self.xvfb is not None:
            self.xvfb.stop()

    def _ready(self) -> bool:
        return bool(self.client.execute_sync(
            "const h = window.__kvmTestHooks;"
            "return !!h && h.isWebRTCConnected() && h.isHidRpcReady();"))

    def _connect(self, url: str, timeout: float) -> None:
        if self.url == url:
            try:
                if self._ready():
                    return
            except Exception:
                pass
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            # A page that failed to load, or loaded before the device was
            # reachable, never connects; load it again. Over the test access
            # point a page load can take more than 15 s, so a load in
            # progress is not stopped before 45 s.
            try:
                self.client.navigate(url)
            except Exception:
                pass
            self.url = url
            started = time.monotonic()
            retry = min(deadline, started + 45.0)
            while time.monotonic() < retry:
                try:
                    if self._ready():
                        self._pause_video()
                        return
                    if time.monotonic() - started > 15.0 and self._load_failed():
                        break
                except Exception:
                    pass
                time.sleep(0.5)
        self.url = None
        raise RuntimeError(f"no RPC session with {url} after {timeout:g}s")

    def _load_failed(self) -> bool:
        """The page load ended without the device UI (an error page)."""
        return bool(self.client.execute_sync(
            "return document.readyState === 'complete' && !window.__kvmTestHooks;"))

    def _pause_video(self) -> None:
        """The test needs only RPC, so it pauses video. Over a weak test
        access point video loses packets, and its retransmits take CPU time
        and airtime from the RPC calls. The pause ends with this session."""
        try:
            call_rpc(self.client, "setVideoStreamPaused", {"paused": True})
        except Exception:
            pass

    def call(self, url: str, method: str, params: dict[str, Any] | None = None,
             timeout: float = 60.0) -> Any:
        self._connect(url, timeout)
        return require_rpc_result(call_rpc(self.client, method, params or {}), method)

    def call_before_leaving(self, url: str, method: str,
                            params: dict[str, Any]) -> dict[str, Any]:
        """A request that makes the Mini leave its network (a connect).

        The Mini answers, then scans for about 10 s after 3 s. If the
        answer's packet is lost once, its retransmission falls into the scan
        and the UI's 10 s RPC timeout ends first, although the Mini acts on
        the request. The caller checks what the Mini does; a lost answer is
        reported as {"answerLost": True}.
        """
        self._connect(url, 60.0)
        response = call_rpc(self.client, method, params)
        error = response.get("error") if isinstance(response, dict) else None
        if isinstance(error, dict) and "RPC timeout" in str(error.get("message", "")):
            return {"answerLost": True}
        return require_rpc_result(response, method)


def device_url(address: str) -> str:
    return f"http://{address}/"


def saved_ssids(settings: dict[str, Any]) -> list[str]:
    return [item["ssid"] for item in settings.get("networks", [])]


# getWifiSettings takes ssid, channel and rssi from the C5's link details,
# which refresh every 2 s; right after a join they can still name the
# previous network (networks[].connected is already right).
# The budget also covers a new RPC session to the Mini's new address.
SSID_REFRESH_S = 120.0


def on_network(ssid: str) -> Callable[[dict[str, Any]], bool]:
    """getWifiSettings agrees in both places that the Mini is on ``ssid``."""
    def check(settings: dict[str, Any]) -> bool:
        connected = [item["ssid"] for item in settings.get("networks", [])
                     if item.get("connected")]
        return settings.get("ssid") == ssid and connected == [ssid]
    return check


class Run:
    def __init__(self, args: argparse.Namespace, output_dir: Path) -> None:
        self.args = args
        self.output_dir = output_dir
        self.serial = SerialLog(args.serial, output_dir, history=args.restore_only)
        self.ap = AccessPoint(args.ap_iface, args.ap_band, args.ap_channel, args.ap_sudo,
                              output_dir / "access-point.json", history=args.restore_only)
        self.rpc = DeviceRpc(args, output_dir)
        self.home_password = os.environ.get(HOME_PASSWORD_ENV, "")
        self.steps: list[dict[str, Any]] = []
        self.started = time.monotonic()
        # Where the Mini answers RPC during the test, and its BLE name.
        self.current_url: str | None = args.url
        self.ble_name: str | None = None
        # Why BLE cannot be used on this host; --restore-only can do without.
        self.ble_unavailable: str | None = None
        # Set before the first change to the Mini or the host; the restore
        # runs only after that (or with --restore-only).
        self.changed = False

    def step(self, name: str, action: Callable[[], dict[str, Any]]) -> dict[str, Any]:
        started = time.monotonic()
        record: dict[str, Any] = {"step": name, "at": utc_now()}
        self.steps.append(record)
        print(f"[{time.monotonic() - self.started:7.1f}s] {name}", flush=True)
        try:
            record.update(action())
            record["passed"] = True
        except Exception as exc:
            record["passed"] = False
            record["error"] = f"{type(exc).__name__}: {exc}"
            raise
        finally:
            record["elapsedS"] = round(time.monotonic() - started, 3)
        return record

    # -- helpers -----------------------------------------------------------

    def wait_settings(self, url: str, predicate: Callable[[dict[str, Any]], bool],
                      timeout: float) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        last: Any = None
        while time.monotonic() < deadline:
            try:
                last = self.rpc.call(url, "getWifiSettings", timeout=20.0)
                if predicate(last):
                    return last
            except Exception as exc:
                last = f"{type(exc).__name__}: {exc}"
            time.sleep(2.0)
        raise RuntimeError(f"getWifiSettings never matched in {timeout:g}s: {last}")

    def ble_environment(self, passphrase: str) -> dict[str, str]:
        environment = dict(os.environ, MINI_E2E_BLE_PASSPHRASE=passphrase)
        if self.args.idf_path is not None:
            environment["IDF_PATH"] = str(self.args.idf_path)
        return environment

    def ble_session(self, code: str, ssid: str | None = None,
                    passphrase: str = "") -> dict[str, Any]:
        if self.ble_name is None:
            raise RuntimeError("the BLE name has not appeared in the serial log")
        command = [
            self.args.prov_python, str(BLE_CLIENT),
            "--esp-prov-dir", str(self.args.esp_prov_dir),
            "--name", self.ble_name, "--code", code, "--adapter", self.args.ble_adapter,
        ]
        if ssid is not None:
            command += ["--ssid", ssid]
        completed = subprocess.run(
            command, capture_output=True, text=True, env=self.ble_environment(passphrase),
            timeout=240)
        log = self.output_dir / "ble-client.log"
        with log.open("a", encoding="utf-8") as handle:
            handle.write(f"== {utc_now()} code={code} ssid={ssid}\n{completed.stderr}\n")
        lines = completed.stdout.strip().splitlines()
        if completed.returncode != 0 or not lines:
            raise RuntimeError(f"BLE client exited {completed.returncode}; see {log}")
        return json.loads(lines[-1])

    def screens(self, since: int) -> list[str]:
        """The setup steps the display showed since ``since``, in order."""
        return [match.group(1).strip() for _, match in self.serial.find(SETUP_SCREEN, since)]

    def expect_screens(self, since: int, expected: list[str]) -> list[str]:
        """The display showed ``expected`` in this order, and never the code
        again after the phone entered it."""
        shown = self.screens(since)
        position = 0
        for step in shown:
            if position < len(expected) and step == expected[position]:
                position += 1
        if position < len(expected):
            raise RuntimeError(f"display steps {shown}, expected {expected} in order")
        if "code accepted" in shown and "code" in shown[shown.index("code accepted"):]:
            raise RuntimeError(f"the code came back after it was accepted: {shown}")
        return shown

    def wait_joined(self, since: int, gateway: str, timeout: float) -> str:
        """The Mini's address once it joined the access point at ``gateway``."""
        deadline = time.monotonic() + timeout
        while True:
            for _, match in self.serial.find(CONNECTED, since):
                if match.group(2) == gateway:
                    return match.group(1)
            if time.monotonic() >= deadline:
                raise RuntimeError(f"the Mini did not join the access point at {gateway}")
            self.serial.keep_open()
            time.sleep(0.25)

    def locate(self) -> tuple[str, str | None]:
        """Where the Mini is: ("ap", url), ("ble", code) or ("elsewhere", None).

        A Mini that answers on the access point wins over the serial log: the
        log can end with a setup code when the capture missed a later join.
        """
        # The restore loop decides from this log; reopen a dropped console.
        self.serial.keep_open()
        code = self.serial.last(SETUP_CODE)
        stopped = self.serial.last(SETUP_STOPPED)
        joined = self.serial.last(CONNECTED)
        latest = max((hit[0] for hit in (stopped, joined) if hit is not None), default=-1)
        in_setup = code is not None and code[0] > latest
        if self.ap.ssid is not None:
            try:
                # The logged address can be stale (a new DHCP lease after the
                # access point came back); use it only if it answers.
                if not in_setup and joined is not None and \
                        joined[1].group(2) == self.ap.address() and \
                        self.ap.answers(joined[1].group(1)):
                    return "ap", device_url(joined[1].group(1))
                host = self.ap.find_device()
                if host is not None:
                    return "ap", device_url(host)
            except (RuntimeError, OSError, subprocess.SubprocessError):
                pass
        if in_setup:
            name = self.serial.last(BLE_NAME)
            self.ble_name = name[1].group(1) if name is not None else self.ble_name
            return "ble", code[1].group(1)
        return "elsewhere", None

    # -- preflight ---------------------------------------------------------

    def preflight(self) -> None:
        missing = []
        if self.args.home_ssid.startswith(SSID_PREFIX):
            # The clean-up removes networks with this prefix.
            missing.append(f"the home network must not start with {SSID_PREFIX}")
        if not self.home_password:
            missing.append(f"{HOME_PASSWORD_ENV} is not set")
        if not Path(self.args.serial).exists():
            missing.append(f"serial device {self.args.serial} is missing")
        if not Path(f"/sys/class/net/{self.args.ap_iface}/wireless").exists():
            missing.append(f"{self.args.ap_iface} is not a Wi-Fi interface")
        ble_missing = []
        if not Path(f"/sys/class/bluetooth/{self.args.ble_adapter}").exists():
            ble_missing.append(f"Bluetooth adapter {self.args.ble_adapter} is missing")
        if not (self.args.esp_prov_dir / "esp_prov.py").exists():
            ble_missing.append(f"no esp_prov.py in {self.args.esp_prov_dir}")
        if not Path(self.args.prov_python).exists():
            ble_missing.append(f"Python for esp_prov {self.args.prov_python} is missing")
        if not ble_missing:
            # A broken esp_prov environment would first show after the Mini
            # is offline, and would also break the restore.
            try:
                probe = subprocess.run(
                    [self.args.prov_python, str(BLE_CLIENT), "--esp-prov-dir",
                     str(self.args.esp_prov_dir), "--probe"],
                    capture_output=True, text=True, env=self.ble_environment(""), timeout=60)
                if probe.returncode != 0:
                    ble_missing.append(f"esp_prov does not load: {probe.stdout.strip()}")
            except (OSError, subprocess.SubprocessError) as exc:
                ble_missing.append(f"esp_prov probe did not run: {type(exc).__name__}: {exc}")
        if ble_missing and self.args.restore_only:
            # The restore can still work over the access point and RPC.
            self.ble_unavailable = "; ".join(ble_missing)
        else:
            missing += ble_missing
        browser = self.rpc.check()
        if browser is not None:
            missing.append(browser)
        if missing:
            raise Skip("; ".join(missing))

    # -- test --------------------------------------------------------------

    def test(self) -> None:
        args = self.args
        ssid_a, password_a = random_network()
        ssid_b, password_b = random_network()

        def check_start() -> dict[str, Any]:
            # A release build does not log the code: the test could not set
            # the Mini up again after it removes the home network.
            stats = fetch_json(args.url.rstrip("/") + "/webrtc/stats", timeout=10.0)
            version = stats.get("firmware", {}).get("version", "")
            if not version.endswith(BENCH_VERSION_SUFFIX):
                raise RuntimeError(
                    f"firmware {version!r} is not a -DJETKVM_WIFI_SETUP_LOG_CODE=1 build")
            settings = self.rpc.call(args.url, "getWifiSettings")
            if settings.get("ssid") != args.home_ssid:
                raise RuntimeError(f"the Mini is not on {args.home_ssid}: {settings}")
            # Removing the home network must leave only the test network.
            if saved_ssids(settings) != [args.home_ssid]:
                raise RuntimeError(
                    f"save only {args.home_ssid} before the test: {saved_ssids(settings)}")
            return {"version": version, "settings": settings}

        self.step("the Mini runs a bench build on the home network only", check_start)

        def join_a() -> dict[str, Any]:
            self.changed = True
            self.ap.start(ssid_a, password_a)
            gateway = self.ap.address()
            # Starting the access point takes the AP interface off the home
            # network, and the host's routing changes for some seconds; a
            # request sent then can lose its answer. Wait for a round trip.
            self.wait_settings(args.url, lambda settings: True, 60.0)
            since = self.serial.mark()
            response = self.rpc.call_before_leaving(
                args.url, "saveWifiNetwork",
                {"ssid": ssid_a, "password": password_a, "connect": True})
            address = self.wait_joined(since, gateway, 90.0)
            self.current_url = device_url(address)
            self.wait_settings(self.current_url, on_network(ssid_a), SSID_REFRESH_S)
            return {"ssid": ssid_a, "address": address, "response": response}

        self.step("save and join access point A", join_a)

        def remove_home() -> dict[str, Any]:
            self.rpc.call(self.current_url, "removeWifiNetwork", {"ssid": args.home_ssid})
            settings = self.rpc.call(self.current_url, "getWifiSettings")
            if saved_ssids(settings) != [ssid_a]:
                raise RuntimeError(f"expected only {ssid_a} saved: {settings}")
            return {"saved": saved_ssids(settings)}

        self.step("remove the home network", remove_home)

        codes: dict[str, str] = {}

        def start_setup() -> dict[str, Any]:
            since = self.serial.mark()
            self.ap.stop()
            self.current_url = None
            self.ble_name = self.serial.wait(BLE_NAME, since, 150.0)[-1][1].group(1)
            codes["first"] = self.serial.wait(SETUP_CODE, since, 10.0)[-1][1].group(1)
            return {"bleName": self.ble_name}

        self.step("lose the only network and start BLE setup", start_setup)

        def wrong_codes() -> dict[str, Any]:
            since = self.serial.mark()
            sessions = []
            deadline = time.monotonic() + 240.0
            while len(self.serial.find(WRONG_CODE, since)) < 3:
                if time.monotonic() >= deadline:
                    raise RuntimeError(f"fewer than 3 misses counted: {sessions}")
                counted = len(self.serial.find(WRONG_CODE, since))
                result = self.ble_session(wrong_code(codes["first"]))
                sessions.append(result)
                if result.get("session"):
                    raise RuntimeError(f"a wrong code opened a session: {result}")
                if result.get("connected"):
                    # Let the miss reach the log before the next session, so
                    # no extra session lands on the next code. A link that
                    # dropped before the handshake counts no miss.
                    try:
                        self.serial.wait(WRONG_CODE, since, 10.0, count=counted + 1)
                    except RuntimeError:
                        pass
            hits = self.serial.find(WRONG_CODE, since)
            misses = [(int(m.group(1)), int(m.group(2))) for _, m in hits]
            if misses[:3] != [(1, 3), (2, 3), (3, 3)]:
                raise RuntimeError(f"unexpected miss count: {misses}")
            codes["second"] = self.serial.wait(SETUP_CODE, hits[2][0] + 1, 60.0)[-1][1].group(1)
            # A rejected code leaves the code on the display until the phone
            # disconnects; then the display shows the setup instructions.
            screens = self.expect_screens(since, ["code", "advertising"])
            if "code accepted" in screens:
                raise RuntimeError(f"a wrong code was shown as accepted: {screens}")
            return {"sessions": sessions, "misses": misses, "screens": screens}

        self.step("three wrong codes start setup again", wrong_codes)

        def wrong_password() -> dict[str, Any]:
            self.ap.start(ssid_b, password_b)
            since = self.serial.mark()
            result = self.ble_session(codes["second"], ssid_b, password_b[::-1])
            if not result.get("session") or result.get("state") != "failed":
                raise RuntimeError(f"expected a failed join: {result}")
            self.serial.wait(JOIN_AUTH_FAILED, since, 10.0)
            if self.serial.find(SETUP_STOPPED, since):
                raise RuntimeError("BLE setup stopped after a failed join")
            self.serial.wait(SETUP_SCREEN, since, 10.0, count=5)
            screens = self.expect_screens(
                since, ["code", "code accepted", "joining", "code accepted", "advertising"])
            return {"session": result, "screens": screens}

        self.step("a wrong network password fails and BLE setup stays up", wrong_password)

        def join_b() -> dict[str, Any]:
            gateway = self.ap.address()
            since = self.serial.mark()
            result = self.ble_session(codes["second"], ssid_b, password_b)
            if not result.get("session") or result.get("state") == "failed":
                raise RuntimeError(f"expected a join: {result}")
            self.serial.wait(PROVISIONED, since, 60.0)
            address = self.wait_joined(since, gateway, 60.0)
            self.serial.wait(SETUP_STOPPED, since, 30.0)
            screens = self.expect_screens(since, ["code", "code accepted", "joining"])
            self.current_url = device_url(address)
            settings = self.wait_settings(self.current_url, on_network(ssid_b), SSID_REFRESH_S)
            if settings.get("provisioning"):
                raise RuntimeError(f"expected no BLE setup: {settings}")
            if sorted(saved_ssids(settings)) != sorted([ssid_a, ssid_b]):
                raise RuntimeError(f"expected {ssid_a} and {ssid_b} saved: {settings}")
            return {"session": result, "address": address, "settings": settings,
                    "screens": screens}

        self.step("set up access point B over BLE", join_b)

        def expect_join_without_setup(since: int, gateway: str, ssid: str,
                                      back_at: float) -> dict[str, Any]:
            # Up to three retries of the lost network (2, 5 and 10 s apart,
            # each join up to 20 s), then a scan, before the other one.
            address = self.wait_joined(since, gateway, 150.0)
            rejoin_s = round(time.monotonic() - back_at, 1)
            if self.serial.find(SETUP_STARTED, since):
                raise RuntimeError("BLE setup started although a saved network was up")
            self.current_url = device_url(address)
            settings = self.wait_settings(self.current_url, on_network(ssid), SSID_REFRESH_S)
            joins = [m.group(1) for _, m in self.serial.find(JOINING, since)]
            return {"address": address, "rejoinS": rejoin_s, "joins": joins}

        def join_retry() -> dict[str, Any]:
            since = self.serial.mark()
            self.ap.stop()
            time.sleep(RETRY_OUTAGE_S)
            self.ap.start(ssid_b, password_b)
            return expect_join_without_setup(since, self.ap.address(), ssid_b,
                                             time.monotonic())

        self.step("B drops for a few seconds; the Mini rejoins B", join_retry)

        def move_to_a() -> dict[str, Any]:
            since = self.serial.mark()
            self.ap.start(ssid_a, password_a)  # stops B first: one radio
            return expect_join_without_setup(since, self.ap.address(), ssid_a,
                                             time.monotonic())

        self.step("B goes away and A comes up; the Mini moves to A", move_to_a)

        def same_password() -> dict[str, Any]:
            since = self.serial.mark()
            response = self.rpc.call_before_leaving(
                self.current_url, "saveWifiNetwork", {"ssid": ssid_a, "password": password_a})
            if not response.get("connecting") and not response.get("answerLost"):
                raise RuntimeError(f"saving the current network did not test it: {response}")
            self.serial.wait(JOINING, since, 30.0)
            address = self.wait_joined(since, self.ap.address(), 90.0)
            self.current_url = device_url(address)
            settings = self.wait_settings(
                self.current_url,
                lambda v: not v.get("applying") and on_network(ssid_a)(v), 60.0)
            if settings.get("ssid") != ssid_a or settings.get("connect_error"):
                raise RuntimeError(f"expected {ssid_a} and no error: {settings}")
            if sorted(saved_ssids(settings)) != sorted([ssid_a, ssid_b]):
                raise RuntimeError(f"the saved list changed: {settings}")
            if self.serial.find(SETUP_STARTED, since):
                raise RuntimeError("BLE setup started")
            return {"response": response, "address": address}

        self.step("save A again with the same password; the Mini rejoins A", same_password)

        def wrong_password_for_current() -> dict[str, Any]:
            since = self.serial.mark()
            response = self.rpc.call_before_leaving(
                self.current_url, "saveWifiNetwork", {"ssid": ssid_a, "password": password_a[::-1]})
            if not response.get("connecting") and not response.get("answerLost"):
                raise RuntimeError(f"saving the current network did not test it: {response}")
            # The failed join sends the Mini back to its scan, which joins A
            # with the saved (old) password.
            joined = self.serial.wait(CONNECTED, since, 120.0)
            address = joined[-1][1].group(1)
            self.current_url = device_url(address)
            settings = self.wait_settings(
                self.current_url,
                lambda v: not v.get("applying") and on_network(ssid_a)(v), 60.0)
            if settings.get("ssid") != ssid_a or settings.get("connect_error") != "Wrong password":
                raise RuntimeError(f"expected {ssid_a} with 'Wrong password': {settings}")
            if self.serial.find(SETUP_STARTED, since):
                raise RuntimeError("BLE setup started")
            return {"response": response, "address": address,
                    "connectError": settings.get("connect_error")}

        self.step("save A with a wrong password; the Mini keeps the old one",
                  wrong_password_for_current)

        def remove_current_offline() -> dict[str, Any]:
            # From here on B is the test network the Mini has saved.
            self.ap.prefer(ssid_b)
            since = self.serial.mark()
            self.rpc.call(self.current_url, "removeWifiNetwork", {"ssid": ssid_a})
            self.current_url = None
            self.serial.wait(SETUP_STARTED, since, 120.0)
            self.ble_name = self.serial.wait(BLE_NAME, since, 10.0)[-1][1].group(1)
            self.serial.wait(SETUP_CODE, since, 10.0)
            return {"bleName": self.ble_name}

        self.step("remove A while B is down; BLE setup starts", remove_current_offline)

        def saved_network_returns() -> dict[str, Any]:
            since = self.serial.mark()
            self.ap.start(ssid_b, password_b)
            back_at = time.monotonic()
            address = self.wait_joined(since, self.ap.address(), 120.0)
            self.serial.wait(SETUP_STOPPED, since, 30.0)
            # No phone takes part: the display shows the join, and before it
            # at most the instructions again after a rescan that missed B.
            screens = self.screens(since)
            if not screens or screens[-1] != "connecting" or \
                    set(screens) - {"connecting", "advertising"}:
                raise RuntimeError(f"display steps {screens}, expected only the join")
            self.current_url = device_url(address)
            settings = self.wait_settings(self.current_url, on_network(ssid_b), SSID_REFRESH_S)
            if settings.get("provisioning"):
                raise RuntimeError(f"expected no BLE setup: {settings}")
            if saved_ssids(settings) != [ssid_b]:
                raise RuntimeError(f"expected only {ssid_b} saved: {settings}")
            return {"address": address, "rejoinS": round(time.monotonic() - back_at, 1),
                    "screens": screens}

        self.step("B comes up; the Mini leaves BLE setup and joins B", saved_network_returns)

        def wrong_password_in_range() -> dict[str, Any]:
            self.rpc.call(self.current_url, "saveWifiNetwork",
                          {"ssid": ssid_a, "password": password_a[::-1]})
            since = self.serial.mark()
            self.ap.start(ssid_a, password_a)  # in place of B: one radio
            self.current_url = None
            # The first try of A can come before BLE setup starts or at a
            # later rescan (a scan can miss a new AP); watch from its failure.
            deadline = time.monotonic() + 240.0
            failed = None
            while failed is None:
                failed = next((hit for hit in self.serial.find(JOIN_FAILED_AUTH, since)
                               if hit[1].group(1) == ssid_a), None)
                if failed is None:
                    if time.monotonic() >= deadline:
                        raise RuntimeError(f"no failed join of {ssid_a} in 240s")
                    self.serial.keep_open()
                    time.sleep(0.5)
            code = self.serial.wait(SETUP_CODE, since, 60.0)[0][1].group(1)
            time.sleep(STABLE_CODE_WATCH_S)
            codes = [m.group(1) for _, m in self.serial.find(SETUP_CODE, since)]
            retried = [m.group(1) for _, m in self.serial.find(JOINING, failed[0] + 1)]
            skipped = len(self.serial.find(SKIPPED_WRONG_PASSWORD, failed[0] + 1))
            if any(other != code for other in codes):
                raise RuntimeError(f"the setup code changed: {codes}")
            if ssid_a in retried:
                raise RuntimeError(f"the Mini tried {ssid_a} again within "
                                   f"{STABLE_CODE_WATCH_S:g}s of its failure: {retried}")
            # Back to B, which the Mini has saved with the right password.
            since = self.serial.mark()
            self.ap.start(ssid_b, password_b)
            address = self.wait_joined(since, self.ap.address(), 120.0)
            self.current_url = device_url(address)
            self.wait_settings(self.current_url, on_network(ssid_b), SSID_REFRESH_S)
            self.rpc.call(self.current_url, "removeWifiNetwork", {"ssid": ssid_a})
            return {"code": code, "codes": codes, "joinsAfterFailure": retried,
                    "skippedRescans": skipped}

        self.step("a saved network with a wrong password in range; the code stays",
                  wrong_password_in_range)

        def remove_current_with_fallback() -> dict[str, Any]:
            since = self.serial.mark()
            self.rpc.call(self.current_url, "saveWifiNetwork",
                          {"ssid": args.home_ssid, "password": self.home_password})
            self.rpc.call(self.current_url, "removeWifiNetwork", {"ssid": ssid_b})
            self.current_url = None
            settings = self.wait_settings(
                args.url, lambda v: v.get("ssid") == args.home_ssid, 120.0)
            self.current_url = args.url
            if self.serial.find(SETUP_STARTED, since):
                raise RuntimeError("BLE setup started although the home network was in range")
            if saved_ssids(settings) != [args.home_ssid]:
                raise RuntimeError(f"expected only {args.home_ssid} saved: {settings}")
            return {"joins": [m.group(1) for _, m in self.serial.find(JOINING, since)]}

        self.step("save home, remove B; the Mini moves to the home network",
                  remove_current_with_fallback)

    # -- restore -----------------------------------------------------------

    def restore(self) -> None:
        args = self.args

        def put_back() -> dict[str, Any]:
            if not self.home_password:
                raise RuntimeError(f"{HOME_PASSWORD_ENV} is not set; restore by hand")
            home = {"ssid": args.home_ssid, "password": self.home_password}
            actions: list[str] = []
            # A transport that failed is tried again after RESTORE_RETRY_S.
            next_try = {"ap": 0.0, "ble": 0.0}
            # When access point A (saved on the Mini for the whole test) came
            # up for a Mini in BLE setup; None while it has not.
            first_up_at: float | None = None
            deadline = time.monotonic() + 300.0
            while True:
                where, detail = self.locate()
                first = self.ap.first()
                if where == "ble" and first is not None and first_up_at is None:
                    # The Mini leaves BLE setup and rejoins a saved access
                    # point by itself.
                    try:
                        if self.ap.ssid != first["ssid"]:
                            self.ap.restart_first()
                            actions.append(f"ap: started {first['ssid']} again")
                        first_up_at = time.monotonic()
                    except Exception as exc:
                        actions.append(f"ap: {type(exc).__name__}: {exc}")
                # A BLE connection holds off the Mini's rescan: while it may
                # still rejoin A, send nothing over BLE.
                waiting_for_first = (first_up_at is not None and
                                     time.monotonic() - first_up_at < FIRST_REJOIN_S)
                if where == "ble" and (waiting_for_first or self.ble_unavailable is not None):
                    note = "ap: waiting for a rejoin" if waiting_for_first else "ble: unavailable"
                    if not actions or actions[-1] != note:
                        actions.append(note)
                elif where in next_try and time.monotonic() >= next_try[where]:
                    next_try[where] = time.monotonic() + RESTORE_RETRY_S
                    try:
                        if where == "ap":
                            self.rpc.call_before_leaving(detail, "saveWifiNetwork",
                                                         {**home, "connect": True})
                        else:
                            result = self.ble_session(detail, args.home_ssid,
                                                      self.home_password)
                            # The Mini may end BLE setup before the state
                            # read; the home URL check below decides.
                            if not result.get("session") or result.get("state") == "failed":
                                raise RuntimeError(f"BLE setup did not join: {result}")
                        actions.append(f"{where}: sent")
                    except Exception as exc:
                        actions.append(f"{where}: {type(exc).__name__}: {exc}")
                try:
                    settings = self.rpc.call(args.url, "getWifiSettings", timeout=20.0)
                    if settings.get("ssid") == args.home_ssid:
                        self.current_url = args.url
                        return {"actions": actions}
                except Exception:
                    pass
                if time.monotonic() >= deadline:
                    raise RuntimeError(f"the Mini is not back on {args.home_ssid}: {actions}")

        def forget_test_networks() -> dict[str, Any]:
            settings = self.rpc.call(args.url, "getWifiSettings")
            removed = []
            for ssid in saved_ssids(settings):
                if ssid.startswith(SSID_PREFIX) and ssid != args.home_ssid:
                    self.rpc.call(args.url, "removeWifiNetwork", {"ssid": ssid})
                    removed.append(ssid)
            return {"removed": removed}

        # No access point is up (a failed run stopped it, or this is
        # --restore-only): take over one left up, or start the recorded one;
        # the Mini still has that network saved and joins it again, also when
        # the log shows no setup code. If nmcli fails, the restore loop still
        # tries RPC and BLE.
        try:
            if self.ap.ssid is None:
                self.ap.adopt()
            if self.ap.ssid is None and self.ap.restart_first():
                print(f"restarted access point {self.ap.ssid}", flush=True)
        except (OSError, subprocess.SubprocessError) as exc:
            print(f"could not prepare the access point: {exc}", flush=True)
        self.step("restore: back on the home network", put_back)
        # Only now: while the Mini is not home, the access point is its way in.
        self.ap.stop()
        self.ap.state_path.unlink(missing_ok=True)
        self.step("restore: forget the test networks", forget_test_networks)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    home = Path.home()
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", required=True,
                        help="the Mini on the home network, for example http://192.168.1.100/")
    parser.add_argument("--home-ssid", required=True,
                        help=f"home network; its password comes from ${HOME_PASSWORD_ENV}")
    parser.add_argument("--serial", default="/dev/ttyACM0")
    parser.add_argument("--ap-iface", required=True,
                        help="spare Wi-Fi interface that NetworkManager can run as an AP")
    parser.add_argument("--ap-band", choices=("bg", "a"), default="bg",
                        help="bg = 2.4 GHz, a = 5 GHz")
    parser.add_argument("--ap-channel", type=int)
    parser.add_argument("--ap-sudo", action="store_true", help="run nmcli with sudo -n")
    parser.add_argument("--ble-adapter", default="hci0")
    parser.add_argument("--prov-python", default=str(home / ".prov-venv/bin/python"),
                        help="Python that has esp_prov's dependencies (bleak, protobuf)")
    parser.add_argument("--esp-prov-dir", type=Path,
                        default=home / "jetkvm-prov/component/tool/esp_prov")
    parser.add_argument("--idf-path", type=Path, default=home / "jetkvm-prov/idf",
                        help="IDF_PATH for esp_prov (components/protocomm/python)")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--display", default=":127")
    parser.add_argument("--browser", default="google-chrome",
                        help="must match ChromeDriver's major version; with the Chromium "
                             "snap's driver, pass the snap's own binary "
                             "/snap/chromium/current/usr/lib/chromium-browser/chrome "
                             "(the /snap/bin/chromium wrapper never starts a session)")
    parser.add_argument("--chromedriver", default=DEFAULT_CHROMEDRIVER)
    parser.add_argument("--restore-only", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    output_dir = args.output_dir.absolute()
    output_dir.mkdir(parents=True, exist_ok=True)
    run = Run(args, output_dir)
    result: dict[str, Any] = {"startedAt": utc_now(), "restoreOnly": args.restore_only}
    status = 0
    try:
        run.preflight()
    except Skip as exc:
        result.update(skipped=str(exc))
        status = EXIT_SKIP
    if status == 0:
        started = False
        try:
            run.serial.start()
            run.rpc.start()
            started = True
            if not args.restore_only:
                run.test()
        except Exception as exc:
            result["error"] = f"{type(exc).__name__}: {exc}"
            status = 1
        finally:
            if started and (run.changed or args.restore_only):
                try:
                    run.restore()
                except Exception as exc:
                    result["restoreError"] = f"{type(exc).__name__}: {exc}"
                    if run.ap.ssid is not None:
                        result["accessPointLeftUp"] = {
                            "ssid": run.ap.ssid, "state": str(run.ap.state_path)}
                    status = 1
            run.rpc.stop()
            run.serial.stop()
        fatal = fatal_serial_markers("\n".join(run.serial.lines(own=True)))
        if fatal:
            result["fatalSerialMarkers"] = fatal
            status = 1
    if run.ble_unavailable is not None:
        result["bleUnavailable"] = run.ble_unavailable
    result.update(steps=run.steps, passed=status == 0, finishedAt=utc_now())
    rendered = json.dumps(result, indent=2, sort_keys=True)
    (output_dir / "result.json").write_text(rendered + "\n", encoding="utf-8")
    print(rendered)
    return status


if __name__ == "__main__":
    raise SystemExit(main())

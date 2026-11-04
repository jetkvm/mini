"""Minimal W3C WebDriver client and headed Chromium/Xvfb launchers.

Talks to ChromeDriver's HTTP API directly; no Selenium or Playwright.
"""

from __future__ import annotations

import base64
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import time
from typing import Any, Mapping, Sequence
from urllib import error, request


DEFAULT_BROWSER = "/usr/bin/google-chrome-stable"
DEFAULT_CHROMEDRIVER = "/snap/chromium/current/usr/lib/chromium-browser/chromedriver"
DEFAULT_XVFB = "/usr/bin/Xvfb"


class WebDriverError(RuntimeError):
    pass


def _utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def resolve_executable(value: str, fallbacks: Sequence[str] = ()) -> str:
    candidates = (value, *fallbacks)
    for candidate in candidates:
        if not candidate:
            continue
        if os.path.sep in candidate:
            path = Path(candidate)
            if path.is_file() and os.access(path, os.X_OK):
                return str(path.absolute())
        else:
            resolved = shutil.which(candidate)
            if resolved:
                return resolved
    raise WebDriverError(f"executable not found: {', '.join(repr(item) for item in candidates)}")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def executable_evidence(path: str) -> dict[str, Any]:
    requested = Path(path).absolute()
    real = Path(os.path.realpath(requested))
    result: dict[str, Any] = {
        "path": str(requested),
        "realPath": str(real),
        "size": real.stat().st_size,
        "sha256": _sha256(real),
    }
    try:
        completed = subprocess.run(
            [str(requested), "--version"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=10,
        )
        result["versionOutput"] = completed.stdout.strip()
        result["versionExitCode"] = completed.returncode
    except Exception as exc:
        result["versionError"] = str(exc)

    # Google Chrome's public executable is a launcher script.  Hash the actual
    # ELF payload as well when it is next to that script.
    payload = real.parent / "chrome"
    if payload.is_file() and os.access(payload, os.X_OK):
        result["payload"] = {
            "path": str(payload),
            "size": payload.stat().st_size,
            "sha256": _sha256(payload),
        }
    return result


def _version_major(evidence: Mapping[str, Any]) -> int | None:
    match = re.search(r"\b(\d+)\.", str(evidence.get("versionOutput", "")))
    return int(match.group(1)) if match else None


def _terminate_process_group(process: subprocess.Popen[Any] | None, timeout: float = 5.0) -> None:
    if process is None or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=timeout)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                pass


class XvfbDisplay:
    """Own a dedicated headed X server for receiver-side Chrome rendering."""

    def __init__(
        self,
        output_path: Path,
        *,
        executable: str = DEFAULT_XVFB,
        display: str = ":99",
        screen: str = "1920x1080x24",
    ) -> None:
        if not re.fullmatch(r":\d+", display):
            raise WebDriverError(f"invalid X display {display!r}")
        self.output_path = Path(output_path)
        self.executable = resolve_executable(executable, ("Xvfb",))
        self.display = display
        self.screen = screen
        self.process: subprocess.Popen[Any] | None = None
        self._log = None

    def start(self, timeout: float = 10.0) -> None:
        if self.process is not None:
            raise WebDriverError("Xvfb is already started")
        display_number = self.display.removeprefix(":")
        socket_path = Path(f"/tmp/.X11-unix/X{display_number}")
        lock_path = Path(f"/tmp/.X{display_number}-lock")
        if socket_path.exists() or lock_path.exists():
            raise WebDriverError(
                f"X display {self.display} is already in use; choose --xvfb-display explicitly"
            )
        self.output_path.parent.mkdir(parents=True, exist_ok=True)
        self._log = self.output_path.open("wb")
        self.process = subprocess.Popen(
            [
                self.executable,
                self.display,
                "-screen",
                "0",
                self.screen,
                "-nolisten",
                "tcp",
                "-noreset",
            ],
            stdin=subprocess.DEVNULL,
            stdout=self._log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise WebDriverError(f"Xvfb exited with code {self.process.returncode}")
            if socket_path.exists():
                return
            time.sleep(0.05)
        self.stop()
        raise WebDriverError(f"Xvfb did not create {socket_path} within {timeout:g}s")

    def stop(self) -> None:
        _terminate_process_group(self.process)
        self.process = None
        if self._log is not None:
            self._log.close()
            self._log = None

    @property
    def evidence(self) -> dict[str, Any]:
        return {
            "headed": True,
            "display": self.display,
            "screen": self.screen,
            "executable": executable_evidence(self.executable),
            "logPath": str(self.output_path),
        }


class WebDriverHttpClient:
    """A small W3C WebDriver HTTP client suitable for ChromeDriver."""

    def __init__(self, base_url: str, *, timeout: float = 30.0) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout
        self.session_id: str | None = None
        self.capabilities: dict[str, Any] = {}

    def _request_raw(
        self,
        method: str,
        path: str,
        payload: Any | None = None,
        *,
        timeout: float | None = None,
    ) -> Any:
        data = None if payload is None else json.dumps(payload).encode("utf-8")
        req = request.Request(
            self.base_url + path,
            data=data,
            method=method,
            headers={"Content-Type": "application/json; charset=utf-8"},
        )
        body = b""
        try:
            with request.urlopen(req, timeout=timeout or self.timeout) as response:
                body = response.read()
        except error.HTTPError as exc:
            body = exc.read()
            detail = body.decode("utf-8", errors="replace")
            raise WebDriverError(f"WebDriver {method} {path} returned HTTP {exc.code}: {detail}") from exc
        except (error.URLError, TimeoutError, OSError) as exc:
            raise WebDriverError(f"WebDriver {method} {path} failed: {exc}") from exc
        if not body:
            return None
        try:
            result = json.loads(body)
        except json.JSONDecodeError as exc:
            raise WebDriverError(
                f"WebDriver {method} {path} returned invalid JSON: "
                f"{body[:500].decode('utf-8', errors='replace')}"
            ) from exc
        # WebDriver errors come as non-2xx (HTTPError above); a script result
        # may itself contain an ``error`` field.
        return result

    @staticmethod
    def _value(result: Any) -> Any:
        if isinstance(result, Mapping) and "value" in result:
            return result["value"]
        return result

    def status(self) -> Any:
        return self._value(self._request_raw("GET", "/status", timeout=2.0))

    def _create_session_with_capabilities(
        self, always_match: Mapping[str, Any]
    ) -> dict[str, Any]:
        if self.session_id is not None:
            raise WebDriverError("WebDriver session has already been created")
        payload = {"capabilities": {"alwaysMatch": dict(always_match)}}
        result = self._request_raw("POST", "/session", payload, timeout=60.0)
        value = self._value(result)
        if not isinstance(value, Mapping):
            raise WebDriverError(f"new session returned unexpected payload: {result!r}")
        session_id = value.get("sessionId")
        if not session_id and isinstance(result, Mapping):
            session_id = result.get("sessionId")
        if not isinstance(session_id, str) or not session_id:
            raise WebDriverError(f"new session did not return a session id: {result!r}")
        capabilities = value.get("capabilities", {})
        self.session_id = session_id
        self.capabilities = dict(capabilities) if isinstance(capabilities, Mapping) else {}
        self.set_timeouts(script_ms=30_000, page_load_ms=60_000)
        return {"sessionId": session_id, "capabilities": self.capabilities}

    def create_session(self, browser_binary: str, chrome_args: Sequence[str]) -> dict[str, Any]:
        return self._create_session_with_capabilities(
            {
                "browserName": "chrome",
                "acceptInsecureCerts": True,
                "goog:chromeOptions": {
                    "binary": browser_binary,
                    "args": list(chrome_args),
                },
            }
        )

    def _session_path(self, suffix: str) -> str:
        if not self.session_id:
            raise WebDriverError("WebDriver session has not been created")
        return f"/session/{self.session_id}{suffix}"

    def set_timeouts(self, *, script_ms: int, page_load_ms: int) -> None:
        self._request_raw(
            "POST",
            self._session_path("/timeouts"),
            {"script": script_ms, "pageLoad": page_load_ms, "implicit": 0},
        )

    def navigate(self, url: str) -> None:
        self._request_raw("POST", self._session_path("/url"), {"url": url}, timeout=75.0)

    def execute_sync(self, script: str, args: Sequence[Any] = ()) -> Any:
        result = self._request_raw(
            "POST", self._session_path("/execute/sync"), {"script": script, "args": list(args)}
        )
        return self._value(result)

    def execute_async(self, script: str, args: Sequence[Any] = ()) -> Any:
        result = self._request_raw(
            "POST",
            self._session_path("/execute/async"),
            {"script": script, "args": list(args)},
            timeout=35.0,
        )
        return self._value(result)

    def screenshot(self, path: Path) -> None:
        result = self._request_raw("GET", self._session_path("/screenshot"))
        encoded = self._value(result)
        if not isinstance(encoded, str):
            raise WebDriverError("WebDriver screenshot did not return base64 data")
        try:
            image = base64.b64decode(encoded, validate=True)
        except (ValueError, base64.binascii.Error) as exc:
            raise WebDriverError(f"WebDriver screenshot returned invalid base64: {exc}") from exc
        if not image.startswith(b"\x89PNG\r\n\x1a\n"):
            raise WebDriverError("WebDriver screenshot is not a PNG")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(image)

    def quit(self) -> None:
        if not self.session_id:
            return
        session_id = self.session_id
        self.session_id = None
        try:
            self._request_raw("DELETE", f"/session/{session_id}", timeout=10.0)
        except WebDriverError:
            pass


class ChromiumWebDriver:
    """Launch ChromeDriver and a headed Google Chrome on the chosen X display."""

    def __init__(
        self,
        output_path: Path,
        *,
        browser: str = DEFAULT_BROWSER,
        chromedriver: str = DEFAULT_CHROMEDRIVER,
        display: str = ":99",
        extra_chrome_args: Sequence[str] = (),
    ) -> None:
        self.output_path = Path(output_path)
        self.browser = resolve_executable(browser, ("google-chrome-stable", "google-chrome"))
        self.chromedriver = resolve_executable(
            chromedriver, ("chromium.chromedriver", "chromedriver")
        )
        self.display = display
        self.extra_chrome_args = list(extra_chrome_args)
        self.process: subprocess.Popen[Any] | None = None
        self._log = None
        self.client: WebDriverHttpClient | None = None
        self.chrome_args: list[str] = []
        self.browser_evidence = executable_evidence(self.browser)
        self.driver_evidence = executable_evidence(self.chromedriver)

    @staticmethod
    def _free_port() -> int:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.bind(("127.0.0.1", 0))
            return int(sock.getsockname()[1])

    def start(self, timeout: float = 20.0) -> WebDriverHttpClient:
        browser_major = _version_major(self.browser_evidence)
        driver_major = _version_major(self.driver_evidence)
        if browser_major is None or driver_major is None or browser_major != driver_major:
            raise WebDriverError(
                "Chrome and ChromeDriver major versions must match: "
                f"browser={self.browser_evidence.get('versionOutput')!r}, "
                f"driver={self.driver_evidence.get('versionOutput')!r}"
            )

        port = self._free_port()
        self.output_path.parent.mkdir(parents=True, exist_ok=True)
        self._log = self.output_path.open("wb")
        environment = dict(os.environ)
        environment["DISPLAY"] = self.display
        self.process = subprocess.Popen(
            [self.chromedriver, f"--port={port}", "--allowed-ips=127.0.0.1"],
            stdin=subprocess.DEVNULL,
            stdout=self._log,
            stderr=subprocess.STDOUT,
            env=environment,
            start_new_session=True,
        )
        client = WebDriverHttpClient(f"http://127.0.0.1:{port}")
        deadline = time.monotonic() + timeout
        last_error: str | None = None
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise WebDriverError(f"ChromeDriver exited with code {self.process.returncode}")
            try:
                status = client.status()
                if not isinstance(status, Mapping) or status.get("ready", True):
                    break
            except WebDriverError as exc:
                last_error = str(exc)
            time.sleep(0.1)
        else:
            self.stop()
            raise WebDriverError(f"ChromeDriver was not ready within {timeout:g}s: {last_error}")

        chrome_args = [
            "--no-first-run",
            "--no-default-browser-check",
            "--autoplay-policy=no-user-gesture-required",
            "--disable-background-timer-throttling",
            "--disable-backgrounding-occluded-windows",
            "--disable-renderer-backgrounding",
            "--disable-features=WebRtcHideLocalIpsWithMdns",
            "--force-webrtc-ip-handling-policy=default",
            "--window-size=1920,1080",
            "--start-maximized",
            *self.extra_chrome_args,
        ]
        self.chrome_args = chrome_args
        try:
            client.create_session(self.browser, chrome_args)
        except Exception:
            self.stop()
            raise
        self.client = client
        return client

    def stop(self) -> None:
        if self.client is not None:
            self.client.quit()
            self.client = None
        _terminate_process_group(self.process)
        self.process = None
        if self._log is not None:
            self._log.close()
            self._log = None

    @property
    def evidence(self) -> dict[str, Any]:
        return {
            "capturedAt": _utc_now(),
            "headed": True,
            "display": self.display,
            "browser": self.browser_evidence,
            "chromedriver": self.driver_evidence,
            "chromeArguments": self.chrome_args,
            "capabilities": self.client.capabilities if self.client else {},
            "logPath": str(self.output_path),
        }

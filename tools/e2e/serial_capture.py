"""Concurrent, read-only serial log capture using only the Python stdlib."""

from __future__ import annotations

import codecs
from datetime import datetime, timezone
import errno
import fcntl
import os
from pathlib import Path
import select
import termios
import threading
from typing import Any


class SerialCaptureError(RuntimeError):
    pass


def _utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


class SerialCapture:
    """Capture a serial stream without ever writing to the device.

    The tty configuration is restored on normal shutdown.  An advisory lock is
    held for the run so a second cooperative monitor cannot silently split the
    evidence stream.
    """

    def __init__(self, device: str, output_path: Path, baud: int = 115200) -> None:
        self.device = device
        self.output_path = Path(output_path)
        self.baud = baud
        self._fd: int | None = None
        self._original_attributes: list[Any] | None = None
        self._output = None
        self._thread: threading.Thread | None = None
        self._stop_event = threading.Event()
        self._decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        self._pending_text = ""
        self._lock = threading.Lock()
        self._bytes_read = 0
        self._line_count = 0
        self._error: str | None = None
        self._started = False
        self._stopped = False

    @staticmethod
    def _baud_constant(baud: int) -> int:
        name = f"B{baud}"
        if not hasattr(termios, name):
            raise SerialCaptureError(f"unsupported baud rate: {baud}")
        return int(getattr(termios, name))

    def start(self) -> None:
        if self._started:
            raise SerialCaptureError("serial capture already started")
        self.output_path.parent.mkdir(parents=True, exist_ok=True)
        self._output = self.output_path.open("w", encoding="utf-8", buffering=1)
        try:
            flags = os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK
            if hasattr(os, "O_CLOEXEC"):
                flags |= os.O_CLOEXEC
            self._fd = os.open(self.device, flags)
            fcntl.flock(self._fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self._original_attributes = termios.tcgetattr(self._fd)
            attributes = list(self._original_attributes)
            attributes[0] = termios.IGNPAR
            attributes[1] = 0
            attributes[2] = termios.CLOCAL | termios.CREAD | termios.CS8
            attributes[3] = 0
            speed = self._baud_constant(self.baud)
            attributes[4] = speed
            attributes[5] = speed
            control_chars = list(attributes[6])
            control_chars[termios.VMIN] = 0
            control_chars[termios.VTIME] = 0
            attributes[6] = control_chars
            termios.tcsetattr(self._fd, termios.TCSANOW, attributes)
        except Exception as exc:
            self._cleanup_open_failure()
            raise SerialCaptureError(f"cannot open serial device {self.device}: {exc}") from exc

        self._started = True
        self._write_host_line(f"capture started device={self.device} baud={self.baud}")
        self._thread = threading.Thread(target=self._reader, name="serial-capture", daemon=True)
        self._thread.start()

    def _cleanup_open_failure(self) -> None:
        if self._fd is not None:
            try:
                fcntl.flock(self._fd, fcntl.LOCK_UN)
            except OSError:
                pass
            try:
                os.close(self._fd)
            except OSError:
                pass
            self._fd = None
        if self._output is not None:
            self._output.close()
            self._output = None

    def _write_host_line(self, message: str) -> None:
        if self._output is None:
            return
        with self._lock:
            self._output.write(f"{_utc_timestamp()} [host] {message}\n")
            self._output.flush()

    def _write_device_line(self, line: str) -> None:
        if self._output is None:
            return
        with self._lock:
            self._output.write(f"{_utc_timestamp()} {line.rstrip(chr(13))}\n")
            self._output.flush()
            self._line_count += 1

    def _consume(self, data: bytes, *, final: bool = False) -> None:
        text = self._decoder.decode(data, final=final)
        self._pending_text += text.replace("\r\n", "\n").replace("\r", "\n")
        while "\n" in self._pending_text:
            line, self._pending_text = self._pending_text.split("\n", 1)
            self._write_device_line(line)
        if final and self._pending_text:
            self._write_device_line(self._pending_text)
            self._pending_text = ""

    def _reader(self) -> None:
        assert self._fd is not None
        while not self._stop_event.is_set():
            try:
                readable, _, _ = select.select([self._fd], [], [], 0.25)
                if not readable:
                    continue
                data = os.read(self._fd, 8192)
                if not data:
                    self._error = "serial device returned EOF"
                    self._write_host_line(self._error)
                    break
                self._bytes_read += len(data)
                self._consume(data)
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK, errno.EINTR):
                    continue
                self._error = f"serial read failed: {exc}"
                self._write_host_line(self._error)
                break
            except Exception as exc:  # evidence must preserve unexpected reader failures
                self._error = f"serial reader failed: {exc}"
                self._write_host_line(self._error)
                break

    def stop(self) -> None:
        if not self._started or self._stopped:
            return
        self._stop_event.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
            if self._thread.is_alive() and self._error is None:
                self._error = "serial reader did not stop within 2 seconds"
        self._consume(b"", final=True)
        self._write_host_line(
            f"capture stopped bytes={self._bytes_read} lines={self._line_count} "
            f"error={self._error!r}"
        )

        if self._fd is not None:
            if self._original_attributes is not None:
                try:
                    termios.tcsetattr(self._fd, termios.TCSANOW, self._original_attributes)
                except (OSError, termios.error) as exc:
                    if self._error is None:
                        self._error = f"could not restore tty attributes: {exc}"
            try:
                fcntl.flock(self._fd, fcntl.LOCK_UN)
            except OSError:
                pass
            try:
                os.close(self._fd)
            except OSError:
                pass
            self._fd = None
        if self._output is not None:
            self._output.close()
            self._output = None
        self._stopped = True

    @property
    def status(self) -> dict[str, Any]:
        return {
            "device": self.device,
            "baud": self.baud,
            "path": str(self.output_path),
            "started": self._started,
            "stopped": self._stopped,
            "bytesRead": self._bytes_read,
            "lineCount": self._line_count,
            "error": self._error,
        }

    def __enter__(self) -> "SerialCapture":
        self.start()
        return self

    def __exit__(self, _type: Any, _value: Any, _traceback: Any) -> None:
        self.stop()

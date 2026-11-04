#!/usr/bin/env python3
"""Raw transport probe for the IP-KVM OS Services USB interface.

Runs on the computer the KVM's USB port is plugged into, with nothing else
using the interface, and talks to it as a sender would before its first
message (OS Services protocol version 5, "Session reset" section):

- the interface (class ff, subclass 4a, protocol 01) has one bulk IN and one
  bulk OUT endpoint;
- RESET answers magic "JKV5", phase 0 (resetting), zero reserved bytes and a
  non-zero 16-byte token, a new one for each RESET;
- STATUS, polled after each RESET, carries that RESET's token and reaches
  phase 1 (ready) within 15 seconds without passing through phase 2 or 3;
- any other request on the interface stalls: an unknown bRequest, a RESET for
  another protocol major version and a STATUS with another wLength;
- a stalled request leaves the admitted session as it was.

The probe sends no bulk data, so the KVM never sees a session.hello; it ends
the admitted session after its 15-second handshake timeout, and the next
sender starts over with its own RESET.

Usage:
  sudo python3 os_services_usb_probe.py [--bus N --address N] [--resets N] [--json]

Access: run it as root (sudo), or as the user of the active local desktop
session when the udev rule 70-ipkvm-os-services.rules that the OS Services
packages install is in place (it tags the KVM with uaccess). The sender
claims the interface while it runs, so stop it first:
  systemctl --user stop ipkvm-os-services.service

It needs python3 and libusb-1.0 (Debian and Ubuntu: libusb-1.0-0), nothing
from pip. With --json the report goes to stdout as one JSON object; the
human-readable lines always go to stderr. Exit status: 0 when every check
passed, 1 when a check failed, 2 when the probe could not run (no libusb, no
KVM, no access, interface busy).
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
from dataclasses import dataclass, field
import json
import sys
import time
from typing import Callable, Protocol, Sequence

INTERFACE_CLASS = 0xFF
INTERFACE_SUBCLASS = 0x4A
INTERFACE_PROTOCOL = 0x01

REQUEST_TYPE = 0xC1  # device to host, vendor, interface
REQUEST_RESET = 0x40
REQUEST_STATUS = 0x41
PROTOCOL_MAJOR = 5
STATUS_LEN = 24
MAGIC = b"JKV5"
TOKEN_LEN = 16

PHASE_RESETTING = 0
PHASE_READY = 1
PHASE_ACTIVE = 2
PHASE_FAULTED = 3
PHASE_NAMES = {
    PHASE_RESETTING: "resetting",
    PHASE_READY: "ready",
    PHASE_ACTIVE: "active",
    PHASE_FAULTED: "faulted",
}

# The OS Services protocol: each control transfer has a 2-second timeout,
# and the sender polls STATUS for at most 15 seconds.
CONTROL_TIMEOUT_MS = 2000
READY_TIMEOUT_S = 15.0
POLL_INTERVAL_S = 0.005

# Requests the KVM must stall: (what, bRequest, wValue, wLength).
STALL_CASES: tuple[tuple[str, int, int, int], ...] = (
    ("unknown bRequest 0x42", 0x42, PROTOCOL_MAJOR, STATUS_LEN),
    ("RESET for protocol major 4", REQUEST_RESET, PROTOCOL_MAJOR - 1, STATUS_LEN),
    ("STATUS with wLength 16", REQUEST_STATUS, PROTOCOL_MAJOR, 16),
)

USB_ENDPOINT_IN = 0x80
USB_TRANSFER_TYPE_MASK = 0x03
USB_TRANSFER_TYPE_BULK = 0x02


class ProbeUnavailable(RuntimeError):
    """The probe cannot run here: exit status 2."""


class ProtocolError(ValueError):
    """An answer that breaks the OS Services protocol."""


class Stall(Exception):
    """The device stalled the control request."""


class TransportError(Exception):
    """The control request failed in another way: a timeout, a disconnect."""


@dataclass(frozen=True)
class Status:
    phase: int
    token: bytes

    @property
    def phase_name(self) -> str:
        return PHASE_NAMES.get(self.phase, str(self.phase))


def parse_status(data: bytes) -> Status:
    """Checks one RESET or STATUS answer and returns its phase and token."""
    if len(data) != STATUS_LEN:
        raise ProtocolError(f"answer has {len(data)} bytes, not {STATUS_LEN}")
    if data[0:4] != MAGIC:
        raise ProtocolError(f"magic is {data[0:4]!r}, not {MAGIC!r}")
    phase = data[4]
    if phase not in PHASE_NAMES:
        raise ProtocolError(f"phase {phase} is not 0 to 3")
    if any(data[5:8]):
        raise ProtocolError(f"reserved bytes are {data[5:8].hex()}, not zero")
    token = bytes(data[8:24])
    if not any(token):
        raise ProtocolError("the session token is all zeros")
    return Status(phase=phase, token=token)


@dataclass(frozen=True)
class Endpoint:
    address: int
    attributes: int
    max_packet_size: int

    @property
    def is_in(self) -> bool:
        return bool(self.address & USB_ENDPOINT_IN)

    @property
    def is_bulk(self) -> bool:
        return self.attributes & USB_TRANSFER_TYPE_MASK == USB_TRANSFER_TYPE_BULK


@dataclass(frozen=True)
class Candidate:
    bus: int
    address: int
    vendor_id: int
    product_id: int
    interface: int
    endpoints: tuple[Endpoint, ...]


def is_os_services_interface(cls: int, subclass: int, protocol: int) -> bool:
    return (cls, subclass, protocol) == (
        INTERFACE_CLASS,
        INTERFACE_SUBCLASS,
        INTERFACE_PROTOCOL,
    )


def endpoint_problem(endpoints: Sequence[Endpoint]) -> str | None:
    """Why the endpoints are not one bulk IN and one bulk OUT, or None."""
    if len(endpoints) != 2:
        return f"{len(endpoints)} endpoints, not 2"
    if not all(endpoint.is_bulk for endpoint in endpoints):
        return "an endpoint is not bulk"
    if sorted(endpoint.is_in for endpoint in endpoints) != [False, True]:
        return "the endpoints are not one IN and one OUT"
    return None


def select_candidate(
    candidates: Sequence[Candidate], bus: int | None, address: int | None
) -> Candidate:
    """The one KVM to probe; --bus and --address pick one of several."""
    chosen = [
        c
        for c in candidates
        if (bus is None or c.bus == bus) and (address is None or c.address == address)
    ]
    if not chosen:
        where = "" if bus is None and address is None else f" at bus {bus} address {address}"
        raise ProbeUnavailable(f"no USB device with an OS Services interface{where}")
    if len(chosen) > 1:
        listed = ", ".join(f"{c.bus}:{c.address}" for c in chosen)
        raise ProbeUnavailable(
            f"several OS Services devices ({listed}); pick one with --bus and --address"
        )
    return chosen[0]


class Transport(Protocol):
    def control_in(self, request: int, value: int, length: int) -> bytes:
        """One IN control request on the interface; raises Stall or
        TransportError."""
        ...


@dataclass
class Check:
    name: str
    ok: bool
    detail: str = ""


@dataclass
class ResetRecord:
    token: str
    reset_phase: str
    ready_ms: float | None = None
    polls: int = 0
    phases_seen: list[str] = field(default_factory=list)


@dataclass
class Report:
    checks: list[Check] = field(default_factory=list)
    resets: list[ResetRecord] = field(default_factory=list)
    control_rtt_ms: list[float] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return bool(self.checks) and all(check.ok for check in self.checks)

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        self.checks.append(Check(name, ok, detail))
        return ok

    def to_json(self) -> dict:
        return {
            "ok": self.ok,
            "checks": [vars(check) for check in self.checks],
            "resets": [vars(record) for record in self.resets],
            "control_rtt_ms": self.control_rtt_ms,
        }


class Prober:
    """Runs the checks against a transport. clock and sleep are injectable so
    the tests can run the polling without waiting."""

    def __init__(
        self,
        transport: Transport,
        clock: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], None] = time.sleep,
        ready_timeout_s: float = READY_TIMEOUT_S,
        poll_interval_s: float = POLL_INTERVAL_S,
    ) -> None:
        self.transport = transport
        self.clock = clock
        self.sleep = sleep
        self.ready_timeout_s = ready_timeout_s
        self.poll_interval_s = poll_interval_s
        self.report = Report()

    def _request(self, request: int, value: int = PROTOCOL_MAJOR, length: int = STATUS_LEN) -> bytes:
        started = self.clock()
        try:
            return self.transport.control_in(request, value, length)
        finally:
            self.report.control_rtt_ms.append(round((self.clock() - started) * 1000.0, 3))

    def _answer(self, what: str, request: int) -> Status | None:
        try:
            return parse_status(self._request(request))
        except Stall:
            self.report.check(f"{what} is answered", False, "the KVM stalled it")
        except ProtocolError as error:
            self.report.check(f"{what} is a valid answer", False, str(error))
        except TransportError as error:
            self.report.check(f"{what} is answered", False, str(error))
        return None

    def reset_once(self, index: int, earlier: Sequence[bytes]) -> bytes | None:
        what = f"RESET {index + 1}"
        started = self.clock()
        status = self._answer(what, REQUEST_RESET)
        if status is None:
            return None
        record = ResetRecord(token=status.token.hex(), reset_phase=status.phase_name)
        self.report.resets.append(record)
        self.report.check(
            f"{what} answers phase 0 (resetting)",
            status.phase == PHASE_RESETTING,
            f"phase {status.phase} ({status.phase_name})",
        )
        self.report.check(
            f"{what} makes a new token",
            status.token not in earlier,
            "" if status.token not in earlier else f"token {status.token.hex()} was used before",
        )
        if status.phase == PHASE_FAULTED:
            return None
        if self._poll_ready(what, status.token, started, record):
            return status.token
        return None

    def _poll_ready(self, what: str, token: bytes, started: float, record: ResetRecord) -> bool:
        while True:
            status = self._answer(f"STATUS after {what}", REQUEST_STATUS)
            if status is None:
                return False
            record.polls += 1
            if not record.phases_seen or record.phases_seen[-1] != status.phase_name:
                record.phases_seen.append(status.phase_name)
            if status.token != token:
                self.report.check(
                    f"STATUS after {what} carries its token",
                    False,
                    f"token {status.token.hex()}, RESET gave {token.hex()}: another RESET replaced it",
                )
                return False
            if status.phase == PHASE_READY:
                record.ready_ms = round((self.clock() - started) * 1000.0, 3)
                self.report.check(
                    f"STATUS after {what} carries its token and reaches phase 1 (ready)",
                    True,
                    f"after {record.polls} polls, {record.ready_ms} ms",
                )
                return True
            if status.phase in (PHASE_ACTIVE, PHASE_FAULTED):
                self.report.check(
                    f"STATUS after {what} reaches phase 1 (ready)",
                    False,
                    f"phase {status.phase} ({status.phase_name})",
                )
                return False
            if self.clock() - started > self.ready_timeout_s:
                self.report.check(
                    f"STATUS after {what} reaches phase 1 (ready)",
                    False,
                    f"still {status.phase_name} after {self.ready_timeout_s} s",
                )
                return False
            self.sleep(self.poll_interval_s)

    def check_stalls(self, token: bytes) -> None:
        for what, request, value, length in STALL_CASES:
            try:
                data = self._request(request, value, length)
            except Stall:
                self.report.check(f"{what} stalls", True)
            except TransportError as error:
                self.report.check(f"{what} stalls", False, str(error))
            else:
                self.report.check(f"{what} stalls", False, f"answered {bytes(data).hex()}")
        status = self._answer("STATUS after the stalled requests", REQUEST_STATUS)
        if status is not None:
            self.report.check(
                "a stalled request leaves the session ready with the same token",
                status.phase == PHASE_READY and status.token == token,
                f"phase {status.phase_name}, token "
                f"{'unchanged' if status.token == token else status.token.hex()}",
            )

    def run(self, resets: int) -> Report:
        tokens: list[bytes] = []
        for index in range(resets):
            token = self.reset_once(index, tokens)
            if token is None:
                return self.report
            tokens.append(token)
        self.check_stalls(tokens[-1])
        return self.report


# --- libusb-1.0 through ctypes ---------------------------------------------

LIBUSB_ERROR_PIPE = -9
LIBUSB_ERROR_BUSY = -6
LIBUSB_ERROR_ACCESS = -3
LIBUSB_ERROR_NOT_FOUND = -5


class _DeviceDescriptor(ctypes.Structure):
    _fields_ = [
        ("bLength", ctypes.c_uint8),
        ("bDescriptorType", ctypes.c_uint8),
        ("bcdUSB", ctypes.c_uint16),
        ("bDeviceClass", ctypes.c_uint8),
        ("bDeviceSubClass", ctypes.c_uint8),
        ("bDeviceProtocol", ctypes.c_uint8),
        ("bMaxPacketSize0", ctypes.c_uint8),
        ("idVendor", ctypes.c_uint16),
        ("idProduct", ctypes.c_uint16),
        ("bcdDevice", ctypes.c_uint16),
        ("iManufacturer", ctypes.c_uint8),
        ("iProduct", ctypes.c_uint8),
        ("iSerialNumber", ctypes.c_uint8),
        ("bNumConfigurations", ctypes.c_uint8),
    ]


class _EndpointDescriptor(ctypes.Structure):
    _fields_ = [
        ("bLength", ctypes.c_uint8),
        ("bDescriptorType", ctypes.c_uint8),
        ("bEndpointAddress", ctypes.c_uint8),
        ("bmAttributes", ctypes.c_uint8),
        ("wMaxPacketSize", ctypes.c_uint16),
        ("bInterval", ctypes.c_uint8),
        ("bRefresh", ctypes.c_uint8),
        ("bSynchAddress", ctypes.c_uint8),
        ("extra", ctypes.c_void_p),
        ("extra_length", ctypes.c_int),
    ]


class _InterfaceDescriptor(ctypes.Structure):
    _fields_ = [
        ("bLength", ctypes.c_uint8),
        ("bDescriptorType", ctypes.c_uint8),
        ("bInterfaceNumber", ctypes.c_uint8),
        ("bAlternateSetting", ctypes.c_uint8),
        ("bNumEndpoints", ctypes.c_uint8),
        ("bInterfaceClass", ctypes.c_uint8),
        ("bInterfaceSubClass", ctypes.c_uint8),
        ("bInterfaceProtocol", ctypes.c_uint8),
        ("iInterface", ctypes.c_uint8),
        ("endpoint", ctypes.POINTER(_EndpointDescriptor)),
        ("extra", ctypes.c_void_p),
        ("extra_length", ctypes.c_int),
    ]


class _Interface(ctypes.Structure):
    _fields_ = [
        ("altsetting", ctypes.POINTER(_InterfaceDescriptor)),
        ("num_altsetting", ctypes.c_int),
    ]


class _ConfigDescriptor(ctypes.Structure):
    _fields_ = [
        ("bLength", ctypes.c_uint8),
        ("bDescriptorType", ctypes.c_uint8),
        ("wTotalLength", ctypes.c_uint16),
        ("bNumInterfaces", ctypes.c_uint8),
        ("bConfigurationValue", ctypes.c_uint8),
        ("iConfiguration", ctypes.c_uint8),
        ("bmAttributes", ctypes.c_uint8),
        ("MaxPower", ctypes.c_uint8),
        ("interface", ctypes.POINTER(_Interface)),
        ("extra", ctypes.c_void_p),
        ("extra_length", ctypes.c_int),
    ]


def _load_libusb() -> ctypes.CDLL:
    # macOS: Homebrew's libusb, which the system Python does not search.
    names = [
        ctypes.util.find_library("usb-1.0"),
        "libusb-1.0.so.0",
        "libusb-1.0.so",
        "/opt/homebrew/lib/libusb-1.0.dylib",
        "/usr/local/lib/libusb-1.0.dylib",
    ]
    for name in names:
        if not name:
            continue
        try:
            lib = ctypes.CDLL(name)
        except OSError:
            continue
        _declare(lib)
        return lib
    raise ProbeUnavailable("libusb-1.0 not found; install libusb-1.0-0")


def _declare(lib: ctypes.CDLL) -> None:
    vp, pp = ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)
    lib.libusb_init.argtypes = [pp]
    lib.libusb_exit.argtypes = [vp]
    lib.libusb_exit.restype = None
    lib.libusb_get_device_list.argtypes = [vp, ctypes.POINTER(pp)]
    lib.libusb_get_device_list.restype = ctypes.c_ssize_t
    lib.libusb_free_device_list.argtypes = [pp, ctypes.c_int]
    lib.libusb_free_device_list.restype = None
    lib.libusb_get_bus_number.argtypes = [vp]
    lib.libusb_get_bus_number.restype = ctypes.c_uint8
    lib.libusb_get_device_address.argtypes = [vp]
    lib.libusb_get_device_address.restype = ctypes.c_uint8
    lib.libusb_get_device_descriptor.argtypes = [vp, ctypes.POINTER(_DeviceDescriptor)]
    lib.libusb_get_active_config_descriptor.argtypes = [
        vp,
        ctypes.POINTER(ctypes.POINTER(_ConfigDescriptor)),
    ]
    lib.libusb_free_config_descriptor.argtypes = [ctypes.POINTER(_ConfigDescriptor)]
    lib.libusb_free_config_descriptor.restype = None
    lib.libusb_open.argtypes = [vp, pp]
    lib.libusb_close.argtypes = [vp]
    lib.libusb_close.restype = None
    lib.libusb_kernel_driver_active.argtypes = [vp, ctypes.c_int]
    lib.libusb_claim_interface.argtypes = [vp, ctypes.c_int]
    lib.libusb_release_interface.argtypes = [vp, ctypes.c_int]
    lib.libusb_control_transfer.argtypes = [
        vp,
        ctypes.c_uint8,
        ctypes.c_uint8,
        ctypes.c_uint16,
        ctypes.c_uint16,
        ctypes.c_void_p,
        ctypes.c_uint16,
        ctypes.c_uint,
    ]
    lib.libusb_error_name.argtypes = [ctypes.c_int]
    lib.libusb_error_name.restype = ctypes.c_char_p


def _error_name(lib: ctypes.CDLL, code: int) -> str:
    return (lib.libusb_error_name(code) or b"").decode() or str(code)


def _candidates(lib: ctypes.CDLL, devices: ctypes.Array) -> list[tuple[Candidate, int]]:
    found: list[tuple[Candidate, int]] = []
    index = 0
    while devices[index]:
        device = devices[index]
        index += 1
        descriptor = _DeviceDescriptor()
        if lib.libusb_get_device_descriptor(device, ctypes.byref(descriptor)) != 0:
            continue
        config = ctypes.POINTER(_ConfigDescriptor)()
        if lib.libusb_get_active_config_descriptor(device, ctypes.byref(config)) != 0:
            continue
        try:
            for i in range(config.contents.bNumInterfaces):
                interface = config.contents.interface[i]
                for a in range(interface.num_altsetting):
                    alt = interface.altsetting[a]
                    if not is_os_services_interface(
                        alt.bInterfaceClass, alt.bInterfaceSubClass, alt.bInterfaceProtocol
                    ):
                        continue
                    endpoints = tuple(
                        Endpoint(
                            alt.endpoint[e].bEndpointAddress,
                            alt.endpoint[e].bmAttributes,
                            alt.endpoint[e].wMaxPacketSize,
                        )
                        for e in range(alt.bNumEndpoints)
                    )
                    candidate = Candidate(
                        bus=lib.libusb_get_bus_number(device),
                        address=lib.libusb_get_device_address(device),
                        vendor_id=descriptor.idVendor,
                        product_id=descriptor.idProduct,
                        interface=alt.bInterfaceNumber,
                        endpoints=endpoints,
                    )
                    found.append((candidate, index - 1))
        finally:
            lib.libusb_free_config_descriptor(config)
    return found


class LibUsbTransport:
    def __init__(self, lib: ctypes.CDLL, handle: ctypes.c_void_p, interface: int) -> None:
        self.lib = lib
        self.handle = handle
        self.interface = interface

    def control_in(self, request: int, value: int, length: int) -> bytes:
        buffer = ctypes.create_string_buffer(max(length, 1))
        result = self.lib.libusb_control_transfer(
            self.handle,
            REQUEST_TYPE,
            request,
            value,
            self.interface,
            buffer,
            length,
            CONTROL_TIMEOUT_MS,
        )
        if result == LIBUSB_ERROR_PIPE:
            raise Stall()
        if result < 0:
            raise TransportError(
                f"control request 0x{request:02x}: {_error_name(self.lib, result)}"
            )
        return buffer.raw[:result]


def probe_device(
    bus: int | None, address: int | None, resets: int
) -> tuple[Candidate, Report]:
    lib = _load_libusb()
    context = ctypes.c_void_p()
    if lib.libusb_init(ctypes.byref(context)) != 0:
        raise ProbeUnavailable("libusb_init failed")
    devices = ctypes.POINTER(ctypes.c_void_p)()
    try:
        if lib.libusb_get_device_list(context, ctypes.byref(devices)) < 0:
            raise ProbeUnavailable("libusb_get_device_list failed")
        try:
            found = _candidates(lib, devices)
            candidate = select_candidate([c for c, _ in found], bus, address)
            device = devices[[i for c, i in found if c == candidate][0]]
            return candidate, _probe_open(lib, device, candidate, resets)
        finally:
            lib.libusb_free_device_list(devices, 1)
    finally:
        lib.libusb_exit(context)


def _probe_open(lib: ctypes.CDLL, device: int, candidate: Candidate, resets: int) -> Report:
    handle = ctypes.c_void_p()
    result = lib.libusb_open(device, ctypes.byref(handle))
    if result == LIBUSB_ERROR_ACCESS:
        raise ProbeUnavailable(
            "no access to the KVM: run with sudo, or install the udev rule "
            "70-ipkvm-os-services.rules and run as the active desktop user"
        )
    if result != 0:
        raise ProbeUnavailable(f"libusb_open: {_error_name(lib, result)}")
    try:
        if lib.libusb_kernel_driver_active(handle, candidate.interface) == 1:
            raise ProbeUnavailable(
                f"a kernel driver is bound to interface {candidate.interface}"
            )
        result = lib.libusb_claim_interface(handle, candidate.interface)
        if result == LIBUSB_ERROR_BUSY:
            raise ProbeUnavailable(
                "the interface is in use, probably by the sender: "
                "systemctl --user stop ipkvm-os-services.service"
            )
        if result != 0:
            raise ProbeUnavailable(f"libusb_claim_interface: {_error_name(lib, result)}")
        try:
            transport = LibUsbTransport(lib, handle, candidate.interface)
            return Prober(transport).run(resets)
        finally:
            lib.libusb_release_interface(handle, candidate.interface)
    finally:
        lib.libusb_close(handle)


def describe(candidate: Candidate) -> dict:
    return {
        "bus": candidate.bus,
        "address": candidate.address,
        "id": f"{candidate.vendor_id:04x}:{candidate.product_id:04x}",
        "interface": candidate.interface,
        "endpoints": [
            {
                "address": f"0x{e.address:02x}",
                "direction": "in" if e.is_in else "out",
                "bulk": e.is_bulk,
                "max_packet_size": e.max_packet_size,
            }
            for e in candidate.endpoints
        ],
    }


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--bus", type=int, help="USB bus number of the KVM")
    parser.add_argument("--address", type=int, help="USB device address of the KVM")
    parser.add_argument(
        "--resets", type=int, default=3, help="RESETs to send, each polled to ready (default 3)"
    )
    parser.add_argument("--json", action="store_true", help="print the report as JSON on stdout")
    args = parser.parse_args(argv)
    if args.resets < 2:
        parser.error("--resets must be at least 2, to compare tokens")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        candidate, report = probe_device(args.bus, args.address, args.resets)
    except ProbeUnavailable as error:
        print(f"os_services_usb_probe: {error}", file=sys.stderr)
        if args.json:
            print(json.dumps({"ok": False, "unavailable": str(error)}))
        return 2
    problem = endpoint_problem(candidate.endpoints)
    report.checks.insert(
        0,
        Check(
            "the interface has one bulk IN and one bulk OUT endpoint",
            problem is None,
            problem or "",
        ),
    )
    for check in report.checks:
        detail = f": {check.detail}" if check.detail else ""
        print(f"{'PASS' if check.ok else 'FAIL'} {check.name}{detail}", file=sys.stderr)
    if args.json:
        print(json.dumps({**report.to_json(), "device": describe(candidate)}))
    return 0 if report.ok else 1


if __name__ == "__main__":
    sys.exit(main())

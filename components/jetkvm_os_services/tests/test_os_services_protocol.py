#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026
# SPDX-License-Identifier: MIT
"""A sender written from the OS Services protocol (version 5.0) against the
firmware's session (tests/session_shim.c through ctypes).

Nothing here comes from the C code: the encodings follow the specification,
the session.ready signature is checked with the `cryptography` package's
Ed25519, and the fingerprint is computed with hashlib. A randomized part
sends valid and invalid messages in random USB chunkings and checks the
invariants the specification states.

GuestSender adds the terminal and file services of the sender, and Browser
drives the KVM's side of them (the browser's terminal channel and file
drops) through the shim and checks what the KVM reports back.

VideoSender adds the video service: it applies video.config, reports
video.status, and streams H.264 access units. The test plays the KVM's
display and WebRTC slot through fake_video.c's g_fake_video.
"""

import ctypes
import hashlib
import json
import os
import random
import struct
import sys
import unicodedata
import unittest

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

LIB = ctypes.CDLL(os.environ["OSVC_SESSION_LIB"])

SESSION, VIDEO, CLIPBOARD, TERMINAL, FILE, UPDATE = range(6)
HELLO, READY, PING, CLOSE, CONFIG, INFO = 1, 2, 3, 4, 5, 6
CONTEXT = b"IP-KVM OS Services session.ready v5"


class ClipboardEvent(ctypes.Structure):
    _fields_ = [
        ("request_id", ctypes.c_uint32),
        ("status", ctypes.c_int32),
        ("answered", ctypes.c_int32),
        ("length", ctypes.c_uint32),
        ("truncated", ctypes.c_int32),
        ("text", ctypes.c_uint8 * 65528),
    ]


LIB.shim_init.argtypes = [ctypes.c_uint64, ctypes.c_char_p, ctypes.c_int]
LIB.shim_set_now.argtypes = [ctypes.c_int64]
LIB.shim_receive.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
LIB.shim_receive.restype = ctypes.c_size_t
LIB.shim_take_output.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
LIB.shim_take_output.restype = ctypes.c_size_t
LIB.shim_reset.argtypes = [ctypes.c_char_p]
LIB.shim_status.argtypes = [ctypes.c_char_p]
LIB.shim_public.argtypes = [ctypes.c_char_p]
LIB.shim_clipboard_request.argtypes = [
    ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int, ctypes.POINTER(ctypes.c_int32)]
LIB.shim_clipboard_request.restype = ctypes.c_uint32
LIB.shim_clipboard_events.restype = ctypes.c_size_t
LIB.shim_clipboard_event.argtypes = [ctypes.c_size_t]
LIB.shim_clipboard_event.restype = ctypes.POINTER(ClipboardEvent)
LIB.shim_set_settings.argtypes = [
    ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32,
    ctypes.c_uint32]
LIB.shim_registered.restype = ctypes.c_uint32

# The protocol's Terminal service section.
T_CAPS, T_OPEN, T_OPENED, T_INPUT, T_OUTPUT, T_RESIZE, T_CLOSE, T_EXITED = range(1, 9)
OPENED_OK, OPENED_UNSUPPORTED, OPENED_BUSY, OPENED_FAILED = range(4)
EXIT_EXITED, EXIT_CLOSED, EXIT_FAILED = range(3)
TERMINAL_DATA_MAX = 65532
# File service.
F_CAPS, F_DROP, F_READY, F_CHUNK, F_CREDIT, F_DONE, F_CANCEL = range(1, 8)
DONE_OK, DONE_FAILED, DONE_CANCELLED = range(3)
CHUNK_DATA_MAX = 65524
WINDOW = 512 * 1024
PAYLOAD_MAX = 65536

# What the browser shows when a terminal or a file transfer ends or fails.
LINE_EXITED = "The shell exited"
LINE_LOST = "The target computer lost its shell"
LINE_DISCONNECTED = "OS Services disconnected"
LINE_DISABLED = "Terminal disabled"
LINE_BUSY = ("Terminal unavailable: the target computer already runs its maximum "
             "number of terminals")
LINE_NO_ANSWER = "Terminal unavailable: the target computer did not answer"
LINE_SLOW = "Terminal closed: the browser did not keep up"
# OSVC_TERMINAL_OUTPUT_HOLD_MS: how long shell output waits for the browser.
TERMINAL_HOLD_MS = 2000
NO_TERMINAL = "no OS Services terminal is connected"
NO_FILES = "no OS Services file transfer is connected"
DROP_BUSY = "a file transfer is already in progress"
STATUS_NAMES = {DONE_OK: "ok", DONE_FAILED: "failed", DONE_CANCELLED: "cancelled"}


class ServiceEvent(ctypes.Structure):
    _fields_ = [
        ("type", ctypes.c_int32),
        ("id", ctypes.c_uint32),
        ("transfer", ctypes.c_uint32),
        ("status", ctypes.c_int32),
        ("last", ctypes.c_int32),
        ("length", ctypes.c_uint32),
        ("bytes", ctypes.c_uint64),
        ("text", ctypes.c_char * 512),
    ]


class Counters(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in (
        "sessions_opened", "sessions_ended", "resets", "messages_in",
        "messages_skipped", "service_faults")] + [
        (name, ctypes.c_uint64) for name in ("bytes_in", "bytes_discarded", "bytes_out")] + [
        ("last_handshake_ms", ctypes.c_int64)]


EVENT_OUTPUT, EVENT_CLOSED, EVENT_CREDIT, EVENT_DONE = 1, 2, 3, 4
EVENT_BATCH = 4096

LIB.shim_counters.restype = ctypes.POINTER(Counters)
LIB.shim_end.argtypes = [ctypes.c_char_p]
LIB.shim_terminal_open.argtypes = [
    ctypes.c_uint32, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t]
LIB.shim_terminal_open.restype = ctypes.c_uint32
LIB.shim_terminal_input.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t]
LIB.shim_terminal_resize.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32]
LIB.shim_terminal_close.argtypes = [ctypes.c_uint32]
LIB.shim_file_drop.argtypes = [
    ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint64),
    ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint32), ctypes.c_char_p, ctypes.c_size_t]
LIB.shim_file_drop.restype = ctypes.c_uint32
LIB.shim_file_attach.argtypes = [ctypes.c_uint32]
LIB.shim_file_data.argtypes = [
    ctypes.c_uint32, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t]
LIB.shim_file_cancel.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
LIB.shim_file_channel_closed.argtypes = [ctypes.c_uint32]
LIB.shim_set_backed_up.argtypes = [ctypes.c_int]
LIB.shim_take_service_events.argtypes = [ctypes.POINTER(ServiceEvent), ctypes.c_size_t]
LIB.shim_take_service_events.restype = ctypes.c_size_t
LIB.shim_take_terminal_bytes.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
LIB.shim_take_terminal_bytes.restype = ctypes.c_size_t


_STREAM = ctypes.create_string_buffer(4 * 1024 * 1024)
_BATCH = (ServiceEvent * EVENT_BATCH)()


def service_faults():
    return LIB.shim_counters().contents.service_faults


_IN = ctypes.create_string_buffer(4 * 1024 * 1024)


def message(service, kind, payload=b""):
    return struct.pack("<HHI", service, kind, len(payload)) + payload


class Kvm:
    """The KVM end of the USB function, as the sender sees it."""

    def __init__(self, seed=1, key_seed=bytes(range(32)), video=False):
        assert LIB.shim_init(seed, key_seed, int(video)) == 0
        self.now = 1000
        self.pending = b""

    def advance(self, ms):
        self.now += ms
        LIB.shim_set_now(self.now)
        LIB.shim_tick()

    def control(self, request):
        out = ctypes.create_string_buffer(24)
        (LIB.shim_reset if request == 0x40 else LIB.shim_status)(out)
        return out.raw

    def write(self, data, chunk=None):
        """Bulk OUT. Returns the bytes the KVM consumed."""
        used = 0
        step = chunk or len(data) or 1
        while used < len(data):
            part = data[used:used + step]
            n = LIB.shim_receive(part, len(part))
            used += n
            if n < len(part):
                break
        return used

    def read(self):
        """Bulk IN: everything the KVM queued."""
        n = LIB.shim_take_output(_IN, len(_IN))
        self.pending += ctypes.string_at(_IN, n)
        return self.pending

    def messages(self):
        data = self.read()
        out = []
        while len(data) >= 8:
            service, kind, size = struct.unpack_from("<HHI", data)
            if len(data) < 8 + size:
                break
            out.append((service, kind, data[8:8 + size]))
            data = data[8 + size:]
        self.pending = data
        return out

    def is_open(self):
        return bool(LIB.shim_open())


class Sender:
    """The sender side of the OS Services protocol: reset, handshake and approval."""

    def __init__(self, kvm, approved=None):
        self.kvm = kvm
        self.approved = approved

    def reset(self):
        status = self.kvm.control(0x40)
        assert status[:4] == b"JKV5", status
        assert status[4] == 0 and status[5:8] == b"\0\0\0"
        token = status[8:24]
        assert token != bytes(16)
        for _ in range(100):
            status = self.kvm.control(0x41)
            assert status[8:24] == token, "another RESET replaced this one"
            assert status[4] in (0, 1), status[4]
            if status[4] == 1:
                return token
        raise AssertionError("STATUS never reached Ready")

    def negotiate(self, chunk=None, stale=b""):
        token = self.reset()
        nonce = os.urandom(32)
        hello_payload = struct.pack("<HH", 5, 0) + token + nonce
        assert self.kvm.write(stale + message(SESSION, HELLO, hello_payload),
                              chunk) == len(stale) + 60
        # The sender searches the IN stream for session.ready with its token.
        data = self.kvm.read()
        assert len(data) <= 128 * 1024
        ready = None
        while len(data) >= 8:
            service, kind, size = struct.unpack_from("<HHI", data)
            payload = data[8:8 + size]
            data = data[8 + size:]
            if (service, kind) == (SESSION, READY) and payload[4:20] == token:
                ready = payload
                break
        assert ready is not None, "no session.ready with the token"
        self.kvm.pending = data
        assert len(ready) == 116
        major, minor = struct.unpack_from("<HH", ready)
        assert (major, minor) == (5, 0)
        public_key = ready[20:52]
        try:
            Ed25519PublicKey.from_public_bytes(public_key).verify(
                ready[52:116], CONTEXT + hello_payload + ready[:52])
        except InvalidSignature as error:
            raise AssertionError("session.ready signature is invalid") from error
        fingerprint = hashlib.sha256(public_key).digest()[:8]
        if self.approved is not None and fingerprint not in self.approved:
            self.kvm.write(message(SESSION, CLOSE, b"\x03"))
            return None
        service, kind, payload = self.kvm.messages()[0]
        assert (service, kind) == (SESSION, CONFIG), (service, kind)
        config = json.loads(payload)
        assert set(config) >= {"terminal_enabled", "files_enabled", "capture_display"}
        assert isinstance(config["terminal_enabled"], bool)
        assert isinstance(config["files_enabled"], bool)
        assert isinstance(config["capture_display"], str)
        self.fingerprint = fingerprint
        self.config = config
        return config


def clipboard_caps(flags=3, os_id=3):
    return message(CLIPBOARD, 1, struct.pack("<IB3x", flags, os_id))


def valid_file_name(name):
    """File service: one path component, not empty, not . or .., no / or \\,
    no control character."""
    try:
        text = name.decode("utf-8")
    except UnicodeDecodeError:
        return False
    if text in ("", ".", ".."):
        return False
    return not any(ch in "/\\" or unicodedata.category(ch) == "Cc" for ch in text)


class GuestSender(Sender):
    """The sender's terminal and file services, from the OS Services protocol.

    It checks every KVM message as strictly as the specification allows: a
    message it does not allow fails the test, where a real sender would end
    the session. Shells echo their input. Files go to a model Downloads
    folder, one at a time in drop order, under a 512 KiB credit window
    returned every `step` bytes."""

    def __init__(self, kvm, max_terminals=4, answer_opens=True, step=128 * 1024,
                 echo=True):
        super().__init__(kvm)
        self.max_terminals = max_terminals
        self.answer_opens = answer_opens
        self.step = step
        self.echo = echo
        self.shell = False           # registered with the shell bit
        self.receive = False         # registered with the receive bit
        self.registrations = {TERMINAL: 0, FILE: 0}
        self.shells = {}             # session_id -> {"size", "input"}
        self.terminal_ids = set()    # every session_id the KVM used
        self.output_sent = {}        # session_id -> every output byte sent
        self.transfers = {}          # transfer_id -> transfer
        self.queue = []              # transfers waiting, in drop order
        self.active = None
        self.ended = {}              # transfer_id -> status of its file.done
        self.downloads = {}          # path -> contents

    # The wire.

    def write(self, data):
        if not self.kvm.is_open():
            return  # The KVM stopped reading; nothing arrives.
        self.assertion(self.kvm.write(data) == len(data), "the KVM stopped reading")

    @staticmethod
    def assertion(condition, text):
        if not condition:
            raise AssertionError("the sender would end the session: " + text)

    def start(self, chunk=None):
        config = self.negotiate(chunk)
        self.apply_config(config)
        self.pump()
        return config

    def pump(self):
        """Reads and answers what the KVM wrote until it writes nothing."""
        for _ in range(10000):
            got = self.kvm.messages()
            if not got:
                return
            for service, kind, payload in got:
                self.handle(service, kind, payload)
        raise AssertionError("the KVM never stops writing")

    def handle(self, service, kind, payload):
        if service == SESSION:
            self.assertion(kind == CONFIG, "session kind %d from the KVM" % kind)
            self.apply_config(self.parse_config(payload))
        elif service == TERMINAL:
            self.handle_terminal(kind, payload)
        elif service == FILE:
            self.handle_file(kind, payload)
        else:
            raise AssertionError("service %d: never registered" % service)

    def parse_config(self, payload):
        config = json.loads(payload)
        for member, kind in (("terminal_enabled", bool), ("files_enabled", bool),
                             ("capture_display", str)):
            self.assertion(isinstance(config.get(member), kind), "session.config " + member)
        return config

    def register(self, service, flags):
        self.registrations[service] += 1
        self.write(message(service, 1, struct.pack("<I", flags)))

    def apply_config(self, config):
        """session.config: a flag that becomes false ends the service's
        activity, then withdraws it; one that becomes true registers it."""
        self.config = config
        if config["terminal_enabled"] and not self.shell:
            self.shell = True
            self.register(TERMINAL, 1)
        elif not config["terminal_enabled"] and self.shell:
            for session_id in list(self.shells):
                self.exit(session_id, EXIT_CLOSED)
            self.shell = False
            self.register(TERMINAL, 0)
        if config["files_enabled"] and not self.receive:
            self.receive = True
            self.register(FILE, 1)
        elif not config["files_enabled"] and self.receive:
            if self.active is not None:
                self.finish(self.active, DONE_CANCELLED)
            for transfer in self.queue:
                self.finish(transfer, DONE_CANCELLED)
            self.queue = []
            self.receive = False
            self.register(FILE, 0)

    # Terminal service.

    def handle_terminal(self, kind, payload):
        if kind == T_OPEN:
            self.assertion(len(payload) == 8, "terminal.open size")
            session_id, rows, cols = struct.unpack("<IHH", payload)
            self.assertion(session_id != 0, "terminal.open with session_id 0")
            self.assertion(session_id not in self.terminal_ids, "a session_id used again")
            self.assertion((rows, cols) == (0, 0), "the KVM opens with size 0 by 0")
            self.terminal_ids.add(session_id)
            self.output_sent[session_id] = bytearray()
            if not self.answer_opens:
                return
            if not self.shell:
                status = OPENED_UNSUPPORTED
            elif len(self.shells) >= self.max_terminals:
                status = OPENED_BUSY
            else:
                status = OPENED_OK
                self.shells[session_id] = {"size": (24, 80), "input": bytearray()}
            self.write(message(TERMINAL, T_OPENED, struct.pack("<IB", session_id, status)))
        elif kind == T_INPUT:
            self.assertion(5 <= len(payload) <= 4 + TERMINAL_DATA_MAX, "terminal.input size")
            session_id = struct.unpack_from("<I", payload)[0]
            self.assertion(session_id in self.terminal_ids, "input for a session never opened")
            shell = self.shells.get(session_id)
            if shell is not None:
                shell["input"] += payload[4:]
                if self.echo:
                    self.output(session_id, payload[4:])
        elif kind == T_RESIZE:
            self.assertion(len(payload) == 8, "terminal.resize size")
            session_id, rows, cols = struct.unpack("<IHH", payload)
            self.assertion(rows != 0 and cols != 0, "terminal.resize to zero")
            self.assertion(session_id in self.terminal_ids, "resize of a session never opened")
            if session_id in self.shells:
                self.shells[session_id]["size"] = (rows, cols)
        elif kind == T_CLOSE:
            self.assertion(len(payload) == 4, "terminal.close size")
            session_id = struct.unpack("<I", payload)[0]
            self.assertion(session_id in self.terminal_ids, "close of a session never opened")
            if session_id in self.shells:
                self.exit(session_id, EXIT_CLOSED)
        else:
            raise AssertionError("terminal kind %d from the KVM" % kind)

    def output(self, session_id, data):
        """What the shell prints, in messages of at most 65,532 bytes."""
        for offset in range(0, len(data), TERMINAL_DATA_MAX):
            piece = data[offset:offset + TERMINAL_DATA_MAX]
            self.output_sent.setdefault(session_id, bytearray()).extend(piece)
            self.write(message(TERMINAL, T_OUTPUT, struct.pack("<I", session_id) + piece))

    def exit(self, session_id, reason):
        del self.shells[session_id]
        self.write(message(TERMINAL, T_EXITED, struct.pack("<IB", session_id, reason)))

    # File service.

    def handle_file(self, kind, payload):
        if kind == F_DROP:
            self.handle_drop(payload)
        elif kind == F_CHUNK:
            self.assertion(12 <= len(payload) <= 12 + CHUNK_DATA_MAX, "file.chunk size")
            transfer_id, offset = struct.unpack_from("<IQ", payload)
            data = payload[12:]
            # Not a rule of the specification, but the KVM never needs one.
            self.assertion(len(data) > 0, "an empty file.chunk")
            self.assertion(transfer_id in self.transfers, "a chunk for an unknown transfer")
            if transfer_id in self.ended:
                return  # The sender ignores a chunk for a transfer that ended.
            transfer = self.transfers[transfer_id]
            self.assertion(transfer is self.active, "a chunk before file.ready (no credit)")
            self.assertion(offset == transfer["received"], "a chunk offset out of order")
            end = offset + len(data)
            self.assertion(end <= transfer["credit"], "a chunk beyond the credit")
            self.assertion(end <= transfer["size"], "a chunk beyond the file size")
            transfer["data"] += data
            transfer["received"] = end
            transfer["unreturned"] += len(data)
            if end == transfer["size"]:
                self.finish(transfer, DONE_OK)
                self.next_transfer()
            elif transfer["unreturned"] >= self.step:
                self.grant(transfer, transfer["unreturned"])
                transfer["unreturned"] = 0
        elif kind == F_CANCEL:
            self.assertion(len(payload) == 4, "file.cancel size")
            transfer_id = struct.unpack("<I", payload)[0]
            self.assertion(transfer_id in self.transfers, "a cancel for an unknown transfer")
            if transfer_id in self.ended:
                return
            transfer = self.transfers[transfer_id]
            if transfer is self.active:
                self.finish(transfer, DONE_CANCELLED)  # deletes the partial file
                self.next_transfer()
            else:
                self.queue.remove(transfer)
                self.finish(transfer, DONE_CANCELLED)
        else:
            raise AssertionError("file kind %d from the KVM" % kind)

    def handle_drop(self, payload):
        self.assertion(len(payload) >= 4, "file.drop size")
        count = struct.unpack_from("<I", payload)[0]
        self.assertion(count >= 1, "file.drop without files")
        offset = 4
        entries = []
        for _ in range(count):
            self.assertion(offset + 14 <= len(payload), "a file.drop entry cut short")
            transfer_id, size, name_length = struct.unpack_from("<IQH", payload, offset)
            offset += 14
            self.assertion(offset + name_length <= len(payload), "a file name cut short")
            name = payload[offset:offset + name_length]
            offset += name_length
            self.assertion(transfer_id != 0, "transfer_id 0")
            self.assertion(transfer_id not in self.transfers and
                           all(e["id"] != transfer_id for e in entries),
                           "a transfer_id used again")
            self.assertion(valid_file_name(name), "file name %r" % name)
            entries.append({"id": transfer_id, "size": size, "name": name.decode(),
                            "credit": 0, "received": 0, "unreturned": 0,
                            "data": bytearray()})
        self.assertion(offset == len(payload), "bytes after the last file.drop entry")
        for entry in entries:
            self.transfers[entry["id"]] = entry
        if not (self.receive and self.config["files_enabled"]):
            for entry in entries:
                self.finish(entry, DONE_FAILED)
            return
        self.queue.extend(entries)
        self.next_transfer()

    def next_transfer(self):
        """One file at a time: file.ready when it is created, and for an
        empty file file.done at once."""
        self.active = None
        while self.queue:
            transfer = self.queue.pop(0)
            transfer["credit"] = WINDOW
            self.write(message(FILE, F_READY, struct.pack("<II", transfer["id"], WINDOW)))
            if transfer["size"] > 0:
                self.active = transfer
                return
            self.finish(transfer, DONE_OK)

    def grant(self, transfer, amount):
        transfer["credit"] += amount
        self.write(message(FILE, F_CREDIT, struct.pack("<II", transfer["id"], amount)))

    def finish(self, transfer, status):
        path = b""
        if status == DONE_OK:
            stem, dot, extension = transfer["name"].rpartition(".")
            if not dot:
                stem, extension = transfer["name"], ""
            path = "/home/guest/Downloads/" + transfer["name"]
            copy = 1
            while path in self.downloads:
                copy += 1
                path = "/home/guest/Downloads/%s (%d)%s" % (
                    stem, copy, dot + extension if dot else "")
            self.downloads[path] = bytes(transfer["data"])
            path = path.encode()
        if transfer is self.active:
            self.active = None
        self.ended[transfer["id"]] = status
        self.write(message(FILE, F_DONE, struct.pack("<IB3x", transfer["id"], status) + path))


class Browser:
    """The browser's side of the KVM: terminal channels and file drops
    through the shim. poll() collects what the KVM tells the browser and
    checks it: output only for open terminals, one end per terminal, credit
    and one outcome per transfer, `last` on a drop's final outcome."""

    def __init__(self):
        self.opened = []             # terminal IDs, in order
        self.closed_by_browser = set()
        self.output = {}             # terminal_id -> bytes shown
        self.input = {}              # terminal_id -> keystrokes the KVM took
        self.closed = {}             # terminal_id -> (line, exited)
        self.drops = {}              # drop_id -> transfer IDs
        self.drop_of = {}            # transfer_id -> drop_id
        self.size = {}               # transfer_id -> file size
        self.attached = set()
        self.channel_closed = set()
        self.credit = {}             # transfer_id -> credit heard of
        self.sent = {}               # transfer_id -> bytes sent
        self.done = {}               # transfer_id -> (status, path, last)
        self.order = []              # transfer IDs in the order of their outcome

    def open_terminal(self, rows=24, cols=80):
        error = ctypes.create_string_buffer(256)
        terminal_id = LIB.shim_terminal_open(rows, cols, error, len(error))
        if terminal_id:
            assert error.value == b""
            assert terminal_id not in self.opened, "a terminal ID used again"
            self.opened.append(terminal_id)
            self.output[terminal_id] = bytearray()
            self.input[terminal_id] = bytearray()
        else:
            assert error.value, "no ID and no reason"
        return terminal_id, error.value.decode()

    def type(self, terminal_id, data):
        accepted = bool(LIB.shim_terminal_input(terminal_id, data, len(data)))
        if accepted:
            self.input[terminal_id] += data
        return accepted

    def resize(self, terminal_id, rows, cols):
        return bool(LIB.shim_terminal_resize(terminal_id, rows, cols))

    def close_terminal(self, terminal_id):
        if terminal_id not in self.closed:
            self.closed_by_browser.add(terminal_id)
        LIB.shim_terminal_close(terminal_id)

    def drop(self, files, attach=True):
        """files: (name bytes, size) pairs. Returns (drop_id, IDs, error)."""
        count = len(files)
        names = b"".join(name for name, _ in files)
        lengths = (ctypes.c_uint32 * max(count, 1))(*[len(name) for name, _ in files])
        sizes = (ctypes.c_uint64 * max(count, 1))(*[size for _, size in files])
        ids = (ctypes.c_uint32 * max(count, 1))()
        error = ctypes.create_string_buffer(512)
        drop_id = LIB.shim_file_drop(names, lengths, sizes, count, ids, error, len(error))
        if not drop_id:
            assert error.value, "no drop and no reason"
            return 0, [], error.value.decode()
        transfer_ids = list(ids[:count])
        assert error.value == b""
        assert drop_id not in self.drops and len(set(transfer_ids)) == count
        for transfer_id, (_, size) in zip(transfer_ids, files):
            assert transfer_id != 0 and transfer_id not in self.drop_of
            self.drop_of[transfer_id] = drop_id
            self.size[transfer_id] = size
        self.drops[drop_id] = transfer_ids
        if attach:
            self.attach(drop_id)
        return drop_id, transfer_ids, ""

    def attach(self, drop_id):
        attached = bool(LIB.shim_file_attach(drop_id))
        if attached:
            self.attached.add(drop_id)
        return attached

    def room(self, transfer_id):
        return self.credit.get(transfer_id, 0) - self.sent.get(transfer_id, 0)

    def send(self, transfer_id, data):
        accepted = bool(LIB.shim_file_data(self.drop_of[transfer_id], transfer_id,
                                           data, len(data)))
        if accepted:
            self.sent[transfer_id] = self.sent.get(transfer_id, 0) + len(data)
        return accepted

    def cancel(self, transfer_id):
        return bool(LIB.shim_file_cancel(self.drop_of[transfer_id], transfer_id))

    def close_channel(self, drop_id):
        self.channel_closed.add(drop_id)
        LIB.shim_file_channel_closed(drop_id)

    def poll(self):
        """Takes what the KVM told the browser. Returns the events."""
        stream_length = LIB.shim_take_terminal_bytes(_STREAM, len(_STREAM))
        stream = ctypes.string_at(_STREAM, stream_length)
        position = 0
        events = []
        batch = _BATCH
        while True:
            n = LIB.shim_take_service_events(batch, EVENT_BATCH)
            events.extend((e.type, e.id, e.transfer, e.status, e.last, e.length, e.bytes,
                           bytes(e.text[:min(e.length, 511)])) for e in batch[:n])
            if n < EVENT_BATCH:
                break
        assert not LIB.shim_service_events_lost()
        out = []
        for kind, ident, transfer_id, status, last, length, credit, text in events:
            if kind == EVENT_OUTPUT:
                assert ident in self.output, "output for a terminal never opened"
                assert ident not in self.closed, "output after the terminal ended"
                assert ident not in self.closed_by_browser, "output after the browser closed"
                assert length > 0
                self.output[ident] += stream[position:position + length]
                position += length
                out.append(("output", ident, length))
            elif kind == EVENT_CLOSED:
                assert ident in self.output, "an end for a terminal never opened"
                assert ident not in self.closed, "a terminal ended twice"
                assert ident not in self.closed_by_browser, "an end after the browser closed"
                self.closed[ident] = (text.decode(), bool(status))
                out.append(("closed", ident, text.decode(), bool(status)))
            elif kind == EVENT_CREDIT:
                assert self.drop_of.get(transfer_id) == ident, "credit for another drop"
                assert ident in self.attached and ident not in self.channel_closed
                assert transfer_id not in self.done, "credit after the outcome"
                assert credit > 0
                self.credit[transfer_id] = self.credit.get(transfer_id, 0) + credit
                out.append(("credit", transfer_id, credit))
            elif kind == EVENT_DONE:
                assert self.drop_of.get(transfer_id) == ident, "an outcome for another drop"
                assert ident in self.attached and ident not in self.channel_closed
                assert transfer_id not in self.done, "two outcomes for one transfer"
                assert status in STATUS_NAMES
                assert status == DONE_OK or text == b"", "a path for a transfer that failed"
                self.done[transfer_id] = (status, text.decode(), bool(last))
                self.order.append(transfer_id)
                remaining = [t for t in self.drops[ident] if t not in self.done]
                assert bool(last) == (not remaining), "last on the wrong outcome"
                out.append(("done", transfer_id, status))
            else:
                raise AssertionError("event type %d" % kind)
        assert position == len(stream)
        return out

    def open_transfers(self):
        return [t for drop_id, ids in self.drops.items()
                if drop_id in self.attached and drop_id not in self.channel_closed
                for t in ids if t not in self.done]


class ProtocolTest(unittest.TestCase):
    def setUp(self):
        self.kvm = Kvm(seed=random.getrandbits(63) | 1)
        self.sender = Sender(self.kvm)

    def test_handshake_in_every_chunking(self):
        for chunk in (1, 2, 3, 7, 8, 9, 59, 60, 61, 512, 16384):
            config = self.sender.negotiate(chunk=chunk)
            self.assertEqual(config["capture_display"], "")
            self.assertTrue(self.kvm.is_open())

    def test_fingerprint_is_sha256_prefix_of_the_key(self):
        self.sender.negotiate()
        key = ctypes.create_string_buffer(32)
        LIB.shim_public(key)
        self.assertEqual(self.sender.fingerprint, hashlib.sha256(key.raw).digest()[:8])

    def test_stale_bytes_of_the_last_session(self):
        # A sender killed mid-frame leaves a frame head and its body in the
        # OUT endpoint; an old hello carries an old token.
        old = self.sender.negotiate()
        self.assertIsNotNone(old)
        stale = message(VIDEO, 5, os.urandom(3000))[:2000]
        stale += message(SESSION, HELLO, struct.pack("<HH", 5, 0) + os.urandom(48))
        self.sender.negotiate(stale=stale, chunk=4096)
        self.assertTrue(self.kvm.is_open())

    def test_not_approved(self):
        sender = Sender(self.kvm, approved=[b"\0" * 8])
        self.assertIsNone(sender.negotiate())
        self.assertFalse(self.kvm.is_open())
        self.assertTrue(LIB.shim_approval_pending())
        # The user approves the fingerprint; the next session clears it.
        Sender(self.kvm).negotiate()
        self.kvm.write(message(SESSION, INFO, b'{"version":"1","displays":[]}'))
        self.assertFalse(LIB.shim_approval_pending())

    def test_liveness_with_pings(self):
        self.sender.negotiate()
        for _ in range(20):
            self.kvm.advance(2000)
            self.kvm.write(message(SESSION, PING))
        self.assertTrue(self.kvm.is_open())
        self.kvm.advance(15001)
        self.assertFalse(self.kvm.is_open())
        # The KVM stopped reading: a write consumes nothing.
        self.assertEqual(self.kvm.write(message(SESSION, PING)), 0)

    def test_clipboard_round_trip(self):
        self.sender.negotiate()
        self.kvm.write(clipboard_caps())
        self.assertEqual(self.kvm.messages(), [(CLIPBOARD, 7, b"\x01")])
        status = ctypes.c_int32()
        text = "Grüße 👋\n\ttabs".encode()
        set_id = LIB.shim_clipboard_request(text, len(text), 1, ctypes.byref(status))
        get_id = LIB.shim_clipboard_request(None, 0, 0, ctypes.byref(status))
        self.assertNotEqual(set_id, 0)
        self.assertNotEqual(get_id, 0)
        requests = self.kvm.messages()
        self.assertEqual(requests[0], (CLIPBOARD, 4, struct.pack("<I", set_id) + text))
        self.assertEqual(requests[1], (CLIPBOARD, 2, struct.pack("<I", get_id)))
        # Answers in the other order.
        self.kvm.write(message(CLIPBOARD, 3, struct.pack("<IBB2x", get_id, 0, 1) + text))
        self.kvm.write(message(CLIPBOARD, 5, struct.pack("<IB", set_id, 0)))
        events = [LIB.shim_clipboard_event(i).contents for i in range(LIB.shim_clipboard_events())]
        self.assertEqual([(e.request_id, e.status, e.answered) for e in events],
                         [(get_id, 0, 1), (set_id, 0, 1)])
        self.assertEqual(bytes(events[0].text[:events[0].length]), text)
        self.assertEqual(events[0].truncated, 1)

    def test_settings_changes_reach_the_sender(self):
        self.sender.negotiate()
        self.kvm.write(clipboard_caps())
        self.kvm.messages()
        LIB.shim_set_settings(0, 1, 0, "HDMI-A-1".encode(), 1920, 1080)
        got = self.kvm.messages()
        self.assertIn((SESSION, CONFIG), [(s, k) for s, k, _ in got])
        config = json.loads([p for s, k, p in got if (s, k) == (SESSION, CONFIG)][0])
        self.assertEqual(config, {"terminal_enabled": True, "files_enabled": False,
                                  "capture_display": "HDMI-A-1",
                                  "display_mode_by_sender": True})
        self.assertIn((CLIPBOARD, 7, b"\x00"), got)

    def test_registrations_answered_as_specified(self):
        self.sender.negotiate()
        self.kvm.write(message(TERMINAL, 1, struct.pack("<I", 1)))
        self.kvm.write(message(FILE, 1, struct.pack("<I", 1)))
        self.kvm.write(message(UPDATE, 1, struct.pack("<I", 1) + b"linux-x86_64-appimage"))
        self.assertEqual(self.kvm.messages(), [])  # no answers
        self.kvm.write(message(VIDEO, 1, struct.pack("<IIII", 1, 3840, 2160, 60)))
        got = self.kvm.messages()
        self.assertEqual(len(got), 1)
        service, kind, payload = got[0]
        self.assertEqual((service, kind, len(payload)), (VIDEO, 3, 20))
        enabled, layout, codec, reserved, width, height, kbps, fps = struct.unpack("<BBBBIIII", payload)
        self.assertIn(enabled, (0, 1))
        self.assertIn(layout, (0, 1))
        self.assertEqual((codec, reserved), (0, 0))
        self.assertTrue(1 <= width <= 3840 and 1 <= height <= 2160)
        self.assertLessEqual(fps, 60)
        self.assertEqual(LIB.shim_registered(), 0b11110)

    def test_randomized_streams(self):
        """Random mixes of valid, unknown and invalid messages in random
        chunkings. The KVM either keeps the session open and answers every
        clipboard request exactly once, or ends it; it never reads past the
        end of a session."""
        rng = random.Random(1234)
        for round_ in range(300):
            kvm = Kvm(seed=round_ + 1)
            Sender(kvm).negotiate(chunk=rng.choice([1, 5, 64, 512, 4096]))
            stream = b""
            expect_end = False
            for _ in range(rng.randint(1, 25)):
                choice = rng.random()
                if choice < 0.25:
                    stream += message(SESSION, PING)
                elif choice < 0.4:
                    stream += message(rng.choice([6, 7, 200, 65535]), rng.randint(0, 9),
                                      os.urandom(rng.randint(0, 300)))
                elif choice < 0.55:
                    stream += message(SESSION, INFO, json.dumps(
                        {"version": "v" * rng.randint(0, 64),
                         "displays": ["D%d" % i for i in range(rng.randint(0, 64))]}).encode())
                elif choice < 0.65:
                    stream += clipboard_caps(rng.randint(0, 3), rng.randint(0, 3))
                elif choice < 0.75:
                    # Invalid for a service other than the session: only
                    # that service faults.
                    stream += message(rng.choice([CLIPBOARD, TERMINAL, FILE, VIDEO]), 1,
                                      os.urandom(rng.randint(0, 3)))
                elif choice < 0.8:
                    stream += message(VIDEO, 5, os.urandom(rng.randint(0, 70000)))
                elif choice < 0.85:
                    stream += message(SESSION, rng.choice([HELLO, READY, CONFIG]), b"x")
                    expect_end = True
                    break
                else:
                    stream += message(TERMINAL, rng.randint(2, 8), os.urandom(rng.randint(0, 64)))
            used = kvm.write(stream, rng.choice([1, 3, 64, 1000, 65536]))
            if expect_end:
                self.assertFalse(kvm.is_open(), round_)
                self.assertEqual(used, len(stream), round_)
            else:
                self.assertTrue(kvm.is_open(), round_)
                self.assertEqual(used, len(stream))
            self.assertEqual(kvm.write(message(SESSION, PING)), 0 if expect_end else 8)


class ServiceTest(unittest.TestCase):
    """The terminal and file services between GuestSender and Browser."""

    def start(self, **sender_options):
        self.kvm = Kvm(seed=random.getrandbits(63) | 1)
        self.sender = GuestSender(self.kvm, **sender_options)
        self.browser = Browser()
        config = self.sender.start()
        self.assertTrue(config["terminal_enabled"] and config["files_enabled"])
        self.assertTrue(LIB.shim_terminal_available())
        self.assertTrue(LIB.shim_file_available())

    def setUp(self):
        self.start()

    def step(self):
        self.sender.pump()
        return self.browser.poll()

    def advance(self, ms):
        """Time passes; the sender pings so the session stays."""
        while ms > 0:
            n = min(ms, 10000)
            self.kvm.write(message(SESSION, PING))
            self.kvm.advance(n)
            ms -= n
            self.step()

    def run_drop(self, transfer_ids, contents, frame=32 * 1024, limit=100000):
        """The browser sends each file as its credit allows, frame by frame,
        until every transfer has its outcome."""
        for _ in range(limit):
            self.step()
            if all(t in self.browser.done for t in transfer_ids):
                return
            for transfer_id, data in zip(transfer_ids, contents):
                if transfer_id in self.browser.done:
                    continue
                sent = self.browser.sent.get(transfer_id, 0)
                n = min(self.browser.room(transfer_id), len(data) - sent, frame)
                if n > 0:
                    self.assertTrue(self.browser.send(transfer_id, data[sent:sent + n]))
        self.fail("the drop stalled")

    def test_terminal_round_trip(self):
        ids = []
        for rows, cols in ((24, 80), (50, 200), (0, 0)):
            terminal_id, error = self.browser.open_terminal(rows, cols)
            self.assertEqual(error, "")
            ids.append(terminal_id)
        self.step()
        self.assertEqual(sorted(self.sender.shells), sorted(ids))
        # terminal.open came with 0 by 0 (checked by the sender), then the
        # browser's size; without one the shell keeps 24 by 80.
        self.assertEqual([self.sender.shells[t]["size"] for t in ids],
                         [(24, 80), (50, 200), (24, 80)])
        # Keystrokes, echoed by the shell; large input crosses messages.
        big = os.urandom(150000)
        self.assertTrue(self.browser.type(ids[0], b"ls -la\r"))
        self.assertTrue(self.browser.type(ids[1], big))
        self.step()
        self.assertEqual(bytes(self.sender.shells[ids[1]]["input"]), big)
        self.assertEqual(bytes(self.browser.output[ids[0]]), b"ls -la\r")
        self.assertEqual(bytes(self.browser.output[ids[1]]), big)
        self.assertEqual(self.browser.output[ids[2]], b"")
        # Resizes that the protocol allows reach the shell; others do not.
        self.assertTrue(self.browser.resize(ids[2], 30, 100))
        self.assertFalse(self.browser.resize(ids[2], 0, 100))
        self.assertFalse(self.browser.resize(ids[2], 30, 65536))
        self.step()
        self.assertEqual(self.sender.shells[ids[2]]["size"], (30, 100))
        # The browser closes one: the shell ends, the KVM reports nothing.
        self.browser.close_terminal(ids[0])
        self.step()
        self.assertNotIn(ids[0], self.sender.shells)
        self.assertNotIn(ids[0], self.browser.closed)
        self.assertFalse(self.browser.type(ids[0], b"x"))
        # The shells end on the guest.
        self.sender.exit(ids[1], EXIT_EXITED)
        self.sender.exit(ids[2], EXIT_FAILED)
        self.step()
        self.assertEqual(self.browser.closed, {ids[1]: (LINE_EXITED, True),
                                               ids[2]: (LINE_LOST, False)})
        self.assertEqual(service_faults(), 0)
        self.assertTrue(self.kvm.is_open())

    def test_terminal_limits_refusals_and_timeout(self):
        ids = [self.browser.open_terminal()[0] for _ in range(4)]
        self.step()
        self.assertEqual(len(self.sender.shells), 4)
        # The KVM refuses a fifth itself; the sender hears nothing.
        terminal_id, error = self.browser.open_terminal()
        self.assertEqual((terminal_id, error), (0, LINE_BUSY))
        self.step()
        self.assertEqual(len(self.sender.terminal_ids), 4)
        self.browser.close_terminal(ids[0])
        self.assertNotEqual(self.browser.open_terminal()[0], 0)
        self.step()
        self.assertEqual(len(self.sender.shells), 4)

        # A sender busier than the KVM knows answers Busy.
        self.start(max_terminals=1)
        first = self.browser.open_terminal()[0]
        second = self.browser.open_terminal()[0]
        self.step()
        self.assertEqual(list(self.sender.shells), [first])
        self.assertEqual(self.browser.closed, {second: (LINE_BUSY, False)})

        # A sender that never answers: terminal.close after 5 seconds.
        self.start(answer_opens=False)
        silent = self.browser.open_terminal()[0]
        self.step()
        self.advance(4999)
        self.assertEqual(self.browser.closed, {})
        self.advance(1)
        self.assertEqual(self.browser.closed, {silent: (LINE_NO_ANSWER, False)})
        self.assertTrue(self.kvm.is_open())

    def test_file_drop_round_trip(self):
        files = [
            (b"empty.txt", b""),
            (b"one.bin", b"\x01"),
            ("Grüße 👋.txt".encode(), os.urandom(CHUNK_DATA_MAX)),
            (b"edge", os.urandom(CHUNK_DATA_MAX + 1)),
            (b"photo.jpg", os.urandom(WINDOW + 3)),
            (b"same.txt", os.urandom(100)),
            (b"same.txt", os.urandom(200)),
            (b"large.iso", os.urandom(3 * 1024 * 1024 + 17)),
        ]
        drop_id, ids, error = self.browser.drop([(n, len(d)) for n, d in files])
        self.assertEqual(error, "")
        contents = [d for _, d in files]
        self.run_drop(ids, contents)
        # Every file arrived whole at its path, in drop order.
        self.assertEqual(self.browser.order, ids)
        paths = []
        for transfer_id, data in zip(ids, contents):
            status, path, _ = self.browser.done[transfer_id]
            self.assertEqual(status, DONE_OK)
            self.assertEqual(self.sender.downloads[path], data)
            paths.append(path)
        self.assertEqual(len(set(paths)), len(paths))
        self.assertEqual(paths[6], "/home/guest/Downloads/same (2).txt")
        # The browser heard exactly the credit the sender granted.
        for transfer_id in ids:
            self.assertEqual(self.browser.credit.get(transfer_id, 0),
                             self.sender.transfers[transfer_id]["credit"])
        # The next drop may start.
        self.assertEqual(self.browser.drop([(b"next", 1)])[2], "")
        self.assertEqual(service_faults(), 0)

    def test_file_cancels_channel_close_and_session_end(self):
        size = 300 * 1024
        contents = [os.urandom(size) for _ in range(3)]
        drop_id, ids, _ = self.browser.drop([(b"f%d" % i, size) for i in range(3)])
        self.step()
        # Some of the first file, then the browser stops it.
        self.assertTrue(self.browser.send(ids[0], contents[0][:100000]))
        self.assertTrue(self.browser.cancel(ids[0]))
        self.assertFalse(self.browser.cancel(ids[0]))
        self.step()
        self.assertEqual(self.browser.done[ids[0]][0], DONE_CANCELLED)
        self.assertEqual(self.sender.ended[ids[0]], DONE_CANCELLED)
        self.assertEqual(self.sender.downloads, {})  # the partial file is gone
        # The sender moved on to the second file.
        self.assertEqual(self.browser.credit[ids[1]], WINDOW)
        self.assertTrue(self.browser.send(ids[1], contents[1][:5000]))
        # The browser's channel closes: both remaining transfers end on the
        # sender, and nobody is left to tell.
        self.browser.close_channel(drop_id)
        self.assertEqual(self.step(), [])
        self.assertEqual(self.sender.ended, {t: DONE_CANCELLED for t in ids})
        self.assertNotIn(ids[1], self.browser.done)

        # The session ends mid-transfer: each open transfer fails once.
        drop_id, ids, _ = self.browser.drop([(b"a", 10), (b"b", size), (b"c", 0)])
        self.step()
        self.assertTrue(self.browser.send(ids[0], b"0123456789"))
        self.step()
        self.assertTrue(self.browser.send(ids[1], contents[1][:1000]))
        LIB.shim_end(b"the IN endpoint did not drain")
        self.browser.poll()
        self.assertEqual([self.browser.done[t][0] for t in ids],
                         [DONE_OK, DONE_FAILED, DONE_FAILED])
        self.assertEqual(self.browser.poll(), [])
        self.assertFalse(self.browser.send(ids[1], b"x"))
        self.assertFalse(self.kvm.is_open())

    def test_settings_end_and_restore_the_services(self):
        terminals = [self.browser.open_terminal()[0] for _ in range(2)]
        drop_id, ids, _ = self.browser.drop([(b"a", 200000), (b"b", 10)])
        self.step()
        self.assertTrue(self.browser.send(ids[0], os.urandom(50000)))
        # The terminal setting goes off.
        LIB.shim_set_settings(1, 0, 1, b"", 1920, 1080)
        self.step()
        self.assertEqual(self.browser.closed, {t: (LINE_DISABLED, False) for t in terminals})
        self.assertEqual(self.sender.shells, {})
        self.assertFalse(self.sender.shell)
        self.assertFalse(LIB.shim_terminal_available())
        self.assertEqual(self.browser.open_terminal(), (0, NO_TERMINAL))
        # The transfer went on.
        self.assertTrue(self.browser.send(ids[0], os.urandom(1000)))
        # File reception goes off: the browser learns both failed.
        LIB.shim_set_settings(1, 0, 0, b"", 1920, 1080)
        self.step()
        self.assertEqual({t: self.browser.done[t][0] for t in ids},
                         {t: DONE_FAILED for t in ids})
        self.assertEqual(self.sender.ended, {t: DONE_CANCELLED for t in ids})
        self.assertFalse(LIB.shim_file_available())
        self.assertEqual(self.browser.drop([(b"x", 1)])[2], NO_FILES)
        # Both back on: the sender registers again.
        LIB.shim_set_settings(1, 1, 1, b"", 1920, 1080)
        self.step()
        self.assertTrue(LIB.shim_terminal_available() and LIB.shim_file_available())
        terminal_id = self.browser.open_terminal()[0]
        drop_id, ids, _ = self.browser.drop([(b"again", 3)])
        self.step()
        self.assertTrue(self.browser.type(terminal_id, b"ok"))
        self.run_drop(ids, [b"abc"])
        self.assertEqual(bytes(self.browser.output[terminal_id]), b"ok")
        self.assertEqual(service_faults(), 0)

    def test_invalid_sender_messages_fault_one_service(self):
        terminal_id = self.browser.open_terminal()[0]
        drop_id, ids, _ = self.browser.drop([(b"a", 100)])
        self.step()
        # terminal.output with session_id 0.
        self.sender.write(message(TERMINAL, T_OUTPUT, b"\0\0\0\0x"))
        self.step()
        self.assertEqual(self.browser.closed, {
            terminal_id: ("Terminal faulted: invalid terminal.output", False)})
        self.assertEqual(service_faults(), 1)
        self.assertFalse(LIB.shim_terminal_available())
        # The transfer is untouched.
        self.assertTrue(self.browser.send(ids[0], b"x" * 50))
        self.step()
        self.assertNotIn(ids[0], self.browser.done)
        # file.done with a reserved byte set.
        self.sender.write(message(FILE, F_DONE, struct.pack("<IBBH", ids[0], 1, 1, 0)))
        self.step()
        self.assertEqual(self.browser.done[ids[0]][0], DONE_FAILED)
        self.assertEqual(service_faults(), 2)
        # Both register again, and work.
        self.sender.register(TERMINAL, 1)
        self.sender.register(FILE, 1)
        self.assertTrue(LIB.shim_terminal_available() and LIB.shim_file_available())
        again = self.browser.open_terminal()[0]
        self.step()
        self.assertTrue(self.browser.type(again, b"hi"))
        self.step()
        self.assertEqual(bytes(self.browser.output[again]), b"hi")
        self.assertTrue(self.kvm.is_open())

    def test_randomized_terminal_and_file_traffic(self):
        """Random browser, sender and clock actions. The sender checks every
        KVM message; the browser checks every report. After each step:
        output shown is a prefix of the shell's output, input delivered is a
        prefix of what the KVM took, the browser heard exactly the credit
        the sender granted for each open transfer, and the session stays.
        When the session ends, every terminal and every transfer has ended
        exactly once."""
        rng = random.Random(2468)
        sizes = [0, 1, 1000, CHUNK_DATA_MAX, CHUNK_DATA_MAX + 1, 200000, WINDOW + 1]
        for round_ in range(100):
            self.start(step=rng.choice([1, 4096, 128 * 1024, WINDOW]))
            browser, sender = self.browser, self.sender
            faults = 0
            faulted = {TERMINAL: False, FILE: False}
            for _ in range(120):
                registrations = dict(sender.registrations)
                live_terminals = [t for t in browser.opened
                                  if t not in browser.closed and t not in browser.closed_by_browser]
                open_transfers = browser.open_transfers()
                action = rng.random()
                if action < 0.10:
                    terminal_id, error = browser.open_terminal(
                        rng.choice([0, 1, 24, 65535, 65536]), rng.choice([0, 1, 80, 65535]))
                    if not terminal_id:
                        self.assertIn(error, (NO_TERMINAL, LINE_BUSY))
                elif action < 0.22 and browser.opened:
                    data = os.urandom(rng.choice([1, 3, 100, TERMINAL_DATA_MAX + 1, 70000]))
                    browser.type(rng.choice(browser.opened), data)
                elif action < 0.26 and browser.opened:
                    browser.resize(rng.choice(browser.opened), rng.randint(0, 300),
                                   rng.randint(0, 300))
                elif action < 0.30 and browser.opened:
                    browser.close_terminal(rng.choice(browser.opened))
                elif action < 0.34 and sender.shells:
                    sender.exit(rng.choice(list(sender.shells)),
                                rng.choice([EXIT_EXITED, EXIT_CLOSED, EXIT_FAILED]))
                elif action < 0.42:
                    # A shell, or a session_id the KVM never used.
                    target = rng.choice(list(sender.shells) + [rng.randint(1 << 30, (1 << 32) - 1)])
                    sender.output(target, os.urandom(rng.choice([1, 50, 70000])))
                elif action < 0.44 and sender.shells:
                    LIB.shim_set_backed_up(1)
                    slow = rng.choice(list(sender.shells))
                    sender.output(slow, b"flood")
                    # The browser takes nothing while the output waits.
                    self.advance(TERMINAL_HOLD_MS)
                    LIB.shim_set_backed_up(0)
                    browser.poll()
                    if slow in live_terminals:
                        self.assertEqual(browser.closed[slow], (LINE_SLOW, False))
                elif action < 0.52:
                    count = rng.randint(1, 4)
                    files = [(b"r%d-%d.bin" % (round_, i), rng.choice(sizes)) for i in range(count)]
                    drop_id, _, error = browser.drop(files)
                    if not drop_id:
                        self.assertIn(error, (NO_FILES, DROP_BUSY))
                elif action < 0.68 and open_transfers:
                    for transfer_id in open_transfers:
                        n = min(browser.room(transfer_id),
                                browser.size[transfer_id] - browser.sent.get(transfer_id, 0),
                                rng.choice([1, 32 * 1024, 100000]))
                        if n > 0:
                            self.assertTrue(browser.send(transfer_id, os.urandom(n)))
                elif action < 0.70 and open_transfers:
                    # A browser that sends past its credit, or past the
                    # file's size, loses the drop.
                    transfer_id = rng.choice(open_transfers)
                    room = browser.room(transfer_id)
                    left = browser.size[transfer_id] - browser.sent.get(transfer_id, 0)
                    n = left + 1 if room > left and rng.random() < 0.5 else room + 1
                    self.assertFalse(browser.send(transfer_id, os.urandom(n)))
                elif action < 0.75 and open_transfers:
                    self.assertTrue(browser.cancel(rng.choice(open_transfers)))
                elif action < 0.78 and open_transfers:
                    browser.close_channel(browser.drop_of[rng.choice(open_transfers)])
                elif action < 0.86:
                    self.advance(rng.choice([100, 4999, 5000, 15000, 30000]))
                elif action < 0.94:
                    LIB.shim_set_settings(1, rng.random() < 0.7, rng.random() < 0.7, b"",
                                          1920, 1080)
                else:
                    service = rng.choice([TERMINAL, FILE])
                    invalid = rng.choice({
                        TERMINAL: [message(TERMINAL, T_OUTPUT, b"\0\0\0\0x"),
                                   message(TERMINAL, T_OPENED, b"\1\0\0\0\x09"),
                                   message(TERMINAL, T_EXITED, b"\1\0\0"),
                                   message(TERMINAL, T_OPEN, bytes(8))],
                        FILE: [message(FILE, F_READY, bytes(7)),
                               message(FILE, F_DONE, b"\1\0\0\0\x05\0\0\0"),
                               message(FILE, F_DROP, b"\1\0\0\0")],
                    }[service])
                    active = sender.active
                    if (service == FILE and active is not None and
                            active["received"] < active["size"] and rng.random() < 0.3):
                        # OK for a file the KVM has not sent in full.
                        sender.finish(active, DONE_OK)
                    else:
                        sender.write(invalid)
                    if not faulted[service]:
                        faults += 1
                        faulted[service] = True
                    flag = sender.shell if service == TERMINAL else sender.receive
                    if flag and rng.random() < 0.8:
                        sender.register(service, 1)
                self.step()
                for service in (TERMINAL, FILE):
                    if sender.registrations[service] != registrations[service]:
                        faulted[service] = False
                # The invariants.
                self.assertTrue(self.kvm.is_open(), round_)
                self.assertEqual(service_faults(), faults, round_)
                for terminal_id, shown in browser.output.items():
                    sent = sender.output_sent.get(terminal_id, b"")
                    self.assertEqual(bytes(sent[:len(shown)]), bytes(shown))
                for terminal_id, shell in sender.shells.items():
                    typed = browser.input.get(terminal_id, b"")
                    self.assertEqual(bytes(typed[:len(shell["input"])]), bytes(shell["input"]))
                for transfer_id in browser.open_transfers():
                    if transfer_id not in sender.ended:
                        self.assertEqual(browser.credit.get(transfer_id, 0),
                                         sender.transfers[transfer_id]["credit"]
                                         if transfer_id in sender.transfers else 0)
                # What ended at the KVM ended at the sender too: the KVM
                # cancelled or closed it, or the sender ended it itself.
                for transfer_id, drop_id in browser.drop_of.items():
                    if transfer_id in browser.done or drop_id in browser.channel_closed:
                        self.assertIn(transfer_id, sender.ended, round_)
                for terminal_id in browser.opened:
                    if terminal_id in browser.closed or terminal_id in browser.closed_by_browser:
                        self.assertNotIn(terminal_id, sender.shells, round_)
            # The session ends: whatever was open ends now, exactly once.
            LIB.shim_end(b"end of round")
            browser.poll()
            for terminal_id in browser.opened:
                if terminal_id not in browser.closed_by_browser:
                    self.assertIn(terminal_id, browser.closed, round_)
            self.assertEqual(browser.open_transfers(), [], round_)
            self.assertEqual(browser.poll(), [])


# The protocol's Video service section.
V_CAPS, V_CONFIG, V_STATUS, V_FRAME, V_KEYFRAME = 1, 3, 4, 5, 6
FLAG_KEY, FLAG_SETS = 1, 2
SPS = b"\0\0\0\x01\x67\x42\x00\x1f\xe9\x02"
PPS = b"\0\0\0\x01\x68\xce\x3c\x80"
# The KVM's sources (osvc_video_source_t).
PHYSICAL, TO_GUEST, GUEST, TO_PHYSICAL = range(4)


class VideoFrame(ctypes.Structure):
    _fields_ = [("length", ctypes.c_size_t), ("pts_90k", ctypes.c_uint32),
                ("arrival_us", ctypes.c_int64), ("received_us", ctypes.c_int64)] + [
        (name, ctypes.c_uint32) for name in ("width", "height", "fps", "keyframe_generation")] + [
        ("keyframe", ctypes.c_bool), ("parameter_sets_added", ctypes.c_bool)]


class FakeFrame(ctypes.Structure):
    _fields_ = [("meta", VideoFrame), ("hash", ctypes.c_uint32)]


class KeyframeDone(ctypes.Structure):
    _fields_ = [("generation", ctypes.c_uint32), ("delivered", ctypes.c_bool)]


class FakeVideo(ctypes.Structure):
    """tests/fake_video.h"""
    _fields_ = [
        ("presents", ctypes.c_uint), ("present_width", ctypes.c_uint32),
        ("present_height", ctypes.c_uint32), ("serials", ctypes.c_uint32 * 64),
        ("present_fails", ctypes.c_bool), ("restores", ctypes.c_uint),
        ("streams", ctypes.c_uint), ("stream_width", ctypes.c_uint32),
        ("stream_height", ctypes.c_uint32), ("stream_fps", ctypes.c_uint32),
        ("stream_refused", ctypes.c_bool), ("stopped", ctypes.c_uint),
        ("slot", ctypes.c_uint8 * (3 * 1024 * 1024)), ("capacity", ctypes.c_size_t),
        ("reserved", ctypes.c_bool), ("busy", ctypes.c_bool), ("no_viewer", ctypes.c_bool),
        ("reserves", ctypes.c_uint), ("cancels", ctypes.c_uint),
        ("cancel_written", ctypes.c_size_t), ("frames", FakeFrame * 256),
        ("frame_count", ctypes.c_size_t), ("refuse_commit", ctypes.c_bool),
        ("gaps", ctypes.c_uint), ("keyframe_pending", ctypes.c_uint32),
        ("keyframe_done", KeyframeDone * 64), ("keyframe_done_count", ctypes.c_size_t),
        ("manual", ctypes.c_bool), ("stale", ctypes.c_bool), ("estimate_kbps", ctypes.c_uint32),
        ("sent_kbps", ctypes.c_uint32), ("interval_ms", ctypes.c_int64),
        ("ceiling_kbps", ctypes.c_uint32), ("us_offset", ctypes.c_int64), ("now_ms", ctypes.c_void_p),
        ("clock_ctx", ctypes.c_void_p)]


class VideoCounters(ctypes.Structure):
    _fields_ = [("frames", ctypes.c_uint32), ("keyframes", ctypes.c_uint32),
                ("bytes", ctypes.c_uint64)] + [(name, ctypes.c_uint32) for name in (
                    "frames_discarded", "frames_dropped", "slot_waits", "slot_wait_max_ms",
                    "keyframe_requests", "parameter_sets_added", "stream_resets", "switches",
                    "switch_failures", "faults", "largest_keyframe", "slot_wait_ms")]


FAKE = FakeVideo.in_dll(LIB, "g_fake_video")
LIB.shim_video_size.restype = ctypes.c_size_t
assert LIB.shim_video_size() == ctypes.sizeof(FakeVideo), "fake_video.h changed"
LIB.shim_set_video.argtypes = [ctypes.c_int, ctypes.c_uint32]
LIB.shim_display_status.argtypes = [ctypes.c_int, ctypes.c_int]
LIB.shim_video_error.restype = ctypes.c_char_p
LIB.shim_video_counters.restype = ctypes.POINTER(VideoCounters)


def fnv1a(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def video_counters():
    return LIB.shim_video_counters().contents


def video_source():
    return LIB.shim_video_source()


class VideoSender(Sender):
    """The sender's video service, from the OS Services protocol. It checks each
    video.config, applies it, reports video.status, and sends one H.264
    access unit per frame: a keyframe with SPS and PPS first in each stream,
    and a keyframe after each request (with SPS and PPS only when
    sets_on_request). A mute sender applies configs without a status."""

    def __init__(self, kvm, max_size=(3840, 2160), max_fps=60, native_fps=60):
        super().__init__(kvm)
        self.max_size, self.max_fps, self.native_fps = max_size, max_fps, native_fps
        self.sets_on_request = True
        self.mute = False
        self.applied = None  # (width, height, kbps, fps) while enabled
        self.configs = []
        self.requests = 0
        self.keyframe_due = self.sets_due = False
        self.capture_us = 7_000_000

    def register(self):
        self.kvm.write(message(VIDEO, V_CAPS, struct.pack("<IIII", 1, *self.max_size,
                                                          self.max_fps)))
        self.poll()

    def withdraw(self):
        self.kvm.write(message(VIDEO, V_CAPS, bytes(16)))
        self.poll()

    def ping(self):
        assert self.kvm.write(message(SESSION, PING)) == 8

    def poll(self):
        for service, kind, payload in self.kvm.messages():
            assert service == VIDEO, (service, kind)
            if kind == V_CONFIG:
                self.apply(payload)
            else:
                assert (kind, payload) == (V_KEYFRAME, b""), kind
                self.requests += 1
                self.keyframe_due = True
                self.sets_due |= self.sets_on_request

    def apply(self, payload):
        assert len(payload) == 20
        enabled, layout, codec, reserved, width, height, kbps, fps = struct.unpack(
            "<BBBBIIII", payload)
        assert enabled in (0, 1) and layout in (0, 1) and (codec, reserved) == (0, 0)
        assert 0 < width <= self.max_size[0] and 0 < height <= self.max_size[1]
        assert 0 < fps <= self.max_fps and kbps > 0
        self.configs.append((enabled, width, height, kbps, fps))
        before, self.applied = self.applied, (width, height, kbps, fps) if enabled else None
        if self.mute or before == self.applied and enabled:
            return
        reset = enabled and before is None or before and before[:2] != (width, height)
        self.status(reset)

    def status(self, reset=False, error=0):
        """The applied state; a new stream starts with the next frame on
        reset."""
        if self.applied is None:
            body = struct.pack("<4B7I", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, error)
        else:
            width, height, kbps, fps = self.applied
            body = struct.pack("<4B7I", 1, 1, 0, int(bool(reset)), width, height,
                               self.native_fps, kbps, kbps, fps, 0)
            self.keyframe_due |= bool(reset)
            self.sets_due |= bool(reset)
        self.kvm.write(message(VIDEO, V_STATUS, body))

    def encode_frame(self, size=2000, gap_us=None):
        """The next video.frame and its access unit."""
        key, sets = self.keyframe_due, self.sets_due
        self.keyframe_due = self.sets_due = False
        self.capture_us += gap_us or 1_000_000 // self.applied[3]
        body = bytes(0x80 | ((self.capture_us + i) & 0x7F) for i in range(size))
        unit = (SPS + PPS if sets else b"") + b"\0\0\0\x01" + (b"\x65" if key else b"\x41") + body
        flags = (FLAG_KEY if key else 0) | (FLAG_SETS if sets else 0)
        return message(VIDEO, V_FRAME, struct.pack("<QI", self.capture_us, flags) + unit), unit

    def frame(self, size=2000, gap_us=None, chunk=None):
        data, unit = self.encode_frame(size, gap_us)
        assert self.kvm.write(data, chunk) == len(data)
        return unit

    def wait(self, ms):
        """Time passes at 100 ms ticks; a ping each second keeps the
        session."""
        for _ in range(ms // 100):
            self.kvm.advance(100)
            if self.kvm.now % 1000 == 0:
                self.ping()
            self.poll()


class VideoTest(unittest.TestCase):
    """The video service between VideoSender and the KVM's display and slot."""

    def setUp(self):
        self.kvm = Kvm(seed=random.getrandbits(63) | 1, video=True)
        self.sender = VideoSender(self.kvm)

    def show(self):
        """The switch to service video, up to the first keyframe."""
        self.sender.negotiate()
        self.sender.register()
        self.assertEqual((FAKE.presents, FAKE.present_width, FAKE.present_height),
                         (1, 1920, 1080))
        self.assertEqual(video_source(), TO_GUEST)
        self.assertEqual(self.sender.configs, [], "video.config before the display")
        LIB.shim_display_status(1, 1)
        self.sender.poll()
        self.assertEqual(self.sender.configs, [(1, 1920, 1080, 8000, 60)])
        self.assertEqual(video_source(), GUEST)
        self.assertEqual((FAKE.streams, FAKE.stream_width, FAKE.stream_height,
                          FAKE.stream_fps), (1, 1920, 1080, 60))
        self.check_frames([self.sender.frame()], first=0)

    def check_frames(self, units, first):
        self.assertEqual(FAKE.frame_count, first + len(units))
        for i, unit in enumerate(units):
            frame = FAKE.frames[first + i]
            self.assertEqual(frame.hash, fnv1a(unit), i)
            self.assertEqual(frame.meta.keyframe, unit[4] == 0x67 or unit[4] == 0x65, i)

    def test_frames_in_every_chunking_with_capture_spacing(self):
        rng = random.Random(5)
        for chunk in (1, 7, 12, 13, 4096, 65536):
            self.setUp()
            self.show()
            gaps = [rng.randint(1, 1_000_000) for _ in range(8)]
            units = [self.sender.frame(rng.randint(1, 20000), gap, chunk) for gap in gaps]
            self.check_frames(units, first=1)
            # pts follows the capture spacing (90 kHz, truncated).
            pts = [FAKE.frames[i].meta.pts_90k for i in range(9)]
            for i, gap in enumerate(gaps):
                self.assertIn(pts[i + 1] - pts[i] - gap * 9 // 100, (0, 1), (chunk, i))
            self.assertTrue(self.kvm.is_open())

    def test_keyframe_requests_and_parameter_sets(self):
        self.show()
        self.sender.sets_on_request = False
        FAKE.keyframe_pending = 5
        self.sender.wait(3100)
        # At most one request per 600 ms while none is answered.
        self.assertEqual(self.sender.requests, 6)
        unit = self.sender.frame()
        frame = FAKE.frames[1]
        self.assertTrue(frame.meta.keyframe and frame.meta.parameter_sets_added)
        self.assertEqual(frame.hash, fnv1a(SPS + PPS + unit))
        self.assertEqual(frame.meta.keyframe_generation, 5)
        done = FAKE.keyframe_done[FAKE.keyframe_done_count - 1]
        self.assertEqual((done.generation, done.delivered), (5, True))
        self.sender.wait(3000)
        self.assertEqual(self.sender.requests, 6)

    def test_backpressure_then_drop_after_the_slot_wait(self):
        self.show()
        FAKE.busy = True
        data, unit = self.sender.encode_frame(30000)
        used = self.kvm.write(data)
        self.assertEqual(used, 8 + 12)  # envelope and frame head; the bus NAKs
        self.assertTrue(LIB.shim_frame_waiting())
        self.kvm.advance(100)
        self.assertEqual(self.kvm.write(data[used:]), 0)
        FAKE.busy = False
        self.assertEqual(self.kvm.write(data[used:]), len(data) - used)
        self.check_frames([unit], first=1)
        self.assertEqual((video_counters().slot_waits, video_counters().slot_wait_max_ms),
                         (1, 100))
        FAKE.busy = True
        data, _ = self.sender.encode_frame(30000)
        used = self.kvm.write(data)
        self.kvm.advance(999)  # OSVC_VIDEO_SLOT_WAIT_MS less 1
        self.assertEqual(self.kvm.write(data[used:]), 0)
        self.kvm.advance(1)
        self.assertEqual(self.kvm.write(data[used:]), len(data) - used)
        self.assertEqual((FAKE.frame_count, FAKE.gaps, video_counters().frames_dropped),
                         (2, 1, 1))
        # The stream stays aligned.
        FAKE.busy = False
        self.check_frames([self.sender.frame()], first=2)

    def test_resize_starts_a_new_stream(self):
        self.show()
        LIB.shim_set_settings(1, 1, 1, b"", 1280, 720)
        self.assertEqual((FAKE.presents, FAKE.present_width), (2, 1280))
        self.assertNotEqual(FAKE.serials[0], FAKE.serials[1])
        self.sender.poll()
        self.assertEqual(len(self.sender.configs), 1, "video.config before the display")
        LIB.shim_display_status(1, 1)
        self.sender.poll()
        self.assertEqual(self.sender.configs[-1], (1, 1280, 720, 5000, 60))
        self.assertEqual((FAKE.streams, FAKE.stream_width, FAKE.stream_fps), (2, 1280, 60))
        units = [self.sender.frame() for _ in range(3)]
        self.check_frames(units, first=1)
        self.assertEqual(FAKE.frames[1].meta.width, 1280)
        self.assertEqual(video_counters().stream_resets, 2)

    def test_stream_reset_needs_a_keyframe_with_parameter_sets(self):
        self.show()
        self.sender.status(reset=True)
        self.sender.sets_due = False
        self.sender.frame()  # a keyframe without SPS and PPS
        self.assertEqual(LIB.shim_video_error(),
                         b"the first frame of a stream is not a keyframe with parameter sets")
        self.sender.poll()
        self.assertEqual(self.sender.configs[-1][0], 0)
        self.assertEqual((video_source(), FAKE.restores, FAKE.frame_count),
                         (TO_PHYSICAL, 1, 1))
        # New capabilities clear the fault.
        LIB.shim_display_status(0, 0)
        self.sender.register()
        self.assertEqual((FAKE.presents, LIB.shim_video_error()), (2, b""))

    def test_no_status_in_45_seconds(self):
        self.sender.mute = True
        self.sender.negotiate()
        self.sender.register()
        LIB.shim_display_status(1, 1)
        self.sender.wait(44900)
        self.assertEqual(video_source(), TO_GUEST)
        self.sender.wait(100)
        self.assertEqual((video_source(), FAKE.restores), (TO_PHYSICAL, 1))
        self.assertEqual([c[0] for c in self.sender.configs], [1, 0])
        self.assertIn(b"45 seconds", LIB.shim_video_error())
        LIB.shim_display_status(0, 0)
        self.assertEqual(video_source(), PHYSICAL)

    def test_disable_waits_for_the_status_or_15_seconds(self):
        for mute in (False, True):
            self.setUp()
            self.show()
            self.sender.mute = mute
            LIB.shim_set_video(0, 0)
            self.sender.poll()
            self.assertEqual(self.sender.configs[-1][0], 0)
            self.assertEqual(FAKE.restores, 0 if mute else 1)
            self.sender.wait(14900)
            self.assertEqual(FAKE.restores, 0 if mute else 1)
            self.sender.wait(100)
            self.assertEqual(FAKE.restores, 1)

    def test_retry_budget_across_sessions(self):
        for i in range(3):
            self.sender.negotiate()
            self.sender.register()
            self.assertEqual(FAKE.presents, i + 1)
            self.kvm.advance(30000)
            LIB.shim_display_status(0, 0)
        self.sender.negotiate()
        self.sender.register()
        self.assertEqual((FAKE.presents, self.sender.configs[-1][0]), (3, 0))
        self.assertIn(b"too often", LIB.shim_video_error())
        # Blocked past the window and into the next session.
        self.kvm.advance(120000)
        self.sender.negotiate()
        self.sender.register()
        self.assertEqual(FAKE.presents, 3)
        LIB.shim_set_video(0, 0)
        LIB.shim_set_video(1, 0)
        self.assertEqual(FAKE.presents, 4)


if __name__ == "__main__":
    unittest.main(verbosity=1, argv=sys.argv[:1])

"""The OS Services transport probe's checks, against a simulated KVM.

The probe runs on the target PC against the real interface
(tools/integration/tests/os_services_session.test.mjs runs it there); these
tests cover what it decides from the answers: the
status layout, the token and phase rules of the reset procedure, and the
requests that must stall.
"""

from __future__ import annotations

import contextlib
import io
import json
import unittest
from unittest.mock import patch

from tools.e2e import os_services_usb_probe as probe

READY_AFTER_POLLS = 3


def status_bytes(phase: int, token: bytes, magic: bytes = b"JKV5", reserved: bytes = b"\0\0\0") -> bytes:
    return magic + bytes([phase]) + reserved + token


class FakeKvm:
    """A KVM's RESET and STATUS answers, with switches for the ways one can
    break the OS Services protocol."""

    def __init__(self, **faults: object) -> None:
        self.faults = faults
        self.token = b""
        self.phase = probe.PHASE_RESETTING
        self.countdown = 0
        self.resets = 0
        self.requests: list[tuple[int, int, int]] = []

    def _token(self) -> bytes:
        if self.faults.get("zero_token"):
            return bytes(16)
        if self.faults.get("reuse_token") and self.resets == 3:
            return bytes([1]) * 16
        return bytes([self.resets]) * 16

    def _answer(self, phase: int, token: bytes) -> bytes:
        data = status_bytes(
            phase,
            token,
            magic=self.faults.get("magic", b"JKV5"),  # type: ignore[arg-type]
            reserved=self.faults.get("reserved", b"\0\0\0"),  # type: ignore[arg-type]
        )
        return data[: self.faults.get("length", len(data))]  # type: ignore[misc]

    def control_in(self, request: int, value: int, length: int) -> bytes:
        self.requests.append((request, value, length))
        known = request in (probe.REQUEST_RESET, probe.REQUEST_STATUS)
        if not self.faults.get("answers_everything") and (
            not known or value != probe.PROTOCOL_MAJOR or length != probe.STATUS_LEN
        ):
            raise probe.Stall()
        if request == probe.REQUEST_RESET:
            self.resets += 1
            self.token = self._token()
            self.phase = self.faults.get("reset_phase", probe.PHASE_RESETTING)  # type: ignore[assignment]
            self.countdown = READY_AFTER_POLLS
            return self._answer(self.phase, self.token)
        if self.faults.get("status_times_out"):
            raise probe.TransportError("control request 0x41: LIBUSB_ERROR_TIMEOUT")
        if self.phase == probe.PHASE_RESETTING and not self.faults.get("never_ready"):
            self.countdown -= 1
            if self.countdown <= 0:
                self.phase = self.faults.get("ready_phase", probe.PHASE_READY)  # type: ignore[assignment]
        token = bytes([0xEE]) * 16 if self.faults.get("status_other_token") else self.token
        return self._answer(self.phase, token)


class FakeClock:
    def __init__(self) -> None:
        self.now = 100.0

    def __call__(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds


def run(kvm: FakeKvm, resets: int = 3) -> probe.Report:
    clock = FakeClock()
    return probe.Prober(kvm, clock=clock, sleep=clock.sleep, poll_interval_s=0.5).run(resets)


def failed(report: probe.Report) -> list[str]:
    return [check.name for check in report.checks if not check.ok]


class ParseStatusTests(unittest.TestCase):
    def test_a_valid_answer(self) -> None:
        status = probe.parse_status(status_bytes(1, bytes(range(1, 17))))
        self.assertEqual(status.phase, probe.PHASE_READY)
        self.assertEqual(status.phase_name, "ready")
        self.assertEqual(status.token, bytes(range(1, 17)))

    def test_rejects_what_breaks_the_layout(self) -> None:
        token = bytes([7]) * 16
        cases = {
            "bytes, not 24": status_bytes(0, token)[:23],
            "magic": status_bytes(0, token, magic=b"JKV4"),
            "phase 4": status_bytes(4, token),
            "reserved": status_bytes(0, token, reserved=b"\0\1\0"),
            "all zeros": status_bytes(0, bytes(16)),
        }
        for message, data in cases.items():
            with self.subTest(message), self.assertRaisesRegex(probe.ProtocolError, message):
                probe.parse_status(data)


class ProberTests(unittest.TestCase):
    def test_a_conforming_kvm_passes(self) -> None:
        kvm = FakeKvm()
        report = run(kvm)
        self.assertEqual(failed(report), [])
        self.assertTrue(report.ok)
        tokens = [record.token for record in report.resets]
        self.assertEqual(len(set(tokens)), 3)
        for record in report.resets:
            self.assertEqual(record.reset_phase, "resetting")
            self.assertEqual(record.polls, READY_AFTER_POLLS)
            self.assertEqual(record.phases_seen, ["resetting", "ready"])
            self.assertEqual(record.ready_ms, 1000.0)
        names = [check.name for check in report.checks]
        for what, _, _, _ in probe.STALL_CASES:
            self.assertIn(f"{what} stalls", names)
        self.assertIn("a stalled request leaves the session ready with the same token", names)
        # Every request went to the interface with the major version, as a
        # sender sends them, except the three that must stall.
        stalls = [(request, value, length) for _, request, value, length in probe.STALL_CASES]
        self.assertEqual([r for r in kvm.requests if r in stalls], stalls)
        self.assertEqual(len(report.control_rtt_ms), len(kvm.requests))

    def test_a_reused_token_fails(self) -> None:
        report = run(FakeKvm(reuse_token=True))
        self.assertEqual(failed(report), ["RESET 3 makes a new token"])

    def test_a_status_with_another_token_fails(self) -> None:
        report = run(FakeKvm(status_other_token=True))
        self.assertEqual(failed(report), ["STATUS after RESET 1 carries its token"])
        self.assertEqual(len(report.resets), 1, "the probe stops at the first broken RESET")

    def test_a_kvm_that_never_gets_ready_fails_after_15_seconds(self) -> None:
        report = run(FakeKvm(never_ready=True))
        self.assertEqual(failed(report), ["STATUS after RESET 1 reaches phase 1 (ready)"])
        self.assertIn("after 15.0 s", report.checks[-1].detail)

    def test_reset_must_answer_resetting(self) -> None:
        report = run(FakeKvm(reset_phase=probe.PHASE_READY))
        self.assertIn("RESET 1 answers phase 0 (resetting)", failed(report))

    def test_a_faulted_kvm_fails_and_stops(self) -> None:
        kvm = FakeKvm(reset_phase=probe.PHASE_FAULTED)
        report = run(kvm)
        self.assertEqual(failed(report), ["RESET 1 answers phase 0 (resetting)"])
        self.assertIn("faulted", report.checks[0].detail)
        self.assertEqual(kvm.resets, 1)

    def test_an_active_phase_fails(self) -> None:
        report = run(FakeKvm(ready_phase=probe.PHASE_ACTIVE))
        self.assertEqual(failed(report), ["STATUS after RESET 1 reaches phase 1 (ready)"])

    def test_an_answered_unknown_request_fails(self) -> None:
        # A KVM that accepts the RESET for another major version also makes
        # a new token, which the STATUS after the stalls finds.
        report = run(FakeKvm(answers_everything=True))
        self.assertEqual(
            failed(report),
            [f"{what} stalls" for what, _, _, _ in probe.STALL_CASES]
            + ["a stalled request leaves the session ready with the same token"],
        )

    def test_an_invalid_answer_fails(self) -> None:
        for fault, value in {
            "magic": b"XXXX",
            "reserved": b"\0\0\1",
            "length": 20,
            "zero_token": True,
        }.items():
            with self.subTest(fault):
                report = run(FakeKvm(**{fault: value}))
                self.assertEqual(failed(report), ["RESET 1 is a valid answer"])

    def test_a_transport_error_is_a_failed_check(self) -> None:
        report = run(FakeKvm(status_times_out=True))
        self.assertEqual(failed(report), ["STATUS after RESET 1 is answered"])
        self.assertIn("TIMEOUT", report.checks[-1].detail)


class DescriptorTests(unittest.TestCase):
    BULK_OUT = probe.Endpoint(0x05, 0x02, 512)
    BULK_IN = probe.Endpoint(0x85, 0x02, 512)

    def test_one_bulk_in_and_one_bulk_out(self) -> None:
        self.assertIsNone(probe.endpoint_problem([self.BULK_OUT, self.BULK_IN]))
        self.assertEqual(probe.endpoint_problem([self.BULK_IN]), "1 endpoints, not 2")
        self.assertEqual(
            probe.endpoint_problem([self.BULK_IN, probe.Endpoint(0x86, 0x02, 512)]),
            "the endpoints are not one IN and one OUT",
        )
        self.assertEqual(
            probe.endpoint_problem([self.BULK_OUT, probe.Endpoint(0x85, 0x03, 64)]),
            "an endpoint is not bulk",
        )

    def test_the_interface_triple(self) -> None:
        self.assertTrue(probe.is_os_services_interface(0xFF, 0x4A, 0x01))
        self.assertFalse(probe.is_os_services_interface(0xFF, 0x4A, 0x02))
        self.assertFalse(probe.is_os_services_interface(0xFF, 0x00, 0x01))

    def candidate(self, bus: int, address: int) -> probe.Candidate:
        return probe.Candidate(bus, address, 0x399B, 0x0001, 4, (self.BULK_OUT, self.BULK_IN))

    def test_selects_one_kvm(self) -> None:
        one, two = self.candidate(1, 5), self.candidate(3, 2)
        self.assertEqual(probe.select_candidate([one], None, None), one)
        self.assertEqual(probe.select_candidate([one, two], 3, 2), two)
        with self.assertRaisesRegex(probe.ProbeUnavailable, "several"):
            probe.select_candidate([one, two], None, None)
        with self.assertRaisesRegex(probe.ProbeUnavailable, "at bus 1 address 9"):
            probe.select_candidate([one, two], 1, 9)
        with self.assertRaisesRegex(probe.ProbeUnavailable, "no USB device"):
            probe.select_candidate([], None, None)


class MainTests(unittest.TestCase):
    def main(self, *args: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = probe.main(list(args))
        return status, out.getvalue(), err.getvalue()

    def test_unavailable_exits_2(self) -> None:
        with patch.object(probe, "probe_device", side_effect=probe.ProbeUnavailable("busy")):
            status, out, err = self.main("--json")
        self.assertEqual(status, 2)
        self.assertEqual(json.loads(out), {"ok": False, "unavailable": "busy"})
        self.assertIn("busy", err)

    def test_reports_checks_and_device(self) -> None:
        candidate = DescriptorTests().candidate(1, 5)
        for kvm, expected in ((FakeKvm(), 0), (FakeKvm(reuse_token=True), 1)):
            with self.subTest(expected=expected):
                with patch.object(probe, "probe_device", return_value=(candidate, run(kvm))):
                    status, out, err = self.main("--json")
                self.assertEqual(status, expected)
                report = json.loads(out)
                self.assertEqual(report["ok"], expected == 0)
                self.assertEqual(report["device"]["id"], "399b:0001")
                self.assertEqual(
                    report["checks"][0]["name"],
                    "the interface has one bulk IN and one bulk OUT endpoint",
                )
                self.assertTrue(report["checks"][0]["ok"])
                self.assertIn("PASS RESET 1 answers phase 0 (resetting)", err)

    def test_needs_two_resets(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            probe.parse_args(["--resets", "1"])


if __name__ == "__main__":
    unittest.main()

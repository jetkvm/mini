"""Serial-log markers of a firmware crash."""

from __future__ import annotations


FATAL_SERIAL_MARKERS = (
    "***ERROR*** A stack overflow",
    "Guru Meditation Error",
    "assert failed:",
    "CORRUPT HEAP",
    "heap corruption",
    "panic'ed",
    "Task watchdog got triggered",
    "Interrupt wdt timeout",
    "Brownout detector was triggered",
)


def fatal_serial_markers(text: str) -> list[str]:
    return [marker for marker in FATAL_SERIAL_MARKERS if marker in text]

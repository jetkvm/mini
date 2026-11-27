"""tools/timezones/generate.py against zone files that the test writes.

A zone file here is the smallest TZif version 2 file: the version 1 block
with no content, the version 2 block with the local time types and the
changes, and the rule line. The test machine needs no tzdata.
"""
from datetime import datetime, timezone
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).parents[1] / "timezones/generate.py"


def tzif(rule, types=((0, False),), changes=()):
    """types: (seconds east of UTC, daylight time); changes: (UTC seconds, index of the type from then on)."""
    def block(version_one):
        header = b"TZif2" + bytes(15)
        if version_one:
            return header + struct.pack(">6l", 0, 0, 0, 0, 1, 1) + struct.pack(">lBB", 0, 0, 0) + b"\0"
        body = b"".join(struct.pack(">q", when) for when, _ in changes)
        body += bytes(index for _, index in changes)
        body += b"".join(struct.pack(">lBB", offset, daylight, 0) for offset, daylight in types)
        return header + struct.pack(">6l", 0, 0, 0, len(changes), len(types), 4) + body + b"LMT\0"
    return block(True) + block(False) + b"\n" + rule.encode() + b"\n"


def utc(year, month, day):
    return int(datetime(year, month, day, tzinfo=timezone.utc).timestamp())


class GenerateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(subprocess.run, ["rm", "-rf", str(self.tmp)])
        self.zones = self.tmp / "zoneinfo"
        self.output = self.tmp / "table.h"
        self.write("UTC", tzif("UTC0"))
        self.write("Europe/Stockholm", tzif("CET-1CEST,M3.5.0,M10.5.0/3"))
        self.write("America/Argentina/Ushuaia", tzif("<-03>3"))
        (self.zones / "tzdata.zi").write_text("# version 2026x\n# other lines\n")

    def write(self, name, content):
        path = self.zones / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content if isinstance(content, bytes) else content.encode())

    def generate(self, *args, since="2026-11-27"):
        dated = ["--since", since] if since else []
        return subprocess.run([sys.executable, str(SCRIPT), "--zoneinfo", str(self.zones), "--output", str(self.output),
                               *dated, *args], capture_output=True, text=True, timeout=120)

    def test_table_has_each_zone_with_its_rule_in_the_order_of_the_search(self):
        # Not zones: copies in other time scales, the machine's own zone, a
        # placeholder, and files that are not zone files.
        self.write("posix/Europe/Stockholm", tzif("CET-1CEST,M3.5.0,M10.5.0/3"))
        self.write("right/UTC", tzif("UTC0"))
        self.write("localtime", tzif("CET-1CEST,M3.5.0,M10.5.0/3"))
        self.write("Factory", tzif("<-00>0"))
        self.write("zone.tab", "SE\t+5920+01803\tEurope/Stockholm\n")
        # "Europe/a" sorts after "Europe/Stockholm" for strcmp().
        self.write("Europe/a", tzif("EET-2"))
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        text = self.output.read_text()
        self.assertIn("from tzdata 2026x;", text)
        self.assertIn(" * 4 zones,", text)
        self.assertIn("over the 5 years from 2026-11-27.\n * They agree.\n", text)
        rows = [line.strip() for line in text.splitlines() if line.startswith("    {")]
        self.assertEqual(rows, [
            '{"America/Argentina/Ushuaia", "<-03>3"},',
            '{"Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"},',
            '{"Europe/a", "EET-2"},',
            '{"UTC", "UTC0"},',
        ])

    def test_header_names_a_zone_whose_rule_is_not_yet_its_time(self):
        # One hour ahead of UTC until a change in 2027; the rule gives UTC.
        self.write("Asia/Late", tzif("AAA0", types=((3600, False), (0, False)), changes=((utc(2027, 6, 1), 1),)))
        # The change is before --since, so the rule is the zone's time.
        self.write("Asia/Past", tzif("AAA0", types=((3600, False), (0, False)), changes=((utc(2026, 6, 1), 1),)))
        result = self.generate()
        self.assertEqual(result.returncode, 0, result.stderr)
        text = self.output.read_text()
        self.assertIn("over the 5 years from 2026-11-27.\n * They differ at some time for: Asia/Late.\n", text)
        self.assertIn('{"Asia/Late", "AAA0"},', text)

    def test_rule_that_the_firmware_does_not_read_stops_the_generator(self):
        for rule in ("AAA0BBB,J60,J300", "AAA0BBB,60,300", "AAA0BBB", "AA0", "AAA0BBB,M3.5.0", ":UTC"):
            self.write("Asia/Other", tzif(rule))
            result = self.generate()
            self.assertNotEqual(result.returncode, 0, rule)
            self.assertIn("Asia/Other", result.stderr)
            self.assertFalse(self.output.exists(), rule)

    def test_zone_file_without_a_rule_line_stops_the_generator(self):
        self.write("Asia/Old", tzif("UTC0")[:-1])
        result = self.generate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no rule line", result.stderr)

    def test_no_zone_files_stops_the_generator(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "--zoneinfo", str(self.tmp / "none"),
                                 "--output", str(self.output)], capture_output=True, text=True, timeout=120)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no zone files", result.stderr)

    def test_check_compares_the_header_and_writes_nothing(self):
        self.assertEqual(self.generate().returncode, 0)
        written = self.output.read_text()
        self.assertEqual(self.generate("--check").returncode, 0)
        self.write("Asia/Tokyo", tzif("JST-9"))
        result = self.generate("--check")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("is not what", result.stderr)
        self.assertEqual(self.output.read_text(), written)

    def test_check_without_a_day_compares_from_the_day_of_the_header(self):
        # The rule differs from the zone file until June 2027: a comparison
        # from a day after that would not name the zone.
        self.write("Asia/Late", tzif("AAA0", types=((3600, False), (0, False)), changes=((utc(2027, 6, 1), 1),)))
        self.assertEqual(self.generate().returncode, 0)
        self.assertIn("differ at some time for: Asia/Late.", self.output.read_text())
        result = self.generate("--check", since=None)
        self.assertEqual(result.returncode, 0, result.stderr)
        # A day that is given is used.
        self.assertNotEqual(self.generate("--check", since="2027-07-01").returncode, 0)
        self.assertEqual(self.generate("--check", since="2026-11-27").returncode, 0)
        # Without --check, the day is today, also over a header of another day.
        self.assertEqual(self.generate(since="2027-07-01").returncode, 0)
        self.assertIn("years from 2027-07-01.\n * They agree.", self.output.read_text())
        today = lambda: f"years from {datetime.now(timezone.utc):%Y-%m-%d}."
        before = today()
        self.assertEqual(self.generate(since=None).returncode, 0)
        self.assertTrue(any(day in self.output.read_text() for day in (before, today())))


if __name__ == "__main__":
    unittest.main()

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import release_check  # noqa: E402
from release_check import Version  # noqa: E402


def existing(*names: str) -> list[Version]:
    return release_check.parse_existing(f"{name}/\n" for name in names)


class OrderingTest(unittest.TestCase):
    def test_orders_as_the_semver_package_does(self):
        ordered = [
            "0.5.1-dev.1",
            "0.5.1-dev.2",
            "0.5.1-dev.10",
            "0.5.1-dev202609190000",
            "0.5.1-dev202609191200",
            "0.5.1",
            "0.5.2-dev202609190000",
            "0.5.2",
            "0.6.0",
            "1.0.0",
        ]
        for lower, higher in zip(ordered, ordered[1:]):
            self.assertLess(Version(lower), Version(higher), f"{lower} < {higher}")

    def test_ignores_folders_that_are_not_semver(self):
        self.assertEqual([v.text for v in existing("0.5.1", "latest", "", "skus")], ["0.5.1"])


class CheckTest(unittest.TestCase):
    def test_prerelease_below_the_shipped_stable_is_refused(self):
        reason = release_check.check(Version("0.5.1-dev202609190000"), existing("0.5.1-dev.2", "0.5.1"))
        self.assertIn("below stable 0.5.1", reason)

    def test_prerelease_below_a_newer_prerelease_is_refused(self):
        reason = release_check.check(
            Version("0.5.2-dev202609190000"), existing("0.5.1", "0.5.2-dev202609191200")
        )
        self.assertIn("below prerelease 0.5.2-dev202609191200", reason)

    def test_prerelease_above_everything_is_accepted(self):
        self.assertIsNone(
            release_check.check(Version("0.5.2-dev202609190000"), existing("0.5.1-dev.2", "0.5.1"))
        )

    def test_stable_below_the_latest_stable_is_refused(self):
        reason = release_check.check(Version("0.5.1"), existing("0.5.2"))
        self.assertIn("below stable 0.5.2", reason)

    def test_stable_below_a_prerelease_of_the_next_version_is_accepted(self):
        self.assertIsNone(
            release_check.check(Version("0.5.2"), existing("0.5.1", "0.5.2-dev202609190000", "0.6.0-dev202609190000"))
        )

    def test_existing_version_is_refused(self):
        self.assertEqual(release_check.check(Version("0.5.1"), existing("0.5.1")), "0.5.1 already exists")

    def test_first_release_is_accepted(self):
        self.assertIsNone(release_check.check(Version("0.5.1-dev202609190000"), []))

    def test_stable_below_a_newer_prerelease_leaves_the_dev_channel_on_it(self):
        lines = release_check.channels(Version("0.5.2"), existing("0.5.1", "0.6.0-dev202609190000"))
        self.assertIn("keeps offering 0.6.0-dev202609190000", lines[0])
        self.assertIn("release-sync workflow", lines[1])

    def test_stable_above_everything_reaches_both_channels(self):
        lines = release_check.channels(Version("0.5.2"), existing("0.5.1", "0.5.2-dev202609190000"))
        self.assertIn("offers 0.5.2 within 5 minutes", lines[0])
        self.assertEqual(len(lines), 2)

    def test_prerelease_reaches_the_dev_channel_only(self):
        lines = release_check.channels(Version("0.5.3-dev202609190000"), existing("0.5.2"))
        self.assertEqual(len(lines), 1)
        self.assertIn("offers 0.5.3-dev202609190000 within 5 minutes", lines[0])

    def test_summary_names_the_latest_of_each_channel(self):
        lines = release_check.summary(existing("0.5.1-dev.1", "0.5.1", "0.5.2-dev202609190000"))
        self.assertEqual(lines[0].split()[-1], "0.5.1")
        self.assertEqual(lines[1].split()[-1], "0.5.2-dev202609190000")


if __name__ == "__main__":
    unittest.main()

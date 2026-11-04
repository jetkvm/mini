#!/usr/bin/env python3
"""Decide whether a release version can be served once it is on R2.

api.jetkvm.com answers an update check with semver.maxSatisfying over every
version folder under mini/. The dev channel includes prereleases, the stable
channel does not. A version that is not the new maximum of its channel is
uploaded and never served, so a release must be greater than:

  prerelease  every version already on R2, stable versions included
  stable      every stable version already on R2

Usage: release_check.py <version> < versions.txt   one existing version per
line, as `rclone lsf --dirs-only` prints them. Prints the latest stable and
prerelease on R2 and what each channel will offer, or exits 1 with a reason
when the version cannot be served.
"""

from __future__ import annotations

import re
import sys
from typing import Iterable

SEMVER = re.compile(
    r"^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
    r"(?:-((?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*))?"
    r"(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$"
)


class Version:
    """A semver 2.0.0 version ordered as the `semver` npm package orders it."""

    def __init__(self, text: str):
        match = SEMVER.match(text)
        if not match:
            raise ValueError(f"not semver: {text}")
        self.text = text
        self.release = tuple(int(part) for part in match.group(1, 2, 3))
        tag = match.group(4)
        self.prerelease = tuple(tag.split(".")) if tag else ()

    @property
    def is_prerelease(self) -> bool:
        return bool(self.prerelease)

    def _key(self):
        # A version without a prerelease tag is greater than one with a tag;
        # numeric identifiers are lower than alphanumeric ones; a shorter tag
        # with equal leading identifiers is lower.
        identifiers = tuple(
            (0, int(part), "") if part.isdigit() else (1, 0, part) for part in self.prerelease
        )
        return (self.release, 0 if self.prerelease else 1, identifiers)

    def __lt__(self, other: "Version") -> bool:
        return self._key() < other._key()

    def __eq__(self, other: object) -> bool:
        return isinstance(other, Version) and self._key() == other._key()

    def __repr__(self) -> str:
        return self.text


def parse_existing(lines: Iterable[str]) -> list[Version]:
    """Version folders on R2; names that are not semver are ignored, as the
    cloud does."""
    versions = []
    for line in lines:
        name = line.strip().rstrip("/")
        if not name:
            continue
        try:
            versions.append(Version(name))
        except ValueError:
            continue
    return versions


def latest(versions: Iterable[Version]) -> Version | None:
    versions = list(versions)
    return max(versions) if versions else None


def check(version: Version, existing: list[Version]) -> str | None:
    """The reason the version can never be served, or None when it can."""
    if any(version == other for other in existing):
        return f"{version} already exists"
    if version.is_prerelease:
        newest = latest(existing)
        if newest is not None and version < newest:
            what = "prerelease" if newest.is_prerelease else "stable"
            return (
                f"{version} is below {what} {newest}; the dev channel serves the "
                f"highest version on R2 and would never offer it"
            )
        return None
    newest_stable = latest(v for v in existing if not v.is_prerelease)
    if newest_stable is not None and version < newest_stable:
        return f"{version} is below stable {newest_stable}; the stable channel would never offer it"
    return None


CACHE_MINUTES = 5  # releaseCache TTL in cloud-api src/releases.ts


def summary(existing: list[Version]) -> list[str]:
    newest_stable = latest(v for v in existing if not v.is_prerelease)
    newest_prerelease = latest(v for v in existing if v.is_prerelease)
    return [
        f"latest stable on R2:      {newest_stable or '-'}",
        f"latest prerelease on R2:  {newest_prerelease or '-'}",
    ]


def channels(version: Version, existing: list[Version]) -> list[str]:
    """What each channel offers once the version is on R2 and the cloud's
    cache has expired. Assumes check() accepted the version."""
    newest = latest(existing)
    if newest is not None and version < newest:
        dev = f"dev channel:     keeps offering {newest}; {version} is below it"
    else:
        dev = f"dev channel:     offers {version} within {CACHE_MINUTES} minutes of the upload"
    if version.is_prerelease:
        return [dev]
    return [dev, f"stable channel:  offers {version} at 10% rollout once the release-sync workflow registers it"]


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    try:
        version = Version(argv[1])
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    existing = parse_existing(sys.stdin)
    for line in summary(existing):
        print(line)
    reason = check(version, existing)
    if reason:
        print(reason, file=sys.stderr)
        return 1
    for line in channels(version, existing):
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

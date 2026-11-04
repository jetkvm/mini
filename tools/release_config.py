#!/usr/bin/env python3
"""Check that a build directory holds a release configuration.

A release image is configured from sdkconfig.defaults and
sdkconfig.<variant>.defaults only, with its sdkconfig in the build directory,
the test CMake options off, and no option from another sdkconfig.*.defaults
or BENCH_ONLY.

Usage: release_config.py <repository> <build directory> <variant> <variant>...
The variants after the first are all the board variants of the release, the
first one included. Prints one line per difference and exits 1, or prints
nothing and exits 0.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

# Test-only options that no committed defaults file names.
BENCH_ONLY = {
    "JETKVM_VIRTUAL_MEDIA_HTTP_DIAGNOSTIC_DISCARD": True,
    "JETKVM_USB_DEBUGGING_DEFAULT_ON": True,
    "JETKVM_BENCH_CUSTOM_MAC": True,
}

# Test-only CMake options and their release values.
RELEASE_CACHE = {"JETKVM_MOCK_CLOUD_JWKS_URL": "", "JETKVM_WIFI_SETUP_LOG_CODE": "0"}


def defaults_file_options(path: Path) -> dict[str, object]:
    """The options a defaults file sets, with the values sdkconfig.json gives them."""
    options: dict[str, object] = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line.startswith("CONFIG_") or "=" not in line:
            continue
        name, text = line[len("CONFIG_"):].split("=", 1)
        if text in ("y", "n"):
            options[name] = text == "y"
        elif text.startswith('"') and text.endswith('"') and len(text) >= 2:
            options[name] = text[1:-1]
        else:
            try:
                options[name] = int(text, 0)
            except ValueError:
                options[name] = text
    return options


def bench_options(repository: Path, variants: list[str]) -> dict[str, tuple[object, str]]:
    """Option name to (value, file name) for every value only a bench build has.

    A value that a release defaults file also sets belongs to the release.
    """
    release_files = [repository / "sdkconfig.defaults"]
    release_files += [repository / f"sdkconfig.{variant}.defaults" for variant in variants]
    released = set()
    for path in release_files:
        released.update(defaults_file_options(path).items())
    found: dict[str, tuple[object, str]] = {}
    for path in sorted(repository.glob("sdkconfig.*.defaults")):
        if path in release_files:
            continue
        for name, value in defaults_file_options(path).items():
            if (name, value) not in released:
                found[name] = (value, path.name)
    return found


def cmake_cache(path: Path) -> dict[str, str]:
    cache: dict[str, str] = {}
    for line in path.read_text().splitlines():
        if line.startswith(("#", "//")) or "=" not in line or ":" not in line.split("=", 1)[0]:
            continue
        key, value = line.split("=", 1)
        cache[key.split(":", 1)[0]] = value
    return cache


def sdkconfig_line(name: str, value: object) -> str:
    if isinstance(value, bool):
        return f"CONFIG_{name}={'y' if value else 'n'}"
    if isinstance(value, str):
        return f'CONFIG_{name}="{value}"'
    return f"CONFIG_{name}={value}"


def differences(repository: Path, build: Path, variant: str, variants: list[str]) -> list[str]:
    """Why the build directory does not hold the release configuration of the variant."""
    reasons: list[str] = []
    try:
        cache = cmake_cache(build / "CMakeCache.txt")
        description = json.loads((build / "project_description.json").read_text())
        config = json.loads((build / "config" / "sdkconfig.json").read_text())
    except (OSError, ValueError) as error:
        return [f"cannot read the configuration of {build}: {error}"]

    own = str(build / "sdkconfig")
    if cache.get("SDKCONFIG") != own:
        reasons.append(f"SDKCONFIG is '{cache.get('SDKCONFIG', '')}', not {own}")
    if cache.get("JETKVM_BOARD_VARIANT") != variant:
        reasons.append(f"JETKVM_BOARD_VARIANT is '{cache.get('JETKVM_BOARD_VARIANT', '')}', not {variant}")
    for name, value in RELEASE_CACHE.items():
        if cache.get(name) != value:
            reasons.append(f"{name} is '{cache.get(name, '')}', which only a bench build may have")

    config_file = Path(description.get("config_file", ""))
    if config_file.parent != build:
        reasons.append(f"the configuration file is {config_file}, outside {build}")
    expected = [str(repository / "sdkconfig.defaults"), str(repository / f"sdkconfig.{variant}.defaults")]
    used = [item for item in description.get("config_defaults", "").split(";") if item]
    if used != expected:
        reasons.append(f"the defaults are {', '.join(used) or 'none'}; a release uses {' and '.join(expected)}")

    for name, (value, file_name) in sorted(bench_options(repository, variants).items()):
        if name in config and config[name] == value:
            reasons.append(f"{sdkconfig_line(name, value)} is set, which only a bench build may have ({file_name})")
    for name, value in sorted(BENCH_ONLY.items()):
        if config.get(name) == value:
            reasons.append(f"{sdkconfig_line(name, value)} is set, which only a bench build may have")
    return reasons


def main(argv: list[str]) -> int:
    if len(argv) < 5:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    repository, build, variant = Path(argv[1]), Path(argv[2]), argv[3]
    reasons = differences(repository, build, variant, argv[4:])
    for reason in reasons:
        print(reason)
    return 1 if reasons else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))

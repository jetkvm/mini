#!/usr/bin/env python3
"""Write THIRD_PARTY_NOTICES.md: the third-party code a release ships and the
license texts that come with it.

The firmware part comes from the link maps of the application and the
bootloader: an object counts when it puts at least one byte into the image.
Each linked object is traced back to its source file, and the source file to
the nearest license file above it. A source with no license file above it is
judged by its own header: Espressif Apache-2.0 files fall under ESP-IDF's
LICENSE, and any other header is quoted as it is.

The web UI part comes from the source maps of a device UI build: an npm
package counts when one of its files is in the bundle.

The C5 coprocessor firmware is a prebuilt image and the fonts are assets, so
both are listed by hand below (C5_PARTS, FONTS).

Run by tools/third_party_notices.sh inside espressif/idf, which builds what
this reads. Usage:

  third_party_notices.py --build <dir> [--build <dir> ...] --ui-dist <dir>
      --node-modules <dir> [--output <file>]
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
IDF = Path(os.environ.get("IDF_PATH", "/opt/esp/idf"))

LICENSE_FILE = re.compile(r"(?i)^(licen[cs]e|copying|notice)([.-].*)?$")
# Directories at which the search for a license file stops.
STOP_DIRS = {IDF, IDF / "components", REPO, REPO / "components",
             REPO / "third_party", REPO / "managed_components"}
ESPRESSIF = re.compile(r"(?i)espressif systems")
# A comment in CMakeLists.txt such as "third_party/microlink is exported from
# jetkvm/microlink at <revision>"; the notices link that repository.
EXPORTED = re.compile(r"#\s*(third_party/\S+) is exported from ([\w.-]+/[\w.-]+) at ([0-9a-f]+)")

# Hand-listed parts of the ESP32-C5 network adapter image
# (components/jetkvm_c5_wifi/firmware/README.md): name, license files
# relative to IDF_PATH or the repository, and where the source is.
C5_PARTS = [
    ("ESP-Hosted-MCU 2.12.3 (slave firmware)",
     ["managed_components/espressif__esp_hosted/LICENSE"],
     "https://github.com/espressif/esp-hosted-mcu"),
    ("ESP-IDF v5.5.4", ["$IDF/LICENSE"], "https://github.com/espressif/esp-idf/tree/v5.5.4"),
    ("FreeRTOS kernel (in ESP-IDF v5.5.4)", ["$IDF/components/freertos/FreeRTOS-Kernel/LICENSE.md"],
     "https://github.com/espressif/esp-idf/tree/v5.5.4/components/freertos"),
    ("lwIP (in ESP-IDF v5.5.4)", ["$IDF/components/lwip/lwip/COPYING"],
     "https://github.com/espressif/esp-idf/tree/v5.5.4/components/lwip"),
    ("Mbed TLS (in ESP-IDF v5.5.4)", ["$IDF/components/mbedtls/mbedtls/LICENSE"],
     "https://github.com/espressif/esp-idf/tree/v5.5.4/components/mbedtls"),
    ("wpa_supplicant (in ESP-IDF v5.5.4)", ["$IDF/components/wpa_supplicant/COPYING"],
     "https://github.com/espressif/esp-idf/tree/v5.5.4/components/wpa_supplicant"),
    ("Wi-Fi, PHY, coexistence and Bluetooth controller libraries (in ESP-IDF v5.5.4)",
     ["$IDF/components/esp_wifi/lib/LICENSE"],
     "https://github.com/espressif/esp-idf/tree/v5.5.4/components/esp_wifi"),
]

FONTS = [
    ("Inter (web UI)", ["frontend/jetkvm-ui/src/assets/fonts/OFL.txt"], "https://rsms.me/inter/"),
    ("Inter (display)", ["components/jetkvm_screen/ui/fonts/inter/OFL.txt"], "https://rsms.me/inter/"),
]


@dataclass
class Entry:
    name: str
    version: str = ""
    url: str = ""
    license: str = ""
    texts: list[str] = field(default_factory=list)


# --- firmware ---------------------------------------------------------------

def linked_members(map_path: Path) -> set[tuple[str, str]]:
    """(archive, member) pairs that put a nonzero-sized section into the image."""
    text = map_path.read_text(errors="replace")
    memory_map = text[text.index("Linker script and memory map"):]
    pairs = set()
    for match in re.finditer(
            r"^\s*(?:\S+\s+)?0x[0-9a-f]+\s+0x([0-9a-f]+)\s+(\S+\.a)\(([^)]+)\)",
            memory_map, re.M):
        if int(match.group(1), 16):
            pairs.add((match.group(2), match.group(3)))
    return pairs


def linked_files(build: Path, map_path: Path) -> tuple[set[Path], set[Path]]:
    """Source files of the linked objects, and prebuilt archives linked."""
    objects = collections.defaultdict(list)
    for command in json.loads((build / "compile_commands.json").read_text()):
        output = command.get("output") or re.search(r"-o (\S+)", command["command"]).group(1)
        directory = Path(command.get("directory", build))
        output = Path(os.path.normpath(directory / output))
        objects[(output.as_posix().split("/CMakeFiles/")[0], output.name)].append(
            Path(os.path.normpath(directory / command["file"])))
    sources, archives = set(), set()
    for archive, member in linked_members(map_path):
        archive = Path(os.path.normpath(build / archive))
        found = objects.get((archive.parent.as_posix(), member))
        if found:
            sources.update(found)
        else:
            archives.add(archive)
    return sources, archives


def governing_license(path: Path) -> Path | None:
    directory = path.parent
    while True:
        if directory != REPO and directory.is_dir() and any(
                LICENSE_FILE.match(f.name) and f.is_file() for f in directory.iterdir()):
            return directory
        if directory in STOP_DIRS or directory == directory.parent:
            return None
        directory = directory.parent


def license_texts(directory: Path) -> list[str]:
    """The license files of a directory and of every REUSE-style LICENSES/
    below it, each text once."""
    files = [f for f in sorted(directory.iterdir()) if LICENSE_FILE.match(f.name) and f.is_file()]
    for licenses in sorted(directory.rglob("LICENSES")):
        files += sorted(f for f in licenses.iterdir() if f.is_file())
    texts = {}
    for f in files:
        text = f.read_text(errors="replace")
        texts.setdefault(" ".join(text.split()), text)
    return list(texts.values())


def header_comment(path: Path) -> str:
    """The comments before a source file's first line of code, without
    comment markers."""
    text = path.read_text(errors="replace")
    blocks = []
    while True:
        block = re.match(r"\s*/\*(.*?)\*/", text, re.S)
        if block:
            blocks.append("\n".join(re.sub(r"^\s*\* ?", "", line)
                                    for line in block.group(1).splitlines()).strip())
        else:
            block = re.match(r"(\s*//[^\n]*\n)+", text)
            if not block:
                return "\n\n".join(blocks)
            blocks.append("\n".join(re.sub(r"^\s*// ?", "", line)
                                    for line in block.group(0).strip().splitlines()))
        text = text[block.end():]


def header_license(path: Path) -> tuple[str, tuple[str, ...]]:
    head = path.read_text(errors="replace")[:4000]
    spdx = re.search(r"SPDX-License-Identifier:\s*([^*\n]+?)\s*(?:\*/)?$", head, re.M)
    holders = re.findall(
        r"(?:SPDX-FileCopyrightText:|Copyright)\s*(?:\(c\)|\(C\)|©)?\s*([^\n*]+)", head)
    holders = tuple(sorted({re.sub(r"^[\d\s,-]+", "", h).strip(" .") for h in holders} - {""}))
    return (spdx.group(1) if spdx else "", holders)


def managed_component(directory: Path) -> tuple[str, str, str]:
    """Registry name, version and URL of a managed component directory."""
    name = directory.name.replace("__", "/", 1)
    manifest = directory / "idf_component.yml"
    text = manifest.read_text() if manifest.exists() else ""
    version = re.search(r"^version:\s*['\"]?([^'\"\n]+)", text, re.M)
    url = re.search(r"^(?:repository|url):\s*['\"]?([^'\"\n]+)", text, re.M)
    return (name, version.group(1).strip() if version else "",
            https_url(url.group(1).strip()) if url else
            f"https://components.espressif.com/components/{name}")


def firmware_entries(builds: list[Path], idf_version: str) -> list[Entry]:
    sources, archives = set(), set()
    for build in builds:
        for sub, map_name in ((build, None), (build / "bootloader", "bootloader.map")):
            if map_name is None:
                project = json.loads((build / "project_description.json").read_text())
                map_name = project["app_elf"].removesuffix(".elf") + ".map"
            found_sources, found_archives = linked_files(sub, sub / map_name)
            sources |= found_sources
            archives |= found_archives

    by_directory: dict[Path, set[Path]] = collections.defaultdict(set)
    by_header: dict[tuple, list[Path]] = collections.defaultdict(list)
    build_dirs = [b.resolve() for b in builds]
    for source in sorted(sources):
        in_build = any(source.is_relative_to(b) for b in build_dirs)
        own = (source.is_relative_to(REPO) and not in_build
               and not source.is_relative_to(REPO / "managed_components")
               and "third_party" not in source.parts)
        directory = None if own or in_build else governing_license(source)
        if directory:
            by_directory[directory].add(source)
            continue
        spdx, holders = header_license(source)
        if own and (spdx == "MIT" or not holders):
            continue  # JetKVM's own code, with or without a header
        if in_build and not holders:
            continue  # generated from the repository's own files
        by_header[(spdx, holders, own)].append(source)

    for archive in sorted(archives):
        if archive.name in ("libgcc.a", "libstdc++.a"):
            continue  # GCC Runtime Library Exception: no notice required
        if archive.name == "libc.a":
            by_directory[IDF / "components" / "esp_libc"].add(archive)
            continue
        for build in build_dirs + [b / "bootloader" for b in build_dirs]:
            # A library that an ESP-IDF component builds with its own CMake
            # project, such as Mbed TLS's tfpsacrypto.
            if archive.is_relative_to(build / "esp-idf"):
                archive = IDF / "components" / archive.relative_to(build / "esp-idf")
        directory = governing_license(archive)
        if directory:
            by_directory[directory].add(archive)

    exported = EXPORTED.findall((REPO / "CMakeLists.txt").read_text())
    entries = []
    idf_entry = Entry("ESP-IDF", idf_version,
                      f"https://github.com/espressif/esp-idf/tree/{idf_version}",
                      texts=[(IDF / "LICENSE").read_text()])
    entries.append(idf_entry)
    for directory in sorted(by_directory):
        if directory.is_relative_to(REPO / "managed_components"):
            component = directory.relative_to(REPO / "managed_components").parts[0]
            name, version, url = managed_component(REPO / "managed_components" / component)
            sub = directory.relative_to(REPO / "managed_components" / component)
            if sub.parts:
                name = f"{name} ({sub.as_posix()})"
            entry = Entry(name, version, url)
        elif directory.is_relative_to(IDF):
            relative = directory.relative_to(IDF).as_posix()
            entry = Entry(f"ESP-IDF: {relative}", idf_version,
                          f"https://github.com/espressif/esp-idf/tree/{idf_version}/{relative}")
        else:
            relative = directory.relative_to(REPO).as_posix()
            entry = Entry(relative, url=f"https://github.com/jetkvm/kvm-mini/tree/main/{relative}")
            for folder, repository, revision in exported:
                if relative == folder or relative.startswith(folder + "/"):
                    entry = Entry(relative, revision, f"https://github.com/{repository}/tree/"
                                  f"{revision}{relative.removeprefix(folder)}")
        entry.texts = license_texts(directory)
        entries.append(entry)
    if any("x509_crt_bundle" in (b / "jetkvm-mini.map").read_text(errors="replace")
           for b in builds):
        entries.append(Entry(
            "Mozilla CA certificate list (ESP-IDF certificate bundle)", idf_version,
            f"https://github.com/espressif/esp-idf/tree/{idf_version}/components/mbedtls/esp_crt_bundle",
            "MPL-2.0", ["Mozilla Public License 2.0: https://mozilla.org/MPL/2.0/\n"
                        "Source: https://curl.se/docs/caextract.html"]))
    for (spdx, holders, own), files in sorted(by_header.items()):
        espressif = all(ESPRESSIF.search(h) for h in holders)
        if not own and spdx in ("", "Apache-2.0") and espressif:
            continue  # Espressif's own code, covered by the ESP-IDF entry
        names = sorted({_display(f, idf_version) for f in files})
        texts = [header_comment(files[0])]
        if spdx == "Apache-2.0" and espressif:
            texts = [(IDF / "LICENSE").read_text()]
        entries.append(Entry(", ".join(names[:3]) + (f" and {len(names) - 3} more" if len(names) > 3 else ""),
                             license=spdx, texts=texts))
    return entries


def _display(path: Path, idf_version: str) -> str:
    if path.is_relative_to(IDF):
        return f"ESP-IDF {idf_version}: {path.relative_to(IDF).as_posix()}"
    if path.is_relative_to(REPO):
        relative = path.relative_to(REPO)
        if relative.parts[0].startswith("build"):
            return "generated: " + relative.relative_to(relative.parts[0]).as_posix()
        return relative.as_posix()
    return path.name


def https_url(url: str) -> str:
    """A browsable https URL for an npm repository field, or "" if none."""
    url = re.sub(r"^git\+|\.git$|#.*$", "", url.strip())
    hosts = {"github": "github.com", "gitlab": "gitlab.com", "bitbucket": "bitbucket.org"}
    if match := re.fullmatch(r"(?:(github|gitlab|bitbucket):)?([\w.-]+/[\w.-]+)", url):
        return f"https://{hosts[match.group(1) or 'github']}/{match.group(2)}"
    url = re.sub(r"^(?:ssh://)?git@([^:/]+)[:/]", r"https://\1/", url)
    url = re.sub(r"^(?:git|http)://", "https://", url)
    return url if url.startswith("https://") else ""


# --- web UI -----------------------------------------------------------------

def ui_entries(dist: Path, node_modules: Path) -> list[Entry]:
    packages = set()
    for source_map in dist.rglob("*.map"):
        for source in json.loads(source_map.read_text()).get("sources", []):
            match = re.search(r"node_modules/((?:@[^/]+/)?[^/]+)", source)
            if match:
                packages.add(match.group(1))
    if any("tailwindcss v" in css.read_text(errors="replace") for css in dist.rglob("*.css")):
        packages.add("tailwindcss")
    entries = []
    for package in sorted(packages):
        directory = node_modules / package
        manifest = json.loads((directory / "package.json").read_text())
        repository = manifest.get("repository")
        url = https_url(repository.get("url", "") if isinstance(repository, dict) else repository or "")
        texts = license_texts(directory)
        if not texts:
            raise SystemExit(f"{package}: no license file in {directory}")
        entries.append(Entry(package, manifest.get("version", ""), url or f"https://www.npmjs.com/package/{package}",
                             str(manifest.get("license", "")), texts))
    return entries


# --- hand-listed ------------------------------------------------------------

def listed_entries(parts) -> list[Entry]:
    entries = []
    for name, files, url in parts:
        texts = []
        for file in files:
            path = Path(file.replace("$IDF", str(IDF)))
            texts.append((path if path.is_absolute() else REPO / path).read_text())
        entries.append(Entry(name, url=url, texts=texts))
    return entries


# --- output -----------------------------------------------------------------

def license_name(text: str) -> str:
    # These two quote MIT's grant, so they are recognized first.
    for needle, spdx in (("Espressif Modified MIT", "LicenseRef-Espressif-Modified-MIT"),
                         ("SIL OPEN FONT LICENSE", "OFL-1.1")):
        if needle.lower() in text.lower():
            return spdx
    checks = [
        ("Apache License", "Apache-2.0"),
        ("Permission is hereby granted, free of charge", "MIT"),
        ("Permission to use, copy, modify, and/or distribute", "ISC"),
        ("Permission to use, copy, modify, and distribute", "ISC"),
        ("Redistribution and use in source and binary forms", "BSD"),
        ("GNU GENERAL PUBLIC LICENSE", "GPL"),
    ]
    found = {spdx for needle, spdx in checks if needle.lower() in text.lower()}
    return found.pop() if len(found) == 1 else "see text"


def normalized(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip()


def render(sections: list[tuple[str, str, list[Entry]]]) -> str:
    texts: dict[str, int] = {}
    bodies: list[str] = []

    def text_id(text: str) -> int:
        key = normalized(text)
        if key not in texts:
            texts[key] = len(bodies) + 1
            bodies.append(text.strip("\n"))
        return texts[key]

    out = [
        "# Third-party notices",
        "",
        "The JetKVM Mini firmware and web UI are MIT-licensed (see [LICENSE](LICENSE)).",
        "They include the third-party software below, each under its own license.",
        "The full license texts follow the lists.",
        "",
        "This file is generated by `tools/third_party_notices.sh` from a release build.",
        "Do not edit it by hand.",
        "",
    ]
    for title, intro, entries in sections:
        out += [f"## {title}", "", intro, "", "| Component | Version | License | Text |",
                "| --- | --- | --- | --- |"]
        for entry in sorted(entries, key=lambda e: e.name.lower()):
            ids = sorted({text_id(t) for t in entry.texts})
            name = f"[{entry.name}]({entry.url})" if entry.url else entry.name
            names = list(dict.fromkeys(license_name(t) for t in entry.texts))
            if len(names) > 1 and "see text" in names:
                names.remove("see text")  # a NOTICE file next to the license
            spdx = entry.license or " / ".join(names)
            links = ", ".join(f"[{i}](#license-text-{i})" for i in ids)
            out.append(f"| {name} | {entry.version} | {spdx} | {links} |")
        out.append("")
    out += ["## License texts", ""]
    for number, body in enumerate(bodies, 1):
        fence = "````" if "```" in body else "```"
        out += [f"### License text {number}", "", f"{fence}text", body, fence, ""]
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--build", action="append", type=Path, required=True)
    parser.add_argument("--ui-dist", type=Path, required=True)
    parser.add_argument("--node-modules", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=REPO / "THIRD_PARTY_NOTICES.md")
    args = parser.parse_args()
    args.build = [build.resolve() for build in args.build]

    project = json.loads((args.build[0] / "project_description.json").read_text())
    # "-dirty": the build applies the patches in patches/ to ESP-IDF.
    idf_version = project["git_revision"].removesuffix("-dirty")
    sections = [
        ("Firmware (ESP32-P4)",
         f"Linked into the application or bootloader of either board variant, built with ESP-IDF {idf_version}.",
         firmware_entries(args.build, idf_version)),
        ("Wi-Fi coprocessor firmware (ESP32-C5)",
         "The Wi-Fi board's ESP32-C5 runs a prebuilt ESP-Hosted-MCU image that the firmware carries "
         "(`components/jetkvm_c5_wifi/firmware/`).",
         listed_entries(C5_PARTS)),
        ("Web UI", "Bundled into the web UI the firmware serves.",
         ui_entries(args.ui_dist, args.node_modules)),
        ("Fonts", "Used by the web UI and the display.", listed_entries(FONTS)),
    ]
    args.output.write_text(render(sections))
    print(f"wrote {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

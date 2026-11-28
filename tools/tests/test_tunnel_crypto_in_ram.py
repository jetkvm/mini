"""The tunnel's per-packet crypto is linked into internal RAM.

components/jetkvm_tailscale/linker.lf maps object files of the vendored
WireGuard library by name. The linker script generator takes a name that
matches no object without an error, and the code then runs from flash again,
where its speed depends on what else the image has. So a renamed or removed
source file, or a mapping that the build no longer reads, must fail here.
"""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
TAILSCALE = ROOT / "components" / "jetkvm_tailscale"
WIREGUARD = ROOT / "third_party" / "microlink" / "components" / "wireguard_lwip"


def mapping():
    """The archive and the (object, scheme) entries of the fragment's mapping."""
    text = re.sub(r"#.*", "", (TAILSCALE / "linker.lf").read_text())
    archive = re.search(r"^archive:\s*(\S+)", text, re.M).group(1)
    entries = re.findall(r"^\s+(\S+)\s+\((\w+)\)\s*$", text, re.M)
    return archive, entries


class TunnelCryptoInRam(unittest.TestCase):
    def test_the_build_reads_the_fragment(self):
        cmake = (TAILSCALE / "CMakeLists.txt").read_text()
        self.assertRegex(cmake, r'LDFRAGMENTS\s+"linker\.lf"')

    def test_each_mapped_object_is_a_source_of_the_wireguard_library(self):
        archive, entries = mapping()
        # ESP-IDF names a component's archive after its directory.
        self.assertEqual(archive, f"lib{WIREGUARD.name}.a")
        cmake = (WIREGUARD / "CMakeLists.txt").read_text()
        sources = {Path(name).stem for name in re.findall(r'"([^"]+\.c)"', cmake)}
        for name, _ in entries:
            self.assertIn(name, sources, f"{name}.c is not built into {archive}")

    def test_the_cipher_and_the_authenticator_of_each_packet_are_mapped(self):
        _, entries = mapping()
        placed = {name for name, scheme in entries if scheme == "noflash_text"}
        # What chacha20poly1305_encrypt() and _decrypt() run for each packet.
        aead = (WIREGUARD / "src" / "crypto" / "refc" / "chacha20poly1305.c").read_text()
        for header, name in (("chacha20.h", "chacha20"), ("poly1305-donna.h", "poly1305-donna")):
            self.assertIn(f'#include "{header}"', aead)
            self.assertIn(name, placed)
        self.assertIn("chacha20poly1305", placed)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Static contract for the ESP-IDF emSign distrust backport.

Intermediates under these eMudhra roots had weak keys, so the bundle that
TLS clients attach must not contain them: they move from cacrt_all.pem to
cacrt_deprecated.pem, which the bundle leaves out.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PATCH_NAME = "esp-idf-6.0.2-crt-bundle-emsign-distrust.patch"
patch = (ROOT / "patches" / PATCH_NAME).read_text(encoding="utf-8")
cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")

assert "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEPRECATED_LIST=y" not in defaults
assert f"apply_idf_patch(patches/{PATCH_NAME})" in cmake
assert cmake.index(f"apply_idf_patch(patches/{PATCH_NAME})") < cmake.index(
    "include($ENV{IDF_PATH}/tools/cmake/project.cmake)")

ROOTS = ["emSign ECC Root CA - G3", "emSign Root CA - C1", "emSign ECC Root CA - C3"]
files = patch.split("diff --git ")[1:]
by_file = {f.split()[0].removeprefix("a/"): f for f in files}
all_pem = by_file["components/mbedtls/esp_crt_bundle/cacrt_all.pem"]
deprecated = by_file["components/mbedtls/esp_crt_bundle/cacrt_deprecated.pem"]
for name in ROOTS:
    assert f"\n-{name}\n" in all_pem, name
    assert f"\n+{name}\n" in deprecated, name
assert not any(l.startswith("+") and not l.startswith("+++")
               for l in all_pem.splitlines())
print("ESP-IDF crt bundle emSign distrust backport contract: PASS")

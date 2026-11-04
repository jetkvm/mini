#!/usr/bin/env python3
"""Static contract for the ESP-IDF certificate bundle leak backport.

With CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY, the CA callback
must reference the matched CA's subject instead of copying it into buffers
that mbedtls_x509_crt_free() never frees.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PATCH_NAME = "esp-idf-6.0.2-crt-bundle-cross-signed-leak.patch"
patch = (ROOT / "patches" / PATCH_NAME).read_text(encoding="utf-8")
cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")

# The leaking path only exists with cross-signed verification on.
assert "CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y" in defaults
assert f"apply_idf_patch(patches/{PATCH_NAME})" in cmake
assert cmake.index(f"apply_idf_patch(patches/{PATCH_NAME})") < cmake.index(
    "include($ENV{IDF_PATH}/tools/cmake/project.cmake)")
assert "components/mbedtls/esp_crt_bundle/esp_crt_bundle.c" in patch

removed = [l[1:].strip() for l in patch.splitlines() if l.startswith("-") and not l.startswith("---")]
added = [l[1:].strip() for l in patch.splitlines() if l.startswith("+") and not l.startswith("+++")]
# The three copies that leaked are gone ...
assert "new_cert->subject_raw.p = calloc(1, cert_name_len);" in removed
assert "dst->oid.p = calloc(1, src->oid.len);" in removed
assert "dst->val.p = calloc(1, src->val.len);" in removed
# ... and replaced by references into the bundle and the child certificate.
assert "new_cert->subject_raw.p = (unsigned char *)cert_name;" in added
assert "dst->oid.p = src->oid.p;" in added
assert "dst->val.p = src->val.p;" in added
assert not any("calloc(1, src->" in l for l in added)
print("ESP-IDF crt bundle cross-signed leak backport contract: PASS")

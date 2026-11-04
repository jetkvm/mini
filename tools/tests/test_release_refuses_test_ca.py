"""The release script must recognise an image that trusts the e2e test CA.

A mock cloud build (-DJETKVM_MOCK_CLOUD_JWKS_URL) adds that CA, whose key is in
the repository, to the certificate bundle. tools/release_r2.sh refuses an
image that contains the text it searches for. This test builds the bundle
from the CA file with ESP-IDF's own generator and checks that the bundle
contains that text, so the two cannot drift apart.
"""
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TEST_CA = ROOT / "frontend/jetkvm-ui/e2e/mini/mock-cloud-ca.pem"


class ReleaseRefusesTestCa(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("IDF_PATH"), "needs ESP-IDF (IDF_PATH) for its bundle generator")
    def test_the_text_the_release_script_searches_for_is_in_a_bundle_with_the_test_ca(self):
        script = (ROOT / "tools/release_r2.sh").read_text()
        searched = re.findall(r'grep -aqF "([^"]+)" "\$stage_dir/jetkvm-mini\.bin"; then\n\s+fail "\$variant image trusts', script)
        self.assertEqual(len(searched), 1, "release_r2.sh has one check for the test CA")

        generator = Path(os.environ["IDF_PATH"]) / "components/mbedtls/esp_crt_bundle/gen_crt_bundle.py"
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run([sys.executable, str(generator), "--quiet", "--input", str(TEST_CA)],
                           cwd=directory, check=True, capture_output=True)
            bundle = (Path(directory) / "x509_crt_bundle").read_bytes()
        self.assertIn(searched[0].encode(), bundle)


if __name__ == "__main__":
    unittest.main()

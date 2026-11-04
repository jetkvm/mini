#!/usr/bin/env python3
"""Run the production HTTP access and reset implementations with platform doubles.

Only platform includes are replaced. Requests pass through registered guards,
JSON parsing, password staging, NVS persistence, and the actual reset phases.
Cryptography has independent OpenSSL-vector tests in run_password_test.sh.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
depth = re.search(r'^CONFIG_CJSON_NESTING_LIMIT=(\d+)$',
                  (root / 'sdkconfig.defaults').read_text(), re.M).group(1)
access = root / 'components/jetkvm_access'
reset = root / 'components/jetkvm_factory_reset'
source = '\n'.join('\n'.join(line for line in p.read_text().splitlines()
                            if not line.startswith('#include'))
                   for p in (reset / 'jetkvm_factory_reset.c', access / 'jetkvm_access.c'))
harness = (access / 'tests/test_setup.c').read_text().replace('/* PRODUCTION_IMPLEMENTATIONS */', source)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.c').write_text(harness)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-DCJSON_NESTING_LIMIT=' + depth,
                    '-fsanitize=address,undefined',
                    '-I' + str(access / 'include'),
                    '-I' + str(root / 'components/device_api/include'),
                    '-I' + str(root / 'managed_components/espressif__cjson/cJSON'),
                    str(path / 'test.c'), str(access / 'password_core.c'),
                    str(root / 'components/device_api/device_contract.c'),
                    str(root / 'managed_components/espressif__cjson/cJSON/cJSON.c'),
                    '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)

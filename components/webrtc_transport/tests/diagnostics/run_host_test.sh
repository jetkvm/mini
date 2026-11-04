#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../../.." && pwd)"
test_dir="$(mktemp -d)"
trap 'rm -rf "$test_dir"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$root/components/webrtc_transport" \
    "$root/components/webrtc_transport/diagnostics_zip.c" \
    "$root/components/webrtc_transport/tests/diagnostics/test_zip.c" -o "$test_dir/test"
"$test_dir/test" "$test_dir/diagnostics.zip"
python3 - "$test_dir/diagnostics.zip" <<'PY'
import json, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as bundle:
    assert bundle.testzip() is None
    assert json.loads(bundle.read('diagnostics.json')) == {'schemaVersion': 1}
    assert bundle.read('empty.txt') == b''
    assert bundle.read('binary.dat') == bytes([0, 1, 255, 42, 0])
print('ZIP extracted and CRCs verified with Python zipfile: PASS')
PY
python3 "$root/components/webrtc_transport/tests/diagnostics/test_collection.py"

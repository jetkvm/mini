#!/usr/bin/env bash
# Run every host check: component suites, web-app packing, tool tests and IDF
# patch guards. Needs `idf.py reconfigure` first.
set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

failed=0
run() {
    local label="$1"; shift
    echo "== $label"
    if "$@"; then
        echo "-- ok: $label"
    else
        echo "-- FAIL: $label"
        failed=1
    fi
}

for script in components/*/tests/run*host_test*.sh \
              components/*/tests/run_static_test.sh \
              components/*/tests/run_password_test.sh \
              components/*/tests/run_sctp_hid_test.sh \
              components/*/tests/*/run_host_test.sh; do
    [ -e "$script" ] || continue
    run "$script" bash "$script"
done

run "tools/web_app/run_host_tests.sh" bash tools/web_app/run_host_tests.sh

run "python unittest discover (tools)" \
    python3 -m unittest discover -s tools/tests -t . -p "test_*.py" -q

run "tools/combined_load unit tests" node --test tools/combined_load/runner/combined_load.test.mjs
run "patches/tests/run_static_test.sh" bash patches/tests/run_static_test.sh

if [ "$failed" -ne 0 ]; then
    echo "== OVERALL: FAILURES"
    exit 1
fi
echo "== OVERALL: ALL TESTS PASS"

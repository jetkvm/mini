#!/usr/bin/env bash
# Builds this checkout for a bench, installs it on the bench's Mini and runs
# the integration tests one at a time.
#
#   tools/integration/run.sh <bench> [test name pattern]
#
# <bench>: a file in tools/benches/ (name without .env) or a path to one.
# Exit status: 0 all passed, 1 a test failed, 2 nothing ran.
set -euo pipefail
if [ $# -lt 1 ]; then
    sed -n 2,8p "$0" >&2
    exit 2
fi
bench="$1"
pattern="${2:-}"
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$root/.e2e-out/integration/$(basename "$bench" .env)"
image="$(node "$root/tools/combined_load/runner/build.mjs" "$bench" "$out")" || exit 2
export INTEGRATION_BENCH="$bench" INTEGRATION_IMAGE="$image"
node "$root/tools/integration/lib/install.mjs" || exit 2
# A busy tool machine skews timing-sensitive tests.
if [ -r /proc/loadavg ]; then
    while [ "$(cut -d. -f1 /proc/loadavg)" -ge "$(( $(nproc) / 2 ))" ]; do
        echo "tool machine busy (load $(cut -d' ' -f1 /proc/loadavg)), waiting" >&2
        sleep 10
    done
fi
# One Mini per bench: no concurrency. TAP is parsed below.
set +e
node --test --test-reporter=tap --test-concurrency=1 ${pattern:+--test-name-pattern="$pattern"} "$root"/tools/integration/tests/*.test.mjs |
    tee "$out/tests.tap"
status=${PIPESTATUS[0]}
# node --test passes when the pattern matches nothing, reporting each file as one test.
if ! grep '^# Subtest: ' "$out/tests.tap" | grep -qv '\.test\.mjs$'; then
    echo "no test ran: no test name matches \"$pattern\"" >&2
    exit 2
fi
exit "$status"

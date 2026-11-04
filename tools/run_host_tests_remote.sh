#!/usr/bin/env bash
# Runs tools/run_all_tests.sh on a remote BuildKit host (the Dockerfile's
# host-tests target) and prints the summary and the log's path.
#
# Usage: tools/run_host_tests_remote.sh [<bench>]
#
# The host is the bench file's JETKVM_BUILD_HOST; without <bench>, the one
# that all files in tools/benches/ agree on.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ "$#" -gt 1 ] || [[ "${1:-}" == -* ]]; then
    echo "usage: tools/run_host_tests_remote.sh [<bench>]" >&2
    exit 2
fi

builder="$(node --input-type=module -e '
const [root, bench] = process.argv.slice(1);
const { benchBuilder, benchFile, readBench } = await import(`${root}/tools/combined_load/runner/build.mjs`);
const fs = await import("node:fs");
let values;
if (bench) {
  values = readBench(benchFile(root, bench));
} else {
  const benches = `${root}/tools/benches`;
  const hosts = new Set(fs.readdirSync(benches).filter(name => name.endsWith(".env") && name !== "example.env")
    .map(name => readBench(`${benches}/${name}`).JETKVM_BUILD_HOST));
  if (hosts.size !== 1 || ![...hosts][0]) {
    console.error("the bench files do not all name the same build host; name a bench");
    process.exit(1);
  }
  values = { JETKVM_BUILD_HOST: [...hosts][0] };
}
if (!values.JETKVM_BUILD_HOST) {
  console.error("the bench file has no JETKVM_BUILD_HOST");
  process.exit(1);
}
console.log(benchBuilder(values));
' "$root" "${1:-}")"

out="$(mktemp -d "${TMPDIR:-/tmp}/jetkvm-host-tests.XXXXXX")"
echo "host tests on the builder $builder" >&2
# TEST_RUN is new for each run, so the builder runs the tests again.
if ! docker buildx build --builder "$builder" --target host-tests \
    --build-arg "TEST_RUN=$(date +%s)-$$" \
    --output "type=local,dest=$out" "$root"; then
    echo "the build stopped before the tests ended" >&2
    exit 1
fi

status="$(cat "$out/host-tests.status")"
echo
grep -E '^-- FAIL: ' "$out/host-tests.log" || true
echo "$(grep -c '^-- ok: ' "$out/host-tests.log") suites passed, $(grep -c '^-- FAIL: ' "$out/host-tests.log") failed"
grep '^== OVERALL' "$out/host-tests.log" || true
echo "log: $out/host-tests.log"
exit "$status"

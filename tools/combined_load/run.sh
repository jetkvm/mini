#!/usr/bin/env bash
# Builds this checkout for a bench, installs the image on the bench's Mini,
# and runs the lists.
#
#   tools/combined_load/run.sh <bench> [runner/run.mjs options]
#
# <bench> is a file in tools/benches/ (name without .env) or a path to
# one. runner/build.mjs builds the image; run.mjs installs it if the Mini runs
# another build. --plain-cloud builds and runs without TLS to the mock cloud.
# The image and the recordings go to .e2e-out/combined-load/<bench>/.
set -euo pipefail
if [ $# -lt 1 ]; then
    sed -n 2,10p "$0" >&2
    exit 2
fi
bench="$1"; shift
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$root/.e2e-out/combined-load/$(basename "$bench" .env)"
plain=
case " $* " in
    *" --plain-cloud "*) plain=--plain-cloud ;;
esac
# Without an image no list takes place, which is run.mjs's exit status 2.
image="$(node "$root/tools/combined_load/runner/build.mjs" "$bench" "$out" $plain)" || exit 2
# The machine that runs the tool is part of the measurement: wait while its
# 1-minute load is half its cores or more.
if [ -r /proc/loadavg ]; then
    while [ "$(cut -d. -f1 /proc/loadavg)" -ge "$(( $(nproc) / 2 ))" ]; do
        echo "tool machine busy (load $(cut -d' ' -f1 /proc/loadavg)), waiting" >&2
        sleep 10
    done
fi
exec node "$root/tools/combined_load/runner/run.mjs" "$bench" --image="$image" --install \
    --out="$out/$(date -u +%Y%m%dT%H%M%SZ)" "$@"

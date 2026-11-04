#!/usr/bin/env bash
# Run the whole Playwright e2e suite against one Mini and leave its settings
# as they were.
#
#   tools/e2e/run.sh <bench> [--no-update-tests] [--phase 1|2|all]
#                    [-g <pattern>] [--grep-invert <pattern>] [--headed]
#
# <bench> is a file in tools/benches/ (name without .env) or a path to
# one; see example.env.
#
# Phase 1 (OS Services off): ui, video-codec, remote-agent, then the firmware
# update tests (skipped with --no-update-tests), which build the current
# source as the build under test and as version 0.0.1.
# Phase 2 (OS Services on): the OS Services specs.
# --phase or JETKVM_PHASES picks one; both by default.
#
# The run restores the update settings, host-display idle hiding and the OS
# Services bench on exit, including Ctrl-C. Automatic updates stay off.
# Output goes to .e2e-out/run-<time>.
set -uo pipefail
if [ $# -lt 1 ]; then
    sed -n 2,18p "$0" >&2
    exit 2
fi
bench="$1"; shift
update_tests=1
phases=""
while [ $# -gt 0 ]; do
    case "$1" in
        --no-update-tests) update_tests=0; shift ;;
        --phase)
            case "${2:-}" in
                1|2) phases="$2" ;;
                all) phases="1 2" ;;
                *) echo "--phase takes 1, 2 or all" >&2; exit 2 ;;
            esac
            shift 2
            ;;
        *) break ;;
    esac
done
# Only test selection is forwarded: projects, workers, config and reporters
# must stay as the script sets them.
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
    case "${args[i]}" in
        -g|--grep|--grep-invert)
            ((i + 1 < ${#args[@]})) || { echo "${args[i]} needs a pattern" >&2; exit 2; }
            i=$((i + 1))
            ;;
        --grep=*|--grep-invert=*|--headed)
            ;;
        *)
            echo "run.sh forwards only -g, --grep, --grep-invert and --headed; ${args[i]} is refused" >&2
            exit 2
            ;;
    esac
done
root="$(cd "$(dirname "$0")/../.." && pwd)"
[ -f "$bench" ] || bench="$root/tools/benches/$bench.env"
if [ ! -f "$bench" ]; then
    echo "no bench file: $bench" >&2
    exit 2
fi
set -a
# shellcheck disable=SC1090
. "$bench"
set +a
if [ -z "$phases" ]; then
    case "${JETKVM_PHASES:-all}" in
        1|2) phases="$JETKVM_PHASES" ;;
        all) phases="1 2" ;;
        *) echo "JETKVM_PHASES takes 1, 2 or all" >&2; exit 2 ;;
    esac
fi
: "${JETKVM_URL:?the bench file must set JETKVM_URL}"
# With a device shell, upstream's global teardown resets the whole
# configuration after every Playwright run.
if [ "${JETKVM_DEVICE_SSH:-0}" != 0 ]; then
    echo "run.sh runs without a device shell: with JETKVM_DEVICE_SSH=1 the global" \
        "teardown resets the device configuration after every run" >&2
    exit 2
fi
export JETKVM_DEVICE_SSH=0
export NODE_NO_WARNINGS=1

# The update tests use only the images this script builds; OTA variables
# from the caller's environment are dropped.
unset RELEASE_STAGE_DIR TEST_UPDATE_VERSION BASELINE_STAGE_DIR BASELINE_VERSION
if [ "$update_tests" = 1 ]; then
    : "${JETKVM_SKU:?the bench file must set JETKVM_SKU for the firmware update tests}"
fi

out="$root/.e2e-out/run-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
# Chrome keeps a socket in TMPDIR, and a socket path has at most 107 bytes:
# under the output directory of a deep checkout it does not fit.
tmp="$(mktemp -d /tmp/jetkvm-e2e.XXXXXX)" || exit 1
export TMPDIR="$tmp"
trap 'rm -rf "$tmp"' EXIT
# Every invocation appends its skipped tests here; the run prints them once.
export E2E_SKIP_LOG="$out/skipped.txt"
# The OS Services bench state to restore (os-services-bench.setup.ts).
export E2E_OSVC_BENCH_FILE="$out/os-services-bench.json"
: > "$E2E_SKIP_LOG"

rc=0
phase=""
# playwright <output subdir> <args>: a failure sets rc and the run goes on.
playwright() {
    local sub="$1" status=0; shift
    (cd "$root/frontend/jetkvm-ui" && npx playwright test --no-deps --pass-with-no-tests \
        --config="$root/frontend/jetkvm-ui/playwright.config.ts" \
        --reporter="list,$root/tools/e2e/skip-summary.mjs" \
        --output="$out/${phase:+p$phase-}$sub" "$@") || status=$?
    [ "$status" = 0 ] || rc=$status
    return "$status"
}
# osvc_bench <step> <title>: one OS Services bench step, without the caller's
# test selection.
osvc_bench() { playwright "bench-$1" --project=os-services-bench -g "bench: $2"; }
rpc() { node "$root/tools/e2e/rpc_call.mjs" "$JETKVM_URL/" "$@" | tail -1; }

# An interrupted reset can leave the welcome flow active; complete it, as
# ensureNoPasswordViaAPI does in specs.
node --input-type=module - "$JETKVM_URL" <<'JS' || exit 1
const origin = new URL(process.argv[2]).origin;
const request = (path, options = {}) => fetch(origin + path, {
    ...options, signal: AbortSignal.timeout(10000),
});
const response = await request('/device/status');
if (!response.ok) throw new Error(`Setup status failed: ${response.status}`);
const status = await response.json();
if (status.factoryResetPending) throw new Error('Factory reset is still pending');
if (!status.isSetup) {
    const setup = await request('/device/setup', {
        method: 'POST', headers: { 'Content-Type': 'application/json', Origin: origin },
        body: JSON.stringify({ localAuthMode: 'noPassword' }),
    });
    if (!setup.ok) throw new Error(`Setup failed: ${setup.status}`);
}
const device = await request('/device');
if (!device.ok) throw new Error(`The bench requires no-password mode: ${device.status}`);
JS
# Automatic updates (off by default) would replace the build under test with
# the newest release.
if [ "$(rpc setAutoUpdateState '{"enabled":false}' | jq -c '.result')" != false ]; then
    echo "could not turn automatic updates off on $JETKVM_URL; not starting" >&2
    exit 1
fi
version="$(rpc getLocalVersion | grep -o '"systemVersion":"[^"]*"' | cut -d'"' -f4)"
if [ -z "$version" ]; then
    echo "could not read the firmware version of $JETKVM_URL; not starting" >&2
    exit 1
fi
echo "bench $(basename "$bench" .env)  target $JETKVM_URL  firmware $version  output $out"

# stage_image [--version=<version>]: builds the current source into
# $out/images/<version>/skus/<sku>/ (the release layout) and prints
# $out/images/<version>. <version> drops +codelog build metadata.
stage_image() {
    local image version dir
    image="$(node "$root/tools/combined_load/runner/build.mjs" "$bench" "$out/images/build" --plain-cloud "$@")" || return 1
    version="$(python3 "$root/tools/mini_image.py" "$image" | cut -d' ' -f1)"
    version="${version%%+*}"
    [ -n "$version" ] || return 1
    dir="$out/images/$version/skus/$JETKVM_SKU"
    mkdir -p "$dir" && mv "$out/images/build"/* "$dir/" &&
        (cd "$dir" && sha256sum jetkvm-mini.bin | cut -d' ' -f1 > jetkvm-mini.bin.sha256) || return 1
    echo "$out/images/$version"
}
if [ "$update_tests" = 1 ]; then
    RELEASE_STAGE_DIR="$(stage_image)" && BASELINE_STAGE_DIR="$(stage_image --version=0.0.1)" || {
        echo "could not build the firmware images; not starting" >&2
        exit 1
    }
    export RELEASE_STAGE_DIR TEST_UPDATE_VERSION="${RELEASE_STAGE_DIR##*/}"
    export BASELINE_STAGE_DIR BASELINE_VERSION="${BASELINE_STAGE_DIR##*/}"
    echo "firmware update tests: build under test $TEST_UPDATE_VERSION, old version $BASELINE_VERSION"
fi

# Idle hiding drops HPD between specs; it is off for the run and restored after.
idle="$(rpc getHostDisplayIdleMode | grep -o '"enabled":\(true\|false\)' | cut -d: -f2)"
if [ -z "$idle" ]; then
    echo "could not read host-display idle hiding on $JETKVM_URL; not starting" >&2
    exit 1
fi
# A second signal during the restore stops it.
ota_started=0
restore_ota() {
    [ "$ota_started" = 1 ] || return 0
    # An interrupted update can still restart the Mini; wait for it.
    local end=$((SECONDS + 420))
    until [ "$(rpc isUpdatePending 2>/dev/null | jq -c '.result' 2>/dev/null)" = false ]; do
        [ "$SECONDS" -lt "$end" ] || { echo "an update is still pending on $JETKVM_URL" >&2; break; }
        sleep 5
    done
    # Cleared only on success, so the exit handler retries.
    restore_update_settings || return 1
    ota_started=0
}
osvc_recorded=0
restore_osvc() {
    [ "$osvc_recorded" = 1 ] || return 0
    osvc_bench restore "restore OS Services" || return 1
    osvc_recorded=0
}
restore_device() {
    local status=$? restored=1
    trap - INT TERM
    restore_ota || restored=0
    restore_osvc || restored=0
    rpc setHostDisplayIdleMode "{\"enabled\":$idle}" >/dev/null || {
        echo "could not restore host-display idle hiding ($idle)" >&2
        restored=0
    }
    [ -z "${tmp:-}" ] || command rm -rf "$tmp"
    # A run that passed but did not put the device back fails.
    if [ "$restored" = 0 ] && [ "$status" = 0 ]; then
        echo "rc=1: the device was not restored" >&2
        exit 1
    fi
}
trap restore_device EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if ! rpc setHostDisplayIdleMode '{"enabled":false}' >/dev/null; then
    echo "could not disable host-display idle hiding on $JETKVM_URL; not starting" >&2
    exit 1
fi

# The OTA specs change the update URL, channel and auto-update settings.
update_settings=()
save_update_settings() {
    local entry getter setter key value
    update_settings=()
    for entry in getUpdateApiUrl:setUpdateApiUrl:url \
        getDevChannelState:setDevChannelState:enabled \
        getAutoUpdateState:setAutoUpdateState:enabled; do
        IFS=: read -r getter setter key <<< "$entry"
        # jq -e would count a false setting as a failure.
        value="$(rpc "$getter" | jq -c 'if has("result") then .result else error("no result") end')"
        [ -n "$value" ] || {
            echo "could not read $getter on $JETKVM_URL" >&2
            return 1
        }
        update_settings+=("$setter" "$(jq -cn --arg k "$key" --argjson v "$value" '{($k): $v}')")
    done
}
restore_update_settings() {
    local i status=0
    for ((i = 0; i < ${#update_settings[@]}; i += 2)); do
        rpc "${update_settings[i]}" "${update_settings[i + 1]}" >/dev/null || {
            echo "could not restore ${update_settings[i]} ${update_settings[i + 1]}" >&2
            rc=1
            status=1
        }
    done
    return "$status"
}

# Installs the build under test (tools/integration/lib/install.mjs).
install_build() {
    echo "the build under test $TEST_UPDATE_VERSION"
    # Settings that are still to be written back are the Mini's own.
    restore_ota || return 1
    save_update_settings || return 1
    ota_started=1
    INTEGRATION_BENCH="$bench" INTEGRATION_IMAGE="$RELEASE_STAGE_DIR/skus/$JETKVM_SKU/jetkvm-mini.bin" \
        node "$root/tools/integration/lib/install.mjs" || return 1
    restore_ota
}
if [ "$update_tests" = 1 ] && ! install_build; then
    echo "could not install $TEST_UPDATE_VERSION; not starting" >&2
    exit 1
fi

# A busy test machine skews timings: wait while load >= half its cores.
if [ -r /proc/loadavg ]; then
    while [ "$(cut -d. -f1 /proc/loadavg)" -ge "$(( $(nproc) / 2 ))" ]; do
        echo "tool machine busy (load $(cut -d' ' -f1 /proc/loadavg)), waiting" >&2
        sleep 10
    done
fi
osvc_bench record "record OS Services" || { echo "could not record the OS Services bench; not starting" >&2; exit 1; }
# Without a record (a bench without OS Services) there is nothing to put back.
[ ! -f "$E2E_OSVC_BENCH_FILE" ] || osvc_recorded=1

# ota_begin and restore_ota frame the update tests. While an earlier restore
# keeps failing, no update tests run.
ota_begin() {
    if ! restore_ota; then
        echo "the OTA projects did not run: the update settings were not restored after the last ones" >&2
        return 1
    fi
    if [ -z "${TEST_UPDATE_VERSION:-}" ]; then
        # Not even with skips: their cleanup writes update settings.
        echo "--no-update-tests: the firmware update tests did not run"
        return 1
    fi
    if ! save_update_settings; then
        echo "the OTA projects did not run: the update settings could not be saved" >&2
        rc=1
        return 1
    fi
    ota_started=1
}

phase_1() {
    osvc_bench off "OS Services off" || { echo "phase 1 did not run: OS Services stayed on" >&2; return; }
    playwright ui --project=ui --project=video-codec --project=remote-agent "$@"
    ota_begin || return
    # ota-check runs on the build under test, before anything installs.
    playwright ota-check --project=ota-check "$@"
    playwright ota-install \
        --project=ota-reboot-overlay \
        "$@"
    restore_ota
}

phase_2() {
    if [ "$update_tests" = 1 ] && ! install_build; then
        echo "phase 2 did not run: could not install $TEST_UPDATE_VERSION" >&2
        rc=1
        return
    fi
    osvc_bench on "OS Services on" || { echo "phase 2 did not run: OS Services did not come on" >&2; return; }
    playwright os-services --project=os-services "$@"
}

declare -A phase_rc
failed=0
for phase in $phases; do
    echo
    echo "phase $phase"
    rc=0
    "phase_$phase" "$@"
    phase_rc[$phase]=$rc
    [ "$rc" = 0 ] || failed=1
done
phase=""
rc=0
restore_osvc || failed=1

skip_summary() {
    local count
    count="$(wc -l < "$E2E_SKIP_LOG")"
    [ "$count" -gt 0 ] || return 0
    echo
    echo "Skipped in this run ($count):"
    sed 's/^/  /' "$E2E_SKIP_LOG"
}
skip_summary
echo
for p in 1 2; do
    name="OS Services $([ "$p" = 1 ] && echo off || echo on)"
    if [ -z "${phase_rc[$p]+set}" ]; then
        echo "phase $p ($name): not run"
    elif [ "${phase_rc[$p]}" = 0 ]; then
        echo "phase $p ($name): passed"
    else
        echo "phase $p ($name): failed (rc=${phase_rc[$p]})"
    fi
done
rc=$failed
echo "rc=$rc"
exit $rc

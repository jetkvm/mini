#!/usr/bin/env bash
# Write a build to the bench's Mini over its USB JTAG/serial debugger, also
# when the Mini does not boot. NVS settings stay.
#
#   tools/e2e/serial_flash.sh <bench> <directory>
#
# <directory> is the output of tools/combined_load/runner/build.mjs, built for
# the bench's JETKVM_SKU. The debugger is on JETKVM_COMBINED_LOAD_HOST, else
# JETKVM_REMOTE_HOST (Linux, macOS, or Windows with Git sh over SSH). With
# several Espressif ports, a MAC in a comment of the bench file picks one.
# esptool 5.4.0 is installed in the remote home when missing. Fails unless
# the Mini then reports the image's version.
set -euo pipefail
if [ $# -ne 2 ]; then
    sed -n 2,12p "$0" >&2
    exit 2
fi
bench="$1" directory="$2"
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
: "${JETKVM_URL:?the bench file must set JETKVM_URL}"
host="${JETKVM_COMBINED_LOAD_HOST:-${JETKVM_REMOTE_HOST:-}}"
if [ -z "$host" ]; then
    echo "no serial flash on $(basename "$bench" .env): the bench file has no JETKVM_COMBINED_LOAD_HOST" \
        "or JETKVM_REMOTE_HOST, the PC with the Mini's debugger" >&2
    exit 2
fi
args="$directory/flasher_args.json"
if [ ! -f "$args" ]; then
    echo "no $args: build the directory with tools/combined_load/runner/build.mjs" >&2
    exit 2
fi

# The files as "<offset> <file name>" lines. The build keeps them in
# subdirectories; build.mjs puts them side by side.
mapfile -t flash < <(jq -r '.flash_files | to_entries[] | "\(.key) \(.value | split("/") | last)"' "$args")
files=()
for entry in "${flash[@]}"; do
    file="$directory/${entry#* }"
    [ -f "$file" ] || { echo "no $file, which flasher_args.json names" >&2; exit 2; }
    files+=("$file")
done
read -r version sku < <(python3 "$root/tools/mini_image.py" "$directory/jetkvm-mini.bin")
if [ -z "$version" ]; then
    echo "$directory/jetkvm-mini.bin is not an app image" >&2
    exit 2
fi
if [ -n "${JETKVM_SKU:-}" ] && [ "$sku" != "$JETKVM_SKU" ]; then
    echo "the image is built for ${sku:-no board}, the bench is $JETKVM_SKU" >&2
    exit 2
fi
# esptool's options, then write-flash with the flash settings and the files.
mapfile -t esptool < <(jq -r '.extra_esptool_args as $e |
    "--chip", $e.chip, "--before", $e.before, "--after", $e.after,
    (if $e.stub then empty else "--no-stub" end),
    "write-flash", .write_flash_args[]' "$args")
for entry in "${flash[@]}"; do
    esptool+=("${entry% *}" "${entry#* }")
done

# One MAC in the bench file's comments, in the form of the by-id names.
mac="$(grep '^#' "$bench" | grep -oiE '([0-9a-f]{2}:){5}[0-9a-f]{2}' | tr a-f A-F | sort -u || true)"
if [ "$(printf '%s' "$mac" | grep -c .)" -gt 1 ]; then
    echo "the comments of $bench name more than one MAC:" $mac >&2
    exit 2
fi

# A directory in the host's home, which scp reaches by the same relative path
# on each OS.
remote="$(ssh "$host" mktemp -d serial-flash.XXXXXX)"
trap 'ssh "$host" rm -rf "$remote"' EXIT
scp -q "${files[@]}" "$host:$remote/"
echo "writing $version to the Mini on $host" >&2
# The script below runs on the host, also in bash 3.2, the bash of macOS:
# <directory> <MAC or ''> <esptool args>.
ssh "$host" "bash -s -- $(printf '%q ' "$remote" "$mac" "${esptool[@]}")" <<'HOST'
set -euo pipefail
cd "$1"
mac="$2"
shift 2
# ports prints "<USB serial number> <port>" for each Espressif (USB vendor
# 0x303a) serial port.
case "$(uname -s)" in
    Linux)
        python=python3 bin=bin
        ports() {
            for f in /dev/serial/by-id/usb-Espressif*-if00; do
                [ -e "$f" ] || continue
                s="${f%-if00}"
                echo "${s##*_} $f"
            done
        } ;;
    Darwin)
        # The Python of macOS is older than 3.10, which esptool 5 needs.
        release="macos-$(uname -m | sed 's/x86_64/amd64/')"
        ports() {
            ioreg -a -r -c IOUSBHostDevice -l | python3 -c '
import plistlib, sys
def callouts(node):
    if "IOCalloutDevice" in node: yield node["IOCalloutDevice"]
    for child in node.get("IORegistryEntryChildren", []): yield from callouts(child)
for device in plistlib.loads(sys.stdin.buffer.read()):
    if device.get("idVendor") == 0x303a:
        for port in callouts(device): print(device.get("USB Serial Number"), port)'
        } ;;
    MINGW*|MSYS*)
        # The COM port is an interface of the USB device whose instance ID
        # ends in the serial number.
        python=python bin=Scripts
        ports() {
            cat > ports.ps1 <<'PS'
Get-CimInstance Win32_PnPEntity -Filter "PNPDeviceID LIKE 'USB\\VID_303A%' AND Name LIKE '%(COM%'" | ForEach-Object {
    $parent = (Get-PnpDeviceProperty -InstanceId $_.PNPDeviceID -KeyName DEVPKEY_Device_Parent).Data
    "$($parent.Split('\')[-1]) $([regex]::Match($_.Name, 'COM\d+').Value)"
}
PS
            powershell -NoProfile -ExecutionPolicy Bypass -File ports.ps1 | tr -d '\r'
        } ;;
    *) echo "$(hostname): no serial flash on $(uname -s)" >&2; exit 1 ;;
esac
all="$(ports)"
found="$all"
[ -z "$mac" ] || found="$(printf '%s\n' "$all" | grep "^$mac " || true)"
if [ "$(printf '%s' "$found" | grep -c .)" -ne 1 ]; then
    echo "$(hostname): not one Espressif serial port${mac:+ with $mac}:" $all >&2
    exit 1
fi
port="${found#* }"
if [ -n "${release:-}" ]; then
    # Espressif's build of esptool, which needs no Python.
    esptool="$HOME/esptool-5.4.0/esptool"
    if [ ! -x "$esptool" ]; then
        echo "$(hostname): installing esptool 5.4.0 in ~/esptool-5.4.0" >&2
        mkdir -p "$HOME/esptool-5.4.0"
        curl -fsSL "https://github.com/espressif/esptool/releases/download/v5.4.0/esptool-v5.4.0-$release.tar.gz" |
            tar -xzf - -C "$HOME/esptool-5.4.0" --strip-components 1
    fi
else
    venv="$HOME/esptool-venv"
    esptool="$venv/$bin/esptool"
    if [ ! -x "$esptool" ]; then
        echo "$(hostname): installing esptool 5.4.0 in $venv" >&2
        "$python" -m venv "$venv"
        "$venv/$bin/python" -m pip install -q esptool==5.4.0
    fi
fi
echo "$(hostname): $port" >&2
if [ -n "${release:-}" ]; then
    # In download mode the P4's ROM attaches a USB-OTG device that never
    # answers enumeration, and some macOS USB controllers then power off all
    # their ports. Disconnect it first (DCTL.SftDiscon at 0x50000804), write
    # without a reset, and finish with a watchdog reset (an RTS reset would
    # re-enter download mode).
    args=()
    while [ $# -gt 0 ]; do
        case "$1" in
            --chip) chip="$2"; args+=("$1" "$2"); shift 2 ;;
            --before) before="$2"; args+=(--before no-reset); shift 2 ;;
            --after) args+=(--after watchdog-reset); shift 2 ;;
            *) args+=("$1"); shift ;;
        esac
    done
    set -- "${args[@]}"
    "$esptool" --port "$port" --chip "$chip" --before "$before" --after no-reset --no-stub \
        write-mem 0x50000804 0x2 0x2
fi
"$esptool" --port "$port" "$@"
HOST

echo "waiting for $JETKVM_URL/device/status" >&2
deadline=$((SECONDS + 120))
until curl -fs -m 5 -o /dev/null "$JETKVM_URL/device/status"; do
    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "$JETKVM_URL/device/status does not answer 120 s after the write" >&2
        exit 1
    fi
    sleep 2
done
# /webrtc/stats needs a session on a Mini with a password; the bench tools
# work in no-password mode, as tools/e2e/run.sh requires.
stats="$(curl -sS -m 10 -w '\n%{http_code}' "$JETKVM_URL/webrtc/stats")"
case "${stats##*$'\n'}" in
    200) ;;
    401) echo "the write is done, but the Mini has a password, so /webrtc/stats does not give its version;" \
            "the bench tools need no-password mode" >&2; exit 1 ;;
    *) echo "$JETKVM_URL/webrtc/stats answers ${stats##*$'\n'}" >&2; exit 1 ;;
esac
running="$(printf '%s' "${stats%$'\n'*}" | jq -r '.firmware.version')"
echo "$running"
if [ "$running" != "$version" ]; then
    echo "the Mini runs $running, the image is $version" >&2
    exit 1
fi

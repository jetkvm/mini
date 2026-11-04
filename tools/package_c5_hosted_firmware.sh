#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${1:-${JETKVM_C5_HOSTED_BUILD_DIR:-}}
output_dir="$repo_root/components/jetkvm_c5_wifi/firmware"

if [[ -z "$build_dir" ]]; then
    echo "usage: $0 ESP_HOSTED_MCU_BUILD_DIR" >&2
    exit 2
fi

files=(
    "bootloader/bootloader.bin:c5_bootloader_zlib.bin"
    "partition_table/partition-table.bin:c5_partition_table_zlib.bin"
    "ota_data_initial.bin:c5_ota_data_initial_zlib.bin"
    "network_adapter.bin:c5_network_adapter_zlib.bin"
)

for entry in "${files[@]}"; do
    source_name=${entry%%:*}
    if [[ ! -f "$build_dir/$source_name" ]]; then
        echo "missing build artifact: $build_dir/$source_name" >&2
        exit 1
    fi
done

mkdir -p "$output_dir"
python3 - "$build_dir" "$output_dir" <<'PY'
import hashlib
import pathlib
import sys
import zlib

source = pathlib.Path(sys.argv[1])
output = pathlib.Path(sys.argv[2])
files = {
    "bootloader/bootloader.bin": "c5_bootloader_zlib.bin",
    "partition_table/partition-table.bin": "c5_partition_table_zlib.bin",
    "ota_data_initial.bin": "c5_ota_data_initial_zlib.bin",
    "network_adapter.bin": "c5_network_adapter_zlib.bin",
}

for source_name, output_name in files.items():
    raw = (source / source_name).read_bytes()
    compressed = zlib.compress(raw, level=9)
    (output / output_name).write_bytes(compressed)
    print(
        f"{source_name}: {len(raw)} -> {len(compressed)} bytes, "
        f"md5={hashlib.md5(raw).hexdigest()}"
    )
PY

echo "ESP32-C5 ESP-Hosted-MCU firmware packaged in $output_dir"

#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
python3 "${SCRIPT_DIR}/test_remote_wakeup_static.py"
python3 "${SCRIPT_DIR}/test_release_all_static.py"
python3 "${SCRIPT_DIR}/test_configuration_snapshot_static.py"
python3 "${SCRIPT_DIR}/test_dynamic_descriptors_static.py"
python3 "${SCRIPT_DIR}/test_jetkvm_compat_static.py"
python3 "${SCRIPT_DIR}/test_msc_writable_image_static.py"

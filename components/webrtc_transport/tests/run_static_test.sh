#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
python3 "${SCRIPT_DIR}/test_stats_json_static.py"
python3 "${SCRIPT_DIR}/test_sessions_static.py"
python3 "${SCRIPT_DIR}/test_data_channels_static.py"

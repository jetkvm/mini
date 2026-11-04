#!/usr/bin/env bash
set -euo pipefail
python3 "$(dirname "$0")/test_reset.py"
python3 "$(dirname "$0")/test_boot_recovery.py"

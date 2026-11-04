#!/usr/bin/env bash
set -euo pipefail

python3 "$(dirname "$0")/test_usb_service_static.py"

python3 "$(dirname "$0")/test_emulation_completion.py"

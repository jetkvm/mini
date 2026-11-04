#!/usr/bin/env bash
set -euo pipefail
python3 "$(dirname "$0")/test_audio_state.py"
python3 "$(dirname "$0")/test_audio_capture_clock.py"
python3 "$(dirname "$0")/test_startup.py"
python3 "$(dirname "$0")/test_optional_usb.py"

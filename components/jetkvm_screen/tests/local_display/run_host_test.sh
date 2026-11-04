#!/usr/bin/env bash
set -euo pipefail
python3 "$(dirname "$0")/test_settings.py"
python3 "$(dirname "$0")/test_rotation_redraw.py"
python3 "$(dirname "$0")/test_reset_progress.py"
python3 "$(dirname "$0")/test_wake_press.py"
python3 "$(dirname "$0")/test_wifi_setup_screen.py"
python3 "$(dirname "$0")/test_usb_debugging_screen.py"

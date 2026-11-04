#!/usr/bin/env bash
set -euo pipefail

component_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 "$component_dir/tests/test_credentials_static.py"
python3 "$component_dir/tests/test_credential_cleanup.py"
python3 "$component_dir/tests/test_band_transition.py"
python3 "$component_dir/tests/test_band_rollback.py"
python3 "$component_dir/tests/test_reset_completion.py"
python3 "$component_dir/tests/test_network_migration.py"
python3 "$component_dir/tests/test_setup_code_log_static.py"
python3 "$component_dir/tests/test_conn_task_stack_static.py"
python3 "$component_dir/tests/test_factory_mac_static.py"

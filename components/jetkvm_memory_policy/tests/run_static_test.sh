#!/usr/bin/env bash
set -euo pipefail

component_dir="$(cd "$(dirname "$0")/.." && pwd)"
python3 "$component_dir/tests/test_coexistence_budget.py"
python3 "$component_dir/tests/test_video_rebuild_transaction.py"
python3 "$component_dir/tests/test_peer_admission.py"
python3 "$component_dir/tests/test_tls_admission.py"
python3 "$component_dir/tests/test_fragmented_psram_admission.py"
python3 "$component_dir/tests/test_snapshot_cache.py"
python3 "$component_dir/tests/test_psram_only_admission.py"
python3 "$component_dir/tests/test_psram_wait_admission.py"

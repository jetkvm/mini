#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
python3 "${script_dir}/test_dma_ring.py"
python3 "${script_dir}/test_dma_quiesce.py"
python3 "${script_dir}/test_isr_cache_safe.py"

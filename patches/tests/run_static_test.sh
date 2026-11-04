#!/usr/bin/env bash
set -euo pipefail
for script in "$(dirname "$0")"/test_*.py; do
    python3 "$script"
done

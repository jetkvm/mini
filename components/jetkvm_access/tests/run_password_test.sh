#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
python3 "$root/components/jetkvm_access/tests/test_setup.py"
python3 "$root/components/jetkvm_access/tests/test_tls_boot.py"
python3 "$root/components/jetkvm_access/tests/test_http_recovery.py"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -I"$root/components/jetkvm_access/include" \
 "$root/components/jetkvm_access/password_core.c" \
 "$root/components/jetkvm_access/tests/test_password_core.c" -lcrypto -o "$binary"
"$binary"

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -I"$root/components/jetkvm_access/include" \
 "$root/components/jetkvm_access/tests/test_access_contract.c" -o "$binary"
"$binary"

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 -I"$root/components/jetkvm_access/include" \
 -I"$root/components/jetkvm_access/tests/compat" \
 "$root/components/jetkvm_access/password_pbkdf2.c" \
 "$root/components/jetkvm_access/tests/test_password_pbkdf2.c" -lcrypto -o "$binary"
"$binary"

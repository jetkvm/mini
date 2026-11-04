#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
component="$repo_root/components/jetkvm_ota"
os_services="$repo_root/components/jetkvm_os_services"
binary="$(mktemp)"
trap 'rm -f "$binary"' EXIT

cc -std=c11 -Wall -Wextra -Werror \
  -I"$component/tests/compat" \
  -I"$component/include" \
  -I"$repo_root/components/control_protocol/include" \
  -I"$repo_root/managed_components/espressif__cjson/cJSON" \
  "$component/jetkvm_ota_core.c" \
  "$component/jetkvm_ota_semver.c" \
  "$repo_root/managed_components/espressif__cjson/cJSON/cJSON.c" \
  "$component/tests/test_ota_core.c" \
  -o "$binary"
"$binary"

# The update's signature check against GnuPG's keys and signatures
# (fixtures/), with the OpenPGP code of the OS Services component and
# OpenSSL's hashes. Monocypher is third-party code and has its own flags.
monocypher="$os_services/third_party/monocypher"
objects="$(mktemp -d)"
trap 'rm -rf "$binary" "$objects"' EXIT
cc -std=c11 -O1 -c "$monocypher/monocypher.c" -o "$objects/monocypher.o"
cc -std=c11 -O1 -c "$monocypher/monocypher-ed25519.c" -I"$monocypher" -o "$objects/monocypher-ed25519.o"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -g \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$component/include" -I"$component/tests" \
  -I"$os_services/include" -I"$os_services/tests" -I"$os_services" -I"$monocypher" \
  -DFIXTURES_DIR="\"$component/tests/fixtures\"" \
  "$component/jetkvm_ota_signature.c" "$component/jetkvm_ota_semver.c" \
  "$os_services/os_services_pgp.c" "$os_services/tests/host_hash.c" \
  "$component/tests/test_ota_signature.c" \
  "$objects/monocypher.o" "$objects/monocypher-ed25519.o" \
  -lcrypto -o "$binary"
"$binary"

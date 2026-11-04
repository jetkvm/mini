#!/usr/bin/env bash
set -euo pipefail

component_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 "${component_dir}/tests/test_exfat_direct_io.py"
python3 "${component_dir}/tests/test_upload_owner_contract.py"
python3 "${component_dir}/tests/test_upload_identity_persistence.py"
python3 "${component_dir}/tests/test_upload_identity_resume_model.py"
python3 "${component_dir}/tests/test_upload_copy_pipeline.py"
python3 "${component_dir}/tests/test_http_range_memory_policy.py"
python3 "${component_dir}/tests/test_http_range_tls_reserve.py"
python3 "${component_dir}/tests/test_writable_image_contract.py"
python3 "${component_dir}/tests/test_tf_input_delay_policy.py"
python3 "${component_dir}/tests/test_builtin_installer_iso.py"

binary="$(mktemp)"
dma_binary="$(mktemp)"
identity_binary="$(mktemp)"
download_binary="$(mktemp)"
format_binary="$(mktemp)"
detect_binary="$(mktemp)"
builtin_binary="$(mktemp)"
trap 'rm -f "$binary" "$dma_binary" "$identity_binary" "$download_binary" "$format_binary" "$detect_binary" "$builtin_binary"' EXIT
cc -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE \
  -I"${component_dir}" \
  "${component_dir}/http_range_contract.c" \
  "${component_dir}/tests/test_http_range_contract.c" \
  -o "$binary"
"$binary"

cc -std=c11 -Wall -Wextra -Werror \
  -I"${component_dir}/tests/compat" \
  -I"${component_dir}" \
  "${component_dir}/tests/test_upload_dma_contract.c" \
  -o "$dma_binary"
"$dma_binary"

cc -std=c11 -Wall -Wextra -Werror \
  -I"${component_dir}/include" \
  -I"${component_dir}" \
  "${component_dir}/tests/test_upload_identity_contract.c" \
  -o "$identity_binary"
"$identity_binary"

cc -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE \
  -I"${component_dir}" \
  "${component_dir}/http_range_contract.c" \
  "${component_dir}/storage_download_contract.c" \
  "${component_dir}/tests/test_storage_download_contract.c" \
  -o "$download_binary"
"$download_binary"

cc -std=c11 -Wall -Wextra -Werror \
  -I"${component_dir}/include" \
  -I"${component_dir}" \
  "${component_dir}/storage_format_contract.c" \
  "${component_dir}/tests/test_storage_format_contract.c" \
  -o "$format_binary"
"$format_binary"

cc -std=c11 -Wall -Wextra -Werror \
  -I"${component_dir}" \
  "${component_dir}/tf_detect_contract.c" \
  "${component_dir}/tests/test_tf_detect_contract.c" \
  -o "$detect_binary"
"$detect_binary"

cc -std=c11 -Wall -Wextra -Werror \
  -I"${component_dir}" \
  "${component_dir}/builtin_image.c" \
  "${component_dir}/tests/test_builtin_image.c" \
  -o "$builtin_binary"
"$builtin_binary"

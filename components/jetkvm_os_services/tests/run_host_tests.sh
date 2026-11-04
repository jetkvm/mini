#!/usr/bin/env bash
# Host tests of OS Services: the session's C suite against a sender written
# from the OS Services protocol (signatures checked with OpenSSL), the
# update relay's checks against GnuPG-signed fixtures, the browser's terminal
# and file channels, the bulk transfer policy against a model of USB packets,
# the class driver's transfer completions, the browser's settings, the guest
# display EDID, then the Python sender model in test_os_services_protocol.py.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
component="$repo_root/components/jetkvm_os_services"
build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT

cjson_dir="$repo_root/managed_components/espressif__cjson/cJSON"
if [[ ! -f "$cjson_dir/cJSON.c" ]]; then
  idf_root="${IDF_PATH:-$HOME/esp/esp-idf}"
  cjson_dir="$idf_root/components/json/cJSON"
fi
if [[ ! -f "$cjson_dir/cJSON.c" ]]; then
  echo "cJSON source not found; run the ESP-IDF export script first" >&2
  exit 1
fi

monocypher="$component/third_party/monocypher"
ota_include="$repo_root/components/jetkvm_ota/include"
core_sources=(
  "$component/os_services_wire.c"
  "$component/os_services_session.c"
  "$component/os_services_video_rate.c"
)
# The update relay's pure parts: signatures, package checks, the index.
update_sources=(
  "$component/os_services_pgp.c"
  "$component/os_services_update.c"
  "$component/os_services_release.c"
  "$repo_root/components/jetkvm_ota/jetkvm_ota_semver.c"
)
fixtures=(-DFIXTURES_DIR="\"$component/tests/fixtures\"" -I"$component")

# The service task's stack is 8 KiB: no frame may hold a large buffer. The
# firmware build checks the same limit; this catches it without ESP-IDF.
for source in "${core_sources[@]}" "${update_sources[@]}" \
  "$component/os_services_link.c" \
  "$component/os_services_settings.c" "$component/os_services_channels.c" \
  "$component/os_services_edid.c"; do
  cc -std=c11 -O2 -Wframe-larger-than=1024 -Werror \
    -I"$component/include" -I"$monocypher" -I"$cjson_dir" -I"$ota_include" \
    -c "$source" -o "$build_dir/frame-check.o"
done
# Monocypher is third-party code; build it without the repository's
# warning set.
cc -std=c11 -O2 -c "$monocypher/monocypher.c" -o "$build_dir/monocypher.o"
cc -std=c11 -O2 -I"$monocypher" -c "$monocypher/monocypher-ed25519.c" \
  -o "$build_dir/monocypher-ed25519.o"
cc -std=c11 -O1 -c "$cjson_dir/cJSON.c" -o "$build_dir/cJSON.o"

cc -std=c11 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$component/include" -I"$component/tests" -I"$monocypher" -I"$cjson_dir" \
  -I"$ota_include" "${fixtures[@]}" \
  "${core_sources[@]}" "${update_sources[@]}" \
  "$component/tests/fake_platform.c" "$component/tests/fake_video.c" \
  "$component/tests/host_hash.c" \
  "$component/tests/test_os_services_session.c" \
  "$build_dir/monocypher.o" "$build_dir/monocypher-ed25519.o" \
  "$build_dir/cJSON.o" -lcrypto -lm -o "$build_dir/test_session"
"$build_dir/test_session"

# Signatures, the release index and the browser's guestUpdate object
# against GnuPG-signed fixtures, with the signing keys that the firmware
# update's key fetch reads from a keyserver's answer.
cc -std=c11 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$component/include" -I"$component/tests" -I"$monocypher" -I"$cjson_dir" \
  -I"$ota_include" "${fixtures[@]}" "${update_sources[@]}" \
  "$repo_root/components/jetkvm_ota/jetkvm_ota_signature.c" \
  "$component/tests/host_hash.c" "$component/tests/test_os_services_update.c" \
  "$build_dir/monocypher.o" "$build_dir/monocypher-ed25519.o" \
  "$build_dir/cJSON.o" -lcrypto -lm -o "$build_dir/test_update"
"$build_dir/test_update"

# The service video rate policy on its own.
cc -std=c11 -Wall -Wextra -Werror -pedantic -g -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$component/include" "$component/os_services_video_rate.c" \
  "$component/tests/test_os_services_video_rate.c" -o "$build_dir/test_video_rate"
"$build_dir/test_video_rate"

# The browser's terminal and file channels between a browser model and the
# real session.
cc -std=c11 -Wall -Wextra -Werror -pedantic -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$component/include" -I"$component/tests" -I"$monocypher" -I"$cjson_dir" \
  "${core_sources[@]}" "$component/os_services_channels.c" \
  "$component/tests/fake_platform.c" \
  "$component/tests/test_os_services_channels.c" \
  "$build_dir/monocypher.o" "$build_dir/monocypher-ed25519.o" \
  "$build_dir/cJSON.o" -lm -o "$build_dir/test_channels"
"$build_dir/test_channels"

# The bulk transfer policy against a model of USB packets.
cc -std=c11 -Wall -Wextra -Werror -pedantic -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$component/include" \
  "$component/os_services_link.c" "$component/os_services_wire.c" \
  "$component/tests/test_os_services_link.c" -o "$build_dir/test_link"
"$build_dir/test_link"

# The browser's settings and their NVS record. zlib's CRC-32 reseals
# tampered records independently of the code under test.
cc -std=c11 -Wall -Wextra -Werror -pedantic -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$component/include" -I"$monocypher" -I"$cjson_dir" \
  "$component/os_services_settings.c" "$component/os_services_wire.c" \
  "$component/tests/test_os_services_settings.c" "$build_dir/cJSON.o" \
  -lz -lm -o "$build_dir/test_settings"
"$build_dir/test_settings"

# The guest display EDID against golden EDID bytes for the same serial, and
# the EDID/CTA-861 fields read back independently.
cc -std=c11 -Wall -Wextra -Werror -pedantic -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$component" -I"$component/tests" \
  "$component/os_services_edid.c" "$component/tests/test_os_services_edid.c" \
  -o "$build_dir/test_edid"
"$build_dir/test_edid"

# The service task's settings and video state, with every ESP-IDF header
# that jetkvm_os_services.c includes served by fake_idf.h.
# -Wno-unused-parameter as in ESP-IDF's own build.
for header in esp_err esp_heap_caps esp_log esp_random esp_timer nvs \
  freertos/FreeRTOS freertos/idf_additions freertos/queue freertos/semphr \
  freertos/task jetkvm_memory_policy esp_attr sdkconfig; do
  mkdir -p "$(dirname "$build_dir/compat/$header")"
  echo '#include "fake_idf.h"' >"$build_dir/compat/$header.h"
done
cc -std=c11 -Wall -Wextra -Wno-unused-parameter -Werror -pedantic -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$build_dir/compat" -I"$component/tests" -I"$component/include" \
  -I"$cjson_dir" -I"$repo_root/components/webrtc_transport" \
  -I"$repo_root/components/webrtc_transport/include" \
  -I"$repo_root/components/jetkvm_video/include" \
  -I"$repo_root/components/jetkvm_video_quality/include" \
  -I"$monocypher" -I"$ota_include" \
  "${core_sources[@]}" "${update_sources[@]}" "$component/os_services_settings.c" \
  "$component/os_services_channels.c" "$component/os_services_edid.c" \
  "$component/tests/host_hash.c" \
  "$repo_root/components/webrtc_transport/rpc_events.c" \
  "$component/tests/test_os_services_task.c" \
  "$build_dir/monocypher.o" "$build_dir/monocypher-ed25519.o" \
  "$build_dir/cJSON.o" -lcrypto -lm -o "$build_dir/test_task"
"$build_dir/test_task"

# The update relay's worker and the OS Services task as threads, with every
# ESP-IDF header os_services_relay.c includes served by fake_rtos.h.
for header in esp_attr esp_crt_bundle esp_err esp_heap_caps esp_http_client \
  esp_log esp_timer freertos/FreeRTOS freertos/idf_additions freertos/semphr \
  freertos/task jetkvm_memory_policy psa/crypto; do
  mkdir -p "$(dirname "$build_dir/relay_compat/$header")"
  echo '#include "fake_rtos.h"' >"$build_dir/relay_compat/$header.h"
done
cc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter -Werror -pedantic -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$build_dir/relay_compat" -I"$component/tests" -I"$component" \
  -I"$component/include" -I"$cjson_dir" -I"$monocypher" -I"$ota_include" \
  -I"$repo_root/components/control_protocol/include" \
  "${update_sources[@]}" "$component/tests/test_os_services_relay.c" \
  "$build_dir/monocypher.o" "$build_dir/monocypher-ed25519.o" \
  "$build_dir/cJSON.o" -lcrypto -lm -lpthread -o "$build_dir/test_relay"
"$build_dir/test_relay"

# The class driver's transfer completions, with TinyUSB and every ESP-IDF
# header that os_services_usb.c includes served by fake_usb.h.
for header in device/usbd_pvt esp_cache esp_err esp_heap_caps esp_log \
  esp_private/esp_cache_private esp_random esp_timer freertos/FreeRTOS \
  freertos/semphr freertos/task jetkvm_usb_hid sdkconfig tusb; do
  mkdir -p "$(dirname "$build_dir/usb_compat/$header")"
  echo '#include "fake_usb.h"' >"$build_dir/usb_compat/$header.h"
done
cc -std=c11 -Wall -Wextra -Wno-unused-parameter -Werror -pedantic -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I"$build_dir/usb_compat" -I"$component/tests" -I"$component/include" \
  -I"$cjson_dir" "$component/os_services_link.c" "$component/os_services_wire.c" \
  "$component/tests/test_os_services_usb.c" -o "$build_dir/test_usb"
"$build_dir/test_usb"

# The session as a shared library for the Python sender model.
cc -std=c11 -Wall -Wextra -Werror -shared -fPIC -O1 \
  -I"$component/include" -I"$component/tests" -I"$monocypher" -I"$cjson_dir" \
  "${core_sources[@]}" "$component/tests/session_shim.c" \
  "$component/tests/fake_video.c" \
  "$monocypher/monocypher.c" "$monocypher/monocypher-ed25519.c" \
  "$cjson_dir/cJSON.c" -lm -o "$build_dir/libosvc_session.so" \
  -Wno-error
OSVC_SESSION_LIB="$build_dir/libosvc_session.so" \
  python3 "$component/tests/test_os_services_protocol.py"

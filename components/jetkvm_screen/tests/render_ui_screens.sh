#!/usr/bin/env bash
set -euo pipefail

component_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
renderer_dir="$component_dir/tests/host_renderer"
output_dir="${1:-${TMPDIR:-/tmp}/jetkvm-ui-renders}"
variant="${2:-wifi}"
build_dir="${TMPDIR:-/tmp}/jetkvm-ui-renderer-build-${variant}"

cmake -S "$renderer_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DJETKVM_BOARD_VARIANT="$variant"
cmake --build "$build_dir" --target jetkvm_ui_renderer
"$build_dir/jetkvm_ui_renderer" "$output_dir"

if command -v magick >/dev/null 2>&1; then
    png_dir="$output_dir/png"
    mkdir -p "$png_dir"
    for image_path in "$output_dir"/*.ppm; do
        magick "$image_path" "$png_dir/$(basename "${image_path%.ppm}").png"
    done
    magick montage "$png_dir"/*.png \
        -tile 4x -geometry 240x240+8+22 \
        -background '#20242b' -fill white -pointsize 14 \
        -set label '%t' "$output_dir/montage.png"
    printf 'UI montage: %s\n' "$output_dir/montage.png"
fi

printf 'UI renders: %s\n' "$output_dir"

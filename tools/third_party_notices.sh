#!/usr/bin/env bash
# Regenerate THIRD_PARTY_NOTICES.md. Builds both board variants in
# build-notices-<variant>/ and the device web UI with source maps, then runs
# tools/third_party_notices.py on them (its docstring says what it reads).
# Run it before a release when a dependency, ESP-IDF or the configuration
# changed. Needs Docker and frontend/jetkvm-ui/node_modules (HUSKY=0 npm ci).
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
idf_image=espressif/idf:v6.0.2
ui_dir="$repo_root/frontend/jetkvm-ui"

if [[ ! -d "$ui_dir/node_modules" ]]; then
    echo "$ui_dir/node_modules is missing; run 'HUSKY=0 npm ci' there first" >&2
    exit 1
fi

ui_out=$(mktemp -d)
trap 'rm -rf "$ui_out"' EXIT
echo ">> building the device web UI with source maps"
(cd "$ui_dir" && npm run --silent i18n:compile &&
    npx vite build --mode=device --sourcemap --emptyOutDir --outDir "$ui_out") > /dev/null

# components/web_app packs dist-p4/ and needs it to configure; what it holds
# does not change which libraries are linked.
placeholder=false
if [[ ! -e "$ui_dir/dist-p4/index.html" ]]; then
    mkdir -p "$ui_dir/dist-p4"
    echo '<!doctype html><title>notices</title>' > "$ui_dir/dist-p4/index.html"
    placeholder=true
fi

# As in release_r2.sh: the container sees the repository at its host path so
# a git worktree can resolve its .git file back to the main checkout.
git_common=$(cd "$repo_root" && git rev-parse --path-format=absolute --git-common-dir)
main_checkout=$(cd "$git_common/.." && pwd)
mounts=(-v "$repo_root:$repo_root" -v "$ui_out:$ui_out")
if [[ "$main_checkout" != "$repo_root" ]]; then
    mounts+=(-v "$main_checkout:$main_checkout")
fi

echo ">> building both board variants in build-notices-<variant>/"
# A fresh tree's first configure stops once and asks for another run
# (CMakeLists.txt). ESP-IDF patches apply at configure, inside this container.
docker run --rm "${mounts[@]}" -w "$repo_root" -e HOME=/tmp \
    -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
    "$idf_image" bash -c '
        set -e
        for variant in ethernet wifi; do
            build=build-notices-$variant
            log=$build/notices.log
            rm -rf "$build"
            mkdir -p "$build"
            configure() {
                idf.py -B "$build" -DSDKCONFIG="$build/sdkconfig" \
                    -DJETKVM_BOARD_VARIANT="$variant" reconfigure > "$log" 2>&1
            }
            configure || configure || { tail -n 20 "$log"; exit 1; }
            idf.py -B "$build" build >> "$log" 2>&1 || { tail -n 20 "$log"; exit 1; }
        done
        python3 tools/third_party_notices.py --build build-notices-ethernet \
            --build build-notices-wifi --ui-dist "$1" \
            --node-modules frontend/jetkvm-ui/node_modules
        chown -R "$2" build-notices-* managed_components THIRD_PARTY_NOTICES.md
    ' notices "$ui_out" "$(id -u):$(id -g)"

if [[ "$placeholder" == true ]]; then
    rm -rf "$ui_dir/dist-p4"
fi
echo ">> wrote THIRD_PARTY_NOTICES.md; review it with git diff"

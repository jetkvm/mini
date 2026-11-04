#!/usr/bin/env bash
# Build both board variants at a release version and upload the artifacts to
# the R2 layout api.jetkvm.com serves from:
#
#   mini/<version>/skus/<sku>/jetkvm-mini.bin           over-the-air image
#   mini/<version>/skus/<sku>/jetkvm-mini.bin.sha256
#   mini/<version>/skus/<sku>/jetkvm-mini.bin.sig       stable versions only
#   mini/<version>/skus/<sku>/jetkvm-mini-full.bin      recovery image
#   mini/<version>/skus/<sku>/jetkvm-mini-full.bin.sha256
#   mini/<version>/skus/<sku>/jetkvm-mini-full.bin.sig  stable versions only
#
# and the cloud UI to the bucket app.jetkvm.com serves from:
#
#   v/<version>-mini/                                   UI bundle
#
# A Mini installs a stable version only with a signature by the JetKVM
# release key; prereleases (-dev<timestamp>) are unsigned and served to the
# dev channel. Stable versions build from origin/main (or --release-branch)
# and are registered by the release-sync workflow that the pushed
# release/<version> tag runs. Each image is built from the committed defaults
# files alone in a fresh build directory, and refused if it carries a test
# build option (release_config.py).
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
idf_image=espressif/idf:v6.0.2
r2_path="${R2_PATH:-r2://jetkvm-update/mini}"
cloud_app_path="${CLOUD_APP_R2_PATH:-r2://jetkvm-cloud-app/v}"
version=""
dev=false
dry_run=false
skip_build=false
skip_ui_build=false
release_branch=""
ui_dir="$repo_root/frontend/jetkvm-ui"

# Board variant (the CMake JETKVM_BOARD_VARIANT value) to the SKU its image
# carries; jetkvm_board_config.h is the source of these strings.
declare -A skus=([ethernet]=jetkvm-mini-ethernet [wifi]=jetkvm-mini-wireless)
artifacts=(jetkvm-mini.bin jetkvm-mini-full.bin)

usage() {
    cat <<EOF
usage: $0 --version <semver> [--signing-key <fingerprint>] [--dev] [--dry-run]
       [--skip-build] [--skip-ui-build] [--release-branch <name>]

  --version <semver>  Release version, 0.5.1 or 0.5.1-dev202609191200. Becomes
                      PROJECT_VER, the version devices report and compare.
  --dev               Append -dev<UTC yyyymmddHHMM> to --version, as jetkvm/kvm
                      does; the result is a prerelease for the dev channel.
  --signing-key <fingerprint>
                      The release signing subkey, a subkey of the JetKVM
                      release key, in the local GnuPG keyring. A stable
                      version needs it; a prerelease is not signed.
  --dry-run           Check, build, verify, sign and stage; print the upload;
                      do not upload. R2 is still read when rclone is installed.
  --skip-build        Reuse build-release-<variant>/ from a previous run;
                      refused when a source file is newer than its image,
                      its images are not the ones that run recorded, or its
                      configuration is not the release configuration.
                      Implies --skip-ui-build.
  --skip-ui-build     Reuse frontend/jetkvm-ui/dist-p4; refused when a
                      frontend source file is newer than the bundle or the
                      bundle is not the one a previous run recorded.
  --release-branch <name>
                      Emergency only: build a stable version from a commit
                      on origin/<name> instead of origin/main, for example
                      release-branch/0.5.19. HEAD must be on origin/<name> after a
                      fetch. Without it, a stable version must be on
                      origin/main. Not for a prerelease, which builds from
                      any commit.

Environment:
  SIGNING_KEY_FPR     The value of --signing-key
  R2_PATH             Destination prefix (default: ${r2_path})
  CLOUD_APP_R2_PATH   Cloud UI destination prefix (default: ${cloud_app_path})
EOF
}

fail() {
    echo "$*" >&2
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) version="${2:-}"; shift 2 ;;
        --signing-key) SIGNING_KEY_FPR="${2:-}"; shift 2 ;;
        --dev) dev=true; shift ;;
        --dry-run) dry_run=true; shift ;;
        --skip-build) skip_build=true; skip_ui_build=true; shift ;;
        --skip-ui-build) skip_ui_build=true; shift ;;
        --release-branch)
            if [[ -z "${2:-}" ]]; then
                echo "--release-branch needs the name of a branch other than main: ''" >&2
                exit 2
            fi
            release_branch="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$version" ]]; then
    usage >&2
    exit 2
fi
if [[ "$dev" == true ]]; then
    if [[ "$version" == *-* ]]; then
        echo "--dev appends the prerelease tag; give --version without one: $version" >&2
        exit 2
    fi
    version="$version-dev$(date -u +%Y%m%d%H%M)"
fi
# The only prerelease form is jetkvm/kvm's: -dev followed by a UTC timestamp.
# Timestamps of equal length sort chronologically under semver's identifier
# comparison, so a newer dev build is always the higher version.
number='(0|[1-9][0-9]*)'
if ! [[ "$version" =~ ^$number\.$number\.$number(-dev[0-9]{12})?$ ]]; then
    echo "version must be X.Y.Z or X.Y.Z-dev<yyyymmddHHMM>: $version" >&2
    exit 2
fi
prerelease=false
[[ "$version" == *-* ]] && prerelease=true
# What the log and the cloud UI's metadata say about the source beside the
# commit: the branch of --release-branch, or nothing for origin/main.
on_branch=""
branch_header=()
if [[ -n "$release_branch" ]]; then
    if [[ "$prerelease" == true ]]; then
        echo "--release-branch is for a stable version; a prerelease builds from any commit: $version" >&2
        exit 2
    fi
    if [[ "$release_branch" == main ]] || ! git check-ref-format --branch "$release_branch" >/dev/null 2>&1; then
        echo "--release-branch needs the name of a branch other than main: '$release_branch'" >&2
        exit 2
    fi
    on_branch=" on branch $release_branch"
    branch_header=(--header-upload "x-amz-meta-jetkvm-build-branch: $release_branch")
fi
cloud_version="$version-mini"

have_rclone=true
command -v rclone >/dev/null 2>&1 || have_rclone=false
if [[ "$dry_run" != true && "$have_rclone" != true ]]; then
    fail "rclone is not installed"
fi

# --- signing ----------------------------------------------------------------

# The JetKVM release key, by the fingerprint that the firmware of this
# checkout trusts (the same as rootKeyFingerprint in jetkvm/kvm's
# internal/ota/gpg.go). The signing key is one of its subkeys.
release_key_fpr=$(sed -n 's/^#define JETKVM_OTA_RELEASE_KEY_FINGERPRINT "\([0-9A-F]\{40\}\)"$/\1/p' \
    "$repo_root/components/jetkvm_ota/include/jetkvm_ota_signature.h")
[[ -n "$release_key_fpr" ]] || fail "jetkvm_ota_signature.h names no release key fingerprint"
# The test release key of a bench build, whose private half is in the
# repository; an image that names it is a bench build.
test_key_fpr=$(sed -n 's/^#define JETKVM_OTA_TEST_RELEASE_KEY_FINGERPRINT "\([0-9A-F]\{40\}\)"$/\1/p' \
    "$repo_root/components/jetkvm_ota/jetkvm_ota_test_key.h")
[[ -n "$test_key_fpr" ]] || fail "jetkvm_ota_test_key.h names no test release key fingerprint"

if [[ "$prerelease" != true ]]; then
    command -v gpg >/dev/null 2>&1 || fail "gpg is not installed"
    [[ -n "${SIGNING_KEY_FPR:-}" ]] || fail "stable $version is signed: set --signing-key or SIGNING_KEY_FPR"
    SIGNING_KEY_FPR="${SIGNING_KEY_FPR%!}"
    gpg --list-secret-keys "$SIGNING_KEY_FPR" >/dev/null 2>&1 \
        || fail "signing key $SIGNING_KEY_FPR is not in the local GPG keyring"
fi

# Fails unless the signature verifies against the release key and is one a
# Mini reads (os_services_pgp.h): OpenPGP v4, Ed25519, SHA-256/384/512, binary
# document. GnuPG accepts more than that.
verify_release_signature() {
    local reason
    reason=$(gpg --status-fd 1 --verify "$1" "$2" 2>/dev/null | awk -v key="$release_key_fpr" '
        $2 == "VALIDSIG" && $12 == key {
            found = 1
            if ($7 != 4) print "is an OpenPGP version " $7 " signature; a Mini reads version 4"
            else if ($9 != 22) print "is made with a key of algorithm " $9 "; a Mini reads Ed25519 (22)"
            else if ($10 < 8 || $10 > 10) print "has hash algorithm " $10 "; a Mini reads SHA-256, SHA-384 and SHA-512 (8, 9, 10)"
            else if ($11 != "00") print "has signature type 0x" $11 "; a Mini reads the signature of a binary document (0x00)"
        }
        END { if (!found) print "is not signed with the JetKVM release key " key }') || true
    [[ -z "$reason" ]] || fail "$(basename "$1") $reason"
}

# Writes a binary <file>.sig (--no-armor overrides gpg.conf). The "!" forces
# that exact subkey; without it GnuPG picks the newest signing subkey. Asks
# before each attempt, since a hardware key may need a touch or a PIN.
sign_file() {
    local attempt answer
    for attempt in 1 2 3; do
        read -r -p "ready to sign $(basename "$(dirname "$1")")/$(basename "$1") with key $SIGNING_KEY_FPR? [y/N] " answer
        [[ "$answer" == "y" ]] || fail "signing cancelled"
        if gpg --yes --no-armor --detach-sign --local-user "$SIGNING_KEY_FPR!" --output "$1.sig" "$1"; then
            verify_release_signature "$1.sig" "$1"
            return
        fi
        rm -f "$1.sig"
        echo "GPG signing failed (attempt $attempt/3)" >&2
    done
    fail "GPG signing of $(basename "$1") failed after 3 attempts"
}

# --- source state -----------------------------------------------------------

commit=$(cd "$repo_root" && git rev-parse --short HEAD)
if [[ -n "$(cd "$repo_root" && git status --porcelain)" ]]; then
    (cd "$repo_root" && git status --short >&2)
    fail "working tree is not clean; a release must build from a commit"
fi

# Vite also reads git-ignored .env*.local files and VITE_ environment
# variables, which the clean-tree check cannot see. A release refuses both.
ui_env_check() {
    local ignored variables
    ignored=$(cd "$ui_dir" && git ls-files --others --ignored --exclude-standard -- .env '.env.*')
    if [[ -n "$ignored" ]]; then
        sed 's|^|   frontend/jetkvm-ui/|' <<<"$ignored" >&2
        fail "git ignores these Vite env files, and a UI build would take their values; remove them"
    fi
    variables=$(compgen -e VITE_ || true)
    if [[ -n "$variables" ]]; then
        sed 's|^|   |' <<<"$variables" >&2
        fail "the environment sets these Vite variables, and a UI build would take their values; unset them"
    fi
}
ui_env_check

if [[ "$prerelease" != true ]]; then
    if [[ -n "$release_branch" ]]; then
        # Into its tracking ref by name: a clone of one branch updates only
        # FETCH_HEAD for a fetch of another one.
        (cd "$repo_root" && git fetch --quiet origin "+refs/heads/$release_branch:refs/remotes/origin/$release_branch")
        if ! (cd "$repo_root" && git merge-base --is-ancestor HEAD "origin/$release_branch"); then
            fail "stable $version with --release-branch $release_branch must build from a commit on origin/$release_branch; HEAD $commit is not"
        fi
        echo "Releasing from branch $release_branch (commit $commit) instead of main."
    else
        (cd "$repo_root" && git fetch --quiet origin main)
        if ! (cd "$repo_root" && git merge-base --is-ancestor HEAD origin/main); then
            fail "stable $version must build from a commit on origin/main; HEAD $commit is not"
        fi
    fi
    # The tag push runs the workflow from the tagged commit, so a commit
    # without it would push a tag that registers nothing.
    if ! (cd "$repo_root" && git cat-file -e "HEAD:.github/workflows/release-sync.yml" 2>/dev/null); then
        fail "stable $version must build from a commit that carries .github/workflows/release-sync.yml; HEAD $commit does not"
    fi
fi
# The existing-tag check and the change list need every release tag.
if ! (cd "$repo_root" && git fetch --quiet origin 'refs/tags/release/*:refs/tags/release/*'); then
    fail "could not fetch the release tags from origin"
fi
if (cd "$repo_root" && git rev-parse --quiet --verify "refs/tags/release/$version" >/dev/null); then
    fail "tag release/$version already exists"
fi

# --- what R2 already serves -------------------------------------------------

# Fills r2_summary; fails when the version exists or could never be served
# (the cloud serves the highest version). Runs again before the upload, since
# R2 has no atomic create.
r2_summary=()
r2_check() {
    local existing summary
    if ! existing=$(rclone lsf --dirs-only "$r2_path/" 2>&1); then
        fail "cannot list $r2_path/: $existing"
    fi
    if [[ -n "$existing" ]] && grep -qx "$version/" <<<"$existing"; then
        fail "version $version already exists at $r2_path/$version/"
    fi
    if ! summary=$(python3 "$repo_root/tools/release_check.py" "$version" <<<"$existing"); then
        echo "$summary" >&2
        exit 1
    fi
    mapfile -t r2_summary <<<"$summary"
}

if [[ "$have_rclone" == true ]]; then
    r2_check
else
    r2_summary=("R2 not checked: rclone is not installed")
fi

echo ">> release $version from $commit$on_branch"
printf '   %s\n' "${r2_summary[@]}"

# The previous release is the highest stable tag below X.Y.Z of this version;
# a prerelease of X.Y.Z comes after it too, and before release/X.Y.Z.
version_core="${version%%-*}"
previous_release=$( { (cd "$repo_root" && git tag -l 'release/*') \
        | sed -n 's|^release/\([0-9]\{1,\}\.[0-9]\{1,\}\.[0-9]\{1,\}\)$|\1|p'
    echo "$version_core"; } | sort -V -u \
    | awk -v core="$version_core" '$0 == core { print previous } { previous = $0 }')
if [[ -n "$previous_release" ]]; then
    echo "changes since release/$previous_release:"
    (cd "$repo_root" && git log --oneline "release/$previous_release..HEAD") | sed 's/^/  /'
else
    echo "no earlier release tag; no list of changes"
fi

# --- build ------------------------------------------------------------------

# The container sees the repository at its host path so a git worktree can
# resolve its .git file back to the main checkout.
git_common=$(cd "$repo_root" && git rev-parse --path-format=absolute --git-common-dir)
main_checkout=$(cd "$git_common/.." && pwd)
mounts=(-v "$repo_root:$repo_root")
if [[ "$main_checkout" != "$repo_root" ]]; then
    mounts+=(-v "$main_checkout:$main_checkout")
fi

# An existing sdkconfig wins over the defaults files, so each release build
# uses a new one in its (freshly removed) build directory.
idf_build() {
    docker run --rm "${mounts[@]}" -w "$repo_root" -e HOME=/tmp \
        -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
        "$idf_image" idf.py -B "$1" -DSDKCONFIG="$1/sdkconfig" \
        -DJETKVM_BOARD_VARIANT="$2" -DPROJECT_VER="$version" \
        -DJETKVM_WIFI_SETUP_LOG_CODE=0 -DJETKVM_MOCK_CLOUD_JWKS_URL= reconfigure build
}

# A leftover managed_components/ can hold differently patched sources, and
# its archive mtimes can predate a reused build's objects. Remove both; the
# container owns their files.
clean_release_tree() {
    echo ">> removing managed_components/ and build-release-<variant>/"
    local dirs=("$repo_root/managed_components") variant
    for variant in "${!skus[@]}"; do
        dirs+=("$repo_root/build-release-$variant")
    done
    docker run --rm "${mounts[@]}" "$idf_image" rm -rf "${dirs[@]}"
}

# The first source file newer than $2 below $1, skipping build output
# directories; files named build* (tools/patches/build_*.py) are sources.
# paraglide/ is generated from localization/messages/ by each UI build.
newer_source() {
    find "$1" -type d \( -name node_modules -o -name 'dist*' -o -name 'build*' \
        -o -name managed_components -o -name paraglide -o -name .git \
        -o -name .claude \) -prune -o \
        -type f -newer "$2" -print -quit
}

# --skip-ui-build takes only a bundle whose hashes build_ui recorded.
ui_bundle_record="$repo_root/build-release-stage/device-ui.sha256"
ui_bundle_hashes() {
    (cd "$ui_dir/dist-p4" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum)
}

# The firmware embeds frontend/jetkvm-ui/dist-p4 and must match it.
build_ui() {
    if [[ ! -d "$ui_dir/node_modules" ]]; then
        fail "$ui_dir/node_modules is missing; run 'HUSKY=0 npm ci' there first"
    fi
    echo ">> building the web UI"
    (cd "$ui_dir" && npm run build:device -- --outDir dist-p4)
    mkdir -p "$(dirname "$ui_bundle_record")"
    ui_bundle_hashes > "$ui_bundle_record"
}

ui_bundle_check() {
    local bundle="$ui_dir/dist-p4/index.html"
    if [[ ! -f "$bundle" ]]; then
        fail "$ui_dir/dist-p4 is missing; drop --skip-ui-build"
    fi
    local newer
    newer=$(newer_source "$ui_dir" "$bundle")
    if [[ -n "$newer" ]]; then
        fail "$newer is newer than the web UI bundle; drop --skip-ui-build"
    fi
    if [[ ! -s "$ui_bundle_record" || "$(ui_bundle_hashes)" != "$(<"$ui_bundle_record")" ]]; then
        fail "$ui_dir/dist-p4 is not the bundle that a release build recorded in $ui_bundle_record; drop --skip-ui-build"
    fi
}

artifact_hashes() {
    (cd "$1" && sha256sum "${artifacts[@]}")
}

# A reused build directory must postdate every source file and hold the
# images whose hashes a release build recorded.
build_dir_check() {
    local image="$1/jetkvm-mini.bin"
    if [[ ! -f "$image" ]]; then
        fail "$image is missing; drop --skip-build"
    fi
    local newer
    newer=$(newer_source "$repo_root" "$image")
    if [[ -n "$newer" ]]; then
        fail "$newer is newer than $image; drop --skip-build"
    fi
    local recorded="$1/release-images.sha256"
    if [[ ! -s "$recorded" || "$(artifact_hashes "$1" 2>/dev/null)" != "$(<"$recorded")" ]]; then
        fail "$1 does not hold the images that a release build recorded in $recorded; drop --skip-build"
    fi
}

# release_config.py lists each difference from the release configuration.
release_config_check() {
    local variant="$1" build_dir="$2" differences
    if differences=$(python3 "$repo_root/tools/release_config.py" \
        "$repo_root" "$build_dir" "$variant" "${!skus[@]}"); then
        return 0
    fi
    [[ -z "$differences" ]] || sed 's/^/   /' <<<"$differences" >&2
    if [[ "$skip_build" == true ]]; then
        fail "$build_dir does not hold the $variant release configuration; drop --skip-build"
    fi
    fail "$build_dir does not hold the $variant release configuration"
}

build_variant() {
    local variant="$1"
    local build_dir="$repo_root/build-release-$variant"
    # A file left in the build directory could seed the configuration.
    [[ ! -e "$build_dir" ]] || fail "$build_dir exists; a release build starts without it"
    mkdir -p "$build_dir"
    local log="$build_dir/release-build.log"
    if idf_build "$build_dir" "$variant" 2>&1 | tee "$log"; then
        return 0
    fi
    # A fresh build directory generates its sdkconfig before the TinyUSB patch
    # applies; CMake removes that sdkconfig and asks for one more run.
    if grep -q "Run the build again" "$log"; then
        echo ">> configure regenerated sdkconfig; building $variant again"
        idf_build "$build_dir" "$variant" 2>&1 | tee "$log"
        return
    fi
    return 1
}

# --skip-build reads no dist-p4: it stages the recorded images, and each
# image embeds the bundle of the release build that made it.
if [[ "$skip_build" != true ]]; then
    if [[ "$skip_ui_build" == true ]]; then
        ui_bundle_check
    else
        build_ui
    fi
    clean_release_tree
fi

# The OTA image must fit an app slot; the device downloads the whole image
# before esp_ota learns that it does not.
app_slot_size=$(awk -F'[ ,]+' '$1 == "ota_0" {print $5}' "$repo_root/partitions.csv")
[[ -n "$app_slot_size" ]] || fail "partitions.csv has no ota_0 entry"
app_slot_size=$((app_slot_size))

stage_root="$repo_root/build-release-stage/$version"
rm -rf "$stage_root"

for variant in "${!skus[@]}"; do
    sku="${skus[$variant]}"
    build_dir="$repo_root/build-release-$variant"
    if [[ "$skip_build" == true ]]; then
        build_dir_check "$build_dir"
    else
        echo ">> building $variant ($sku) at $version"
        build_variant "$variant"
    fi
    release_config_check "$variant" "$build_dir"
    if [[ "$skip_build" != true ]]; then
        artifact_hashes "$build_dir" > "$build_dir/release-images.sha256"
    fi
    stage_dir="$stage_root/skus/$sku"
    mkdir -p "$stage_dir"
    for artifact in "${artifacts[@]}"; do
        cp "$build_dir/$artifact" "$stage_dir/$artifact"
        sha256sum "$stage_dir/$artifact" | awk '{print $1}' > "$stage_dir/$artifact.sha256"
    done
    image_size=$(stat -c %s "$stage_dir/jetkvm-mini.bin")
    if (( image_size > app_slot_size )); then
        fail "$variant image is $image_size bytes; the app slot holds $app_slot_size"
    fi
    # Test builds: logged setup code, trusted e2e CA, test release key.
    if grep -aqF "Wi-Fi setup code: %s" "$stage_dir/jetkvm-mini.bin"; then
        fail "$variant image logs the Wi-Fi setup code; it is a bench e2e build"
    fi
    if grep -aqF "JetKVM Mini e2e mock cloud test CA" "$stage_dir/jetkvm-mini.bin"; then
        fail "$variant image trusts the e2e test CA; it is a bench e2e build"
    fi
    if grep -aqF "$test_key_fpr" "$stage_dir/jetkvm-mini.bin"; then
        fail "$variant image names the test release key; it is a bench e2e build"
    fi
    # A Mini checks an update against the release key by this fingerprint.
    if ! grep -aqF "$release_key_fpr" "$stage_dir/jetkvm-mini.bin"; then
        fail "$variant image does not name the JetKVM release key $release_key_fpr"
    fi
    # What the image says about itself must match what the release claims.
    read -r built_version built_sku < <(python3 "$repo_root/tools/mini_image.py" "$stage_dir/jetkvm-mini.bin")
    if [[ "$built_version" != "$version" || "$built_sku" != "$sku" ]]; then
        fail "$variant image is '$built_version' for '$built_sku'; expected '$version' for '$sku'"
    fi
    # The recovery image must carry that same app at the app partition offset
    # flash_args records; a stale merge would otherwise ship a different build.
    app_offset=$(awk '$2 == "jetkvm-mini.bin" {print $1}' "$build_dir/flash_args")
    if [[ -z "$app_offset" ]] || ! cmp -s --ignore-initial=0:$((app_offset)) \
        -n "$image_size" "$stage_dir/jetkvm-mini.bin" "$stage_dir/jetkvm-mini-full.bin"; then
        fail "$variant recovery image does not contain the staged jetkvm-mini.bin at ${app_offset:-?}"
    fi
done

# The signatures come after every image check, so that no rejected image is
# signed. Each one is verified against the release key when it is made.
if [[ "$prerelease" != true ]]; then
    for variant in "${!skus[@]}"; do
        for artifact in "${artifacts[@]}"; do
            sign_file "$stage_root/skus/${skus[$variant]}/$artifact"
        done
    done
    echo ">> signed with $SIGNING_KEY_FPR; each signature verifies against the release key $release_key_fpr"
fi

# The cloud UI is built from the same commit as the firmware, after the image
# checks above; the build writes only paraglide/ in the source tree.
cloud_stage="$repo_root/build-release-stage/cloud-app/$cloud_version"
rm -rf "$cloud_stage"
if [[ ! -d "$ui_dir/node_modules" ]]; then
    fail "$ui_dir/node_modules is missing; run 'HUSKY=0 npm ci' there first"
fi
# The firmware builds took a while; an env file can have appeared since.
ui_env_check
echo ">> building the cloud UI at /v/$cloud_version/"
(cd "$ui_dir" && VITE_CLOUD_ENABLE_VERSIONED_UI=true npm run build:prod -- \
    --base="/v/$cloud_version/" --outDir "$cloud_stage")
[[ -f "$cloud_stage/index.html" ]] || fail "the cloud UI build wrote no index.html"

echo
echo "staged $version from $commit$on_branch:"
(cd "$stage_root" && find . -type f -printf '  %-56P %s\n' | sort)
echo "  destination: $r2_path/$version/skus/<sku>/"
echo "  cloud UI: $(find "$cloud_stage" -type f | wc -l) files, destination: $cloud_app_path/$cloud_version/"

if [[ "$dry_run" == true ]]; then
    printf '  %s\n' "${r2_summary[@]}"
    echo "dry run: nothing uploaded"
    exit 0
fi

# The builds took a while; what R2 serves may have changed since the preflight.
r2_check
printf '  %s\n' "${r2_summary[@]}"
read -r -p "upload $version to $r2_path and $cloud_app_path, and tag release/$version? [y/N] " confirm
[[ "$confirm" == "y" ]] || { echo "cancelled"; exit 1; }

# The cloud UI goes first: devices that install this firmware open
# /v/$cloud_version/. Headers as in jetkvm/kvm's scripts/deploy_cloud_app.sh.
rclone copy --progress \
    --header-upload "x-amz-meta-jetkvm-version: $cloud_version" \
    --header-upload "x-amz-meta-jetkvm-build-ref: $(cd "$repo_root" && git rev-parse HEAD)" \
    "${branch_header[@]}" \
    --header-upload "x-amz-meta-jetkvm-build-timestamp: $(date -u +%FT%T%z)" \
    "$cloud_stage" "$cloud_app_path/$cloud_version"
echo "uploaded $cloud_app_path/$cloud_version/"

# Signatures go first and images only once every signature is in the bucket,
# so a partial upload never leaves an unsigned stable image.
if [[ "$prerelease" != true ]]; then
    for variant in "${!skus[@]}"; do
        for artifact in "${artifacts[@]}"; do
            verify_release_signature "$stage_root/skus/${skus[$variant]}/$artifact.sig" \
                "$stage_root/skus/${skus[$variant]}/$artifact"
        done
    done
    rclone copy --progress --include '*.sig' "$stage_root" "$r2_path/$version"
    rclone check --one-way --include '*.sig' "$stage_root" "$r2_path/$version" \
        || fail "the signatures of $version are not all at $r2_path/$version/; no image was uploaded"
fi
rclone copy --progress "$stage_root" "$r2_path/$version"

echo
echo "uploaded $r2_path/$version/"
(cd "$repo_root" && git tag "release/$version" && git push --quiet origin "release/$version")
echo "tagged release/$version at $commit$on_branch and pushed the tag"
printf '%s\n' "${r2_summary[@]:2}"

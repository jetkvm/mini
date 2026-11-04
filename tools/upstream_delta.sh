#!/usr/bin/env bash
# Show how frontend/jetkvm-ui differs from the upstream jetkvm/kvm ui/ tree
# it was imported from, so a Mini change can be sent upstream as one patch.
#
#   tools/upstream_delta.sh [--patch] [directory inside ui/, for example e2e]
#
# Without --patch it lists the files the Mini changed, added, renamed and
# removed, with line counts. With --patch it prints the diff of everything
# outside the Mini-only directory e2e/mini/: changed, added, renamed and
# removed files, ready for `git apply --directory=ui/<path>` in a jetkvm/kvm
# checkout. Files in e2e/mini/ are listed, never diffed.
#
# The base commit comes from the upstream tree URL in frontend/UPSTREAM.md.
# It must be in this repository: add the remote once with
#   git remote add upstream https://github.com/jetkvm/kvm.git && git fetch upstream dev
set -euo pipefail
patch=0
if [ "${1:-}" = "--patch" ]; then patch=1; shift; fi
sub="${1:-}"
while [ "${sub%/}" != "$sub" ]; do sub="${sub%/}"; done
case "$sub" in
    e2e/mini|e2e/mini/*)
        echo "e2e/mini is Mini-only and has no upstream counterpart; select e2e or the whole UI" >&2
        exit 2
        ;;
esac
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
# One sed, not grep | head: under pipefail a missing URL or an early head exit
# would stop the script before the message below.
base="$(sed -n 's|.*github\.com/jetkvm/kvm/tree/\([0-9a-f]*\)/ui.*|\1|p' frontend/UPSTREAM.md | sed -n 1p)"
if [ -z "$base" ]; then
    echo "no upstream tree URL in frontend/UPSTREAM.md" >&2
    exit 2
fi
if ! git cat-file -e "$base^{commit}" 2>/dev/null; then
    echo "upstream commit $base is not in this repository; fetch it first (see the header)" >&2
    exit 2
fi
upstream="$base:ui${sub:+/$sub}"
mini="HEAD:frontend/jetkvm-ui${sub:+/$sub}"
# The path selects a subtree that both sides have; the patch paths are then
# relative to it, for git apply --directory=ui/<path>.
for tree in "$upstream" "$mini"; do
    if [ "$(git cat-file -t "$tree" 2>/dev/null)" != tree ]; then
        echo "$sub is not a directory in both ui/ at $base and frontend/jetkvm-ui/;" \
            "select a directory both have, or the whole UI" >&2
        exit 2
    fi
done
# e2e/mini/ as a path inside the selected subtree; other subtrees hold none
# of it, and a mini/ directory there is upstream's.
case "$sub" in
    "") mini_dir=e2e/mini ;;
    e2e) mini_dir=mini ;;
    *) mini_dir= ;;
esac
outside_mini=()
[ -z "$mini_dir" ] || outside_mini=(":!$mini_dir")
if [ "$patch" = 1 ]; then
    git diff --binary --find-renames --src-prefix=a/ --dst-prefix=b/ \
        "$upstream" "$mini" -- "${outside_mini[@]}"
    exit 0
fi
echo "base $base  (frontend/UPSTREAM.md)"
echo
echo "Changed upstream files (lines added/removed):"
git diff --find-renames --numstat --diff-filter=M "$upstream" "$mini" |
    awk '{ printf "  +%-5s -%-5s %s\n", $1, $2, $3 }'
echo
if [ -n "$mini_dir" ]; then
    echo "Mini-only files (e2e/mini/):"
    git diff --find-renames --name-only --diff-filter=A "$upstream" "$mini" -- "$mini_dir" |
        sed 's/^/  /'
fi
added="$(git diff --find-renames --name-only --diff-filter=A "$upstream" "$mini" -- \
    "${outside_mini[@]}")"
if [ -n "$added" ]; then
    echo
    echo "New files for upstream:"
    echo "$added" | sed 's/^/  /'
fi
renamed="$(git diff --find-renames --name-status --diff-filter=R "$upstream" "$mini" |
    awk '{ print $2 " -> " $3 }')"
if [ -n "$renamed" ]; then
    echo
    echo "Upstream files the Mini renamed:"
    echo "$renamed" | sed 's/^/  /'
fi
removed="$(git diff --find-renames --name-only --diff-filter=D "$upstream" "$mini")"
if [ -n "$removed" ]; then
    echo
    echo "Upstream files the Mini removed:"
    echo "$removed" | sed 's/^/  /'
fi

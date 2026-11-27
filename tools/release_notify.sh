#!/usr/bin/env bash
# Comment on each issue that a release fixes, so that its reporter learns
# which release has the fix. The release-notify workflow runs this when
# tools/release_r2.sh pushes the release/<version> tag.
#
#   GH_REPO=<owner>/<name> tools/release_notify.sh [--dry-run] <version>
#
# The changes of a release are the commits of its tag that no earlier release
# tag has. A dev release counts every earlier release; a stable release counts
# only the stable ones, so an issue gets one comment for the first dev release
# with its fix and one for the first stable release with it. Earlier means an
# ancestor of the tag: the order of the history, not of the version numbers.
# Only between two tags of one commit does the version decide.
#
# A change names its pull request at the end of its first line, "(#12)", as a
# squash merge writes it. The issues are the ones that the pull request is
# linked to with a closing keyword ("Closes #5") or in its Development box.
# An issue of another repository gets no comment.
#
# Each comment carries a hidden mark with the version, and an issue that has
# the mark gets no second comment, so a second run posts nothing twice.
# A pull request or a list of comments that cannot be read stops the run with
# an error; run it again then.
# --dry-run prints the comments and posts none.
#
# Needs git with the release tags, gh (GH_TOKEN) and jq.
set -euo pipefail

dry_run=false
if [[ "${1:-}" == "--dry-run" ]]; then
    dry_run=true
    shift
fi
if [[ $# -ne 1 ]]; then
    sed -n 2,7p "$0" >&2
    exit 2
fi
version="${1#release/}"
repo="${GH_REPO:?set GH_REPO to <owner>/<name>}"
version_pattern='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-dev[0-9]{12})?$'
if ! [[ "$version" =~ $version_pattern ]]; then
    echo "not a release version: $version" >&2
    exit 2
fi
tag="release/$version"
if ! git rev-parse --quiet --verify "refs/tags/$tag^{commit}" >/dev/null; then
    echo "no tag $tag in this checkout" >&2
    exit 2
fi
stable=true
[[ "$version" == *-* ]] && stable=false

# True when version $1 is a release before version $2: a lower X.Y.Z, or the
# same X.Y.Z as a dev release with an earlier date, or as a dev release when
# $2 is the stable one.
version_before() {
    local core_a="${1%%-*}" core_b="${2%%-*}"
    if [[ "$core_a" != "$core_b" ]]; then
        [[ "$(printf '%s\n%s\n' "$core_a" "$core_b" | sort -V | sed -n 1p)" == "$core_a" ]]
        return
    fi
    [[ "$1" == *-* ]] || return 1
    [[ "$2" != *-* || "$1" < "$2" ]]
}

# The earlier releases that this one is compared with. Two tags of one commit
# have no order in the history, so there the version decides.
commit=$(git rev-parse "$tag^{commit}")
earlier=()
while read -r other; do
    [[ "$other" == "$tag" ]] && continue
    [[ "${other#release/}" =~ $version_pattern ]] || continue
    if [[ "$stable" == true && "$other" == *-* ]]; then continue; fi
    if [[ "$(git rev-parse "$other^{commit}")" == "$commit" ]]; then
        version_before "${other#release/}" "$version" && earlier+=("$other")
        continue
    fi
    git merge-base --is-ancestor "$other" "$tag" && earlier+=("$other")
done < <(git tag -l 'release/*')
if [[ ${#earlier[@]} -eq 0 ]]; then
    echo "$tag has no earlier release in its history; nothing to compare it with"
    exit 0
fi

# Pull request numbers, oldest change first.
mapfile -t pulls < <(git log --reverse --format=%s "$tag" --not "${earlier[@]}" \
    | sed -n 's/.*(#\([0-9]\{1,\}\))$/\1/p' | awk '!seen[$0]++')

errors=$(mktemp)
trap 'rm -f "$errors"' EXIT

# issue number -> "#12, #15", the pull requests that fix it.
declare -A fixed_by=()
order=()
for pull in "${pulls[@]}"; do
    if ! linked=$(gh pr view "$pull" --repo "$repo" --json closingIssuesReferences 2>"$errors"); then
        # A number that is an issue, not a pull request, has no linked issues.
        if grep -q 'Could not resolve to a PullRequest' "$errors"; then
            echo "#$pull is not a pull request of $repo; skipped"
            continue
        fi
        # Any other failure stops the run before the first comment: a skipped
        # pull request would lose its comments, because the next release does
        # not have its commit.
        cat "$errors" >&2
        echo "could not read pull request #$pull" >&2
        exit 1
    fi
    while read -r issue; do
        [[ -n "$issue" ]] || continue
        if [[ -z "${fixed_by[$issue]:-}" ]]; then
            order+=("$issue")
            fixed_by[$issue]="#$pull"
        else
            fixed_by[$issue]+=", #$pull"
        fi
    done < <(jq -r --arg repo "$repo" '.closingIssuesReferences[]
        | select("\(.repository.owner.login)/\(.repository.name)" == $repo) | .number' <<<"$linked")
done

mark="<!-- release-notify $version -->"
posted=0
for issue in "${order[@]}"; do
    if [[ "$stable" == true ]]; then
        text="The fix for this issue (${fixed_by[$issue]}) is in release [\`$version\`](https://github.com/$repo/releases/tag/$tag).

To install it, select **Settings > General > Check for Updates** on the Mini."
    else
        text="The fix for this issue (${fixed_by[$issue]}) is in dev release [\`$version\`](https://github.com/$repo/releases/tag/$tag).

To install it, turn on **Settings > Advanced > Dev Channel Updates** on the Mini, and then select **Settings > General > Check for Updates**."
    fi
    body="$text
$mark"
    # A failed read must not count as "no mark", or a second run would post
    # the comment again.
    if ! existing=$(gh issue view "$issue" --repo "$repo" --json comments --jq '.comments[].body'); then
        echo "could not read the comments of #$issue" >&2
        exit 1
    fi
    if grep -qF -- "$mark" <<<"$existing"; then
        echo "#$issue already has the comment for $version"
        continue
    fi
    if [[ "$dry_run" == true ]]; then
        printf 'would comment on #%s:\n%s\n\n' "$issue" "$text"
    else
        gh issue comment "$issue" --repo "$repo" --body "$body" >/dev/null
        echo "commented on #$issue (${fixed_by[$issue]})"
    fi
    posted=$((posted + 1))
done
echo "$tag: ${#pulls[@]} pull requests, ${#order[@]} fixed issues, $posted comments$([[ "$dry_run" == true ]] && echo ' (dry run)')"

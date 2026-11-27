"""tools/release_notify.sh against a repository made for the test and a gh
that is a script.

The history, oldest first, with the issues that each pull request closes:

    Initial                         release/1.0.0
    Fix A (#11)   closes 5
    Fix B (#12)   closes 6, and 99 of another repository
                                    release/1.0.1-dev202611010000
    Fix C (#13)   closes 7
                                    release/1.0.1-dev202611020000
    Note (#14)    14 is an issue, not a pull request
    Fix A again (#15)   closes 5
                                    release/1.0.1

A side branch from Initial has "Hotfix (#20)", closes 8, with the tag
release/1.0.0-dev202612010000: the newest date, but in no other tag's history.

GH_FAIL names a gh call, for example "pr view 12", that fails as a server
error does.

gh keeps the comments in a JSON file, so a test reads what was posted.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).parents[1] / "release_notify.sh"
REPO = "owner/mini"
CLOSES = {
    "11": [(REPO, 5)],
    "12": [(REPO, 6), ("other/repo", 99)],
    "13": [(REPO, 7)],
    "15": [(REPO, 5)],
    "16": [(REPO, 9)],
    "20": [(REPO, 8)],
}
GH = r'''#!/usr/bin/env python3
import json, os, sys
state_file = os.environ["GH_STATE"]
state = json.load(open(state_file))
args = sys.argv[1:]
number = args[2]
if " ".join(args[:3]) == os.environ.get("GH_FAIL"):
    sys.exit("HTTP 502: Bad Gateway (https://api.github.com/graphql)")
assert args[3:5] == ["--repo", state["repo"]], args
if args[:2] == ["pr", "view"]:
    assert args[5:] == ["--json", "closingIssuesReferences"], args
    if number not in state["closes"]:
        sys.exit(f"GraphQL: Could not resolve to a PullRequest with the number of {number}. (repository.pullRequest)")
    print(json.dumps({"closingIssuesReferences": [
        {"number": issue, "repository": {"name": repo.split("/")[1], "owner": {"login": repo.split("/")[0]}}}
        for repo, issue in state["closes"][number]]}))
elif args[:2] == ["issue", "view"]:
    assert args[5:] == ["--json", "comments", "--jq", ".comments[].body"], args
    print("\n".join(state["comments"].get(number, [])))
elif args[:2] == ["issue", "comment"]:
    assert args[5] == "--body", args
    state["comments"].setdefault(number, []).append(args[6])
    json.dump(state, open(state_file, "w"))
else:
    sys.exit(f"unexpected gh {args}")
'''


class ReleaseNotifyTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(subprocess.run, ["rm", "-rf", str(self.tmp)])
        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        (bin_dir / "gh").write_text(GH)
        (bin_dir / "gh").chmod(0o755)
        self.state = self.tmp / "gh.json"
        self.state.write_text(json.dumps({"repo": REPO, "closes": CLOSES, "comments": {}}))
        self.repo = self.tmp / "repo"
        self.repo.mkdir()
        self.env = {**os.environ, "PATH": f"{bin_dir}:{os.environ['PATH']}", "GH_STATE": str(self.state),
                    "GH_REPO": REPO, "GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_SYSTEM": os.devnull,
                    "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.com",
                    "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.com"}
        self.git("init", "-q", "-b", "main")
        self.commit("Initial", "release/1.0.0")
        self.commit("Fix A (#11)")
        self.commit("Fix B (#12)", "release/1.0.1-dev202611010000")
        self.commit("Fix C (#13)", "release/1.0.1-dev202611020000")
        self.commit("Note (#14)")
        self.commit("Fix A again (#15)", "release/1.0.1")
        self.git("checkout", "-q", "-b", "side", "release/1.0.0")
        self.commit("Hotfix (#20)", "release/1.0.0-dev202612010000")
        self.git("checkout", "-q", "main")

    def git(self, *args):
        subprocess.run(["git", *args], cwd=self.repo, env=self.env, check=True, capture_output=True)

    def commit(self, subject, tag=None):
        self.git("commit", "-q", "--allow-empty", "-m", subject)
        if tag:
            self.git("tag", tag)

    def run_script(self, *args, fail=""):
        return subprocess.run(["bash", str(SCRIPT), *args], cwd=self.repo, env={**self.env, "GH_FAIL": fail},
                              capture_output=True, text=True, timeout=30)

    def notify(self, *args):
        result = self.run_script(*args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def comments(self):
        """issue number -> the comments on it, in the order of their posting."""
        return json.loads(self.state.read_text())["comments"]

    def test_dev_release_comments_on_the_issues_that_no_earlier_release_fixed(self):
        self.notify("1.0.1-dev202611020000")
        comments = self.comments()
        # 5 and 6 were fixed in the dev release before; 8 is on the side branch.
        self.assertEqual(list(comments), ["7"])
        self.assertIn("(#13) is in dev release [`1.0.1-dev202611020000`]"
                      "(https://github.com/owner/mini/releases/tag/release/1.0.1-dev202611020000)", comments["7"][0])
        self.assertIn("Dev Channel Updates", comments["7"][0])

    def test_first_dev_release_after_a_stable_one_has_the_changes_since_it(self):
        self.notify("1.0.1-dev202611010000")
        self.assertEqual(sorted(self.comments()), ["5", "6"])

    def test_stable_release_comments_again_on_what_its_dev_releases_fixed(self):
        self.notify("1.0.1-dev202611010000")
        self.notify("1.0.1-dev202611020000")
        output = self.notify("release/1.0.1")
        comments = self.comments()
        # No 99: it is an issue of another repository. No 14: not a pull request.
        self.assertEqual(sorted(comments), ["5", "6", "7"])
        self.assertEqual([len(comments[issue]) for issue in ("5", "6", "7")], [2, 2, 2])
        stable = comments["5"][1]
        self.assertIn("(#11, #15) is in release [`1.0.1`]", stable)
        self.assertNotIn("Dev Channel", stable)
        self.assertIn("#14 is not a pull request", output)

    def test_second_run_posts_nothing(self):
        self.notify("1.0.1")
        before = self.comments()
        output = self.notify("1.0.1")
        self.assertEqual(self.comments(), before)
        self.assertIn("0 comments", output)

    def test_dry_run_prints_the_comments_and_posts_none(self):
        output = self.notify("--dry-run", "1.0.1-dev202611020000")
        self.assertEqual(self.comments(), {})
        self.assertIn("would comment on #7", output)

    def test_release_on_a_side_branch_has_only_its_own_changes(self):
        self.notify("1.0.0-dev202612010000")
        self.assertEqual(list(self.comments()), ["8"])

    def test_first_release_posts_nothing(self):
        self.notify("1.0.0")
        self.assertEqual(self.comments(), {})

    def test_unknown_version_fails(self):
        for version in ("1.0.2", "v1.0.1", "1.0.1-rc1"):
            self.assertEqual(self.run_script(version).returncode, 2, version)
        self.assertEqual(self.comments(), {})

    def test_two_dev_releases_of_one_commit_comment_once(self):
        first, second = "1.0.2-dev202611030000", "1.0.2-dev202611040000"
        for order in ((first, second), (second, first)):
            with self.subTest(order=order):
                self.state.write_text(json.dumps({"repo": REPO, "closes": CLOSES, "comments": {}}))
                self.git("checkout", "-q", "-B", "twice", "main")
                self.commit("Fix D (#16)", f"release/{first}")
                self.git("tag", f"release/{second}")
                for version in order:
                    self.notify(version)
                comments = self.comments()
                self.assertEqual(list(comments), ["9"])
                self.assertEqual(len(comments["9"]), 1)
                self.assertIn(f"dev release [`{first}`]", comments["9"][0])
                self.git("tag", "-d", f"release/{first}", f"release/{second}")

    def test_stable_release_with_a_dev_release_of_its_commit_gets_both_comments(self):
        self.git("tag", "release/1.0.1-dev202611050000", "release/1.0.1")
        self.notify("1.0.1-dev202611050000")
        # Since release/1.0.1-dev202611020000: only "Fix A again (#15)".
        self.assertEqual(list(self.comments()), ["5"])
        self.notify("1.0.1")
        self.assertEqual([len(found) for found in self.comments().values()], [2, 1, 1])

    def test_pull_request_that_cannot_be_read_stops_the_run_before_a_comment(self):
        result = self.run_script("1.0.1", fail="pr view 13")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("HTTP 502", result.stderr)
        self.assertEqual(self.comments(), {})

    def test_comments_that_cannot_be_read_post_no_second_comment(self):
        self.notify("1.0.1-dev202611020000")
        before = self.comments()
        result = self.run_script("1.0.1-dev202611020000", fail="issue view 7")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(self.comments(), before)


if __name__ == "__main__":
    unittest.main()

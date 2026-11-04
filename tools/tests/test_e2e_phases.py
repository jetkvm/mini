"""run.sh reports each phase and fails the run when either phase fails."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class PhaseResultTests(unittest.TestCase):
    def run_phases(self, phases, phase_1=0, phase_2=0, restore=0):
        source = Path(os.environ.get(
            "RUNNER_SOURCE", Path(__file__).parents[1] / "e2e/run.sh")).read_text()
        tail = "declare -A phase_rc" + source.split("declare -A phase_rc", 1)[1]
        with tempfile.NamedTemporaryFile() as skips:
            script = f'''
set -uo pipefail
E2E_SKIP_LOG={skips.name}
phases="{phases}"
ran=""
phase_1() {{ ran+="1"; rc={phase_1}; }}
phase_2() {{ ran+="2"; rc={phase_2}; }}
restore_osvc() {{ echo "ran $ran"; return {restore}; }}
''' + tail
            result = subprocess.run(["bash", "-c", script], capture_output=True, text=True,
                                    timeout=5)
        return result.returncode, result.stdout

    def test_both_phases_pass(self):
        status, out = self.run_phases("1 2")
        self.assertEqual(status, 0)
        self.assertIn("ran 12", out)
        self.assertIn("phase 1 (OS Services off): passed", out)
        self.assertIn("phase 2 (OS Services on): passed", out)

    def test_a_failed_first_phase_still_runs_the_second_and_fails_the_run(self):
        status, out = self.run_phases("1 2", phase_1=1)
        self.assertEqual(status, 1)
        self.assertIn("ran 12", out)
        self.assertIn("phase 1 (OS Services off): failed (rc=1)", out)
        self.assertIn("phase 2 (OS Services on): passed", out)

    def test_a_failed_second_phase_fails_the_run(self):
        status, out = self.run_phases("1 2", phase_2=2)
        self.assertEqual(status, 1)
        self.assertIn("phase 2 (OS Services on): failed (rc=2)", out)

    def test_one_phase_reports_the_other_as_not_run(self):
        status, out = self.run_phases("2")
        self.assertEqual(status, 0)
        self.assertIn("ran 2", out)
        self.assertIn("phase 1 (OS Services off): not run", out)

    def test_a_failed_bench_restore_fails_the_run(self):
        status, _ = self.run_phases("1 2", restore=1)
        self.assertEqual(status, 1)


if __name__ == "__main__":
    unittest.main()

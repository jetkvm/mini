"""The script ends only after a BLE run has put the Mini back."""
import argparse
import itertools
import unittest
from unittest.mock import patch

from tools.e2e import wifi_reprovision as reprov

ARGS = argparse.Namespace(host="mini")


def run(answers, joins, limit=1000):
    """provision() against a Mini whose HTTP port answers as `answers` says,
    one item per connection and the last one from then on, and whose BLE runs
    end as `joins` says. Each sleep and each BLE run takes a second of the
    clock. Returns (result, BLE runs made)."""
    clock = itertools.count()
    answer = iter(answers)
    last = [answers[-1]]
    made = []

    def reachable(_):
        last[0] = next(answer, last[0])
        return last[0]

    def send(*_):
        made.append(True)
        return joins[min(len(made), len(joins)) - 1]

    with patch.object(reprov, "reachable", side_effect=reachable), \
         patch.object(reprov, "send_network", side_effect=send), \
         patch.object(reprov.time, "monotonic", side_effect=lambda: next(clock)), \
         patch.object(reprov.time, "sleep"):
        return reprov.provision(ARGS, "password", limit), len(made)


class ReprovisionTests(unittest.TestCase):
    def test_one_failed_connection_is_not_the_erase(self):
        # A miss, then answers: no BLE run, and no end before the limit.
        self.assertEqual(run([True, False, True], [True], limit=50), (False, 0))

    def test_a_mini_that_answers_without_a_ble_run_ends_nothing(self):
        # Offline for three connections (a restart), then online, not in BLE
        # setup: the runs fail, and the answers do not end the script.
        result, runs = run([False, False, False, True], [False], limit=60)
        self.assertFalse(result)
        self.assertGreater(runs, 1)

    def test_ends_when_a_ble_run_joined_and_the_mini_answers(self):
        # The Mini answers only after the second BLE run, the one that joins.
        answers = [False, False, False] + [False] * 3 + [True]
        self.assertEqual(run(answers, [False, True]), (True, 2))

    def test_retries_when_a_join_never_reaches_http(self):
        # The first join's HTTP grace period passes without an answer.
        answers = [False] * 3 + [False] * 30 + [True]
        result, runs = run(answers, [True, True])
        self.assertTrue(result)
        self.assertGreaterEqual(runs, 2)


if __name__ == "__main__":
    unittest.main()

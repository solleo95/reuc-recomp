import math, random, statistics, sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "scripts" / "linux"))
import perf_stats as ps


def layouts(true_change, layout_sd, runs_sd, seeds, rng, base=100.0):
    """A synthetic experiment: each code layout adds the same random speed offset to A and to B (the point of pairing), each run its own noise."""
    a, b = {}, {}
    for s in seeds:
        offset = rng.gauss(0, layout_sd)
        a[s] = [base * (1 + offset) * (1 + rng.gauss(0, runs_sd)) for _ in range(2)]
        b[s] = [base * (1 + offset) * (1 + true_change) * (1 + rng.gauss(0, runs_sd)) for _ in range(2)]
    return a, b


class StatsTest(unittest.TestCase):
    def test_interval_of_a_known_sample(self):
        mean, half = ps.mean_ci([1.0, 2.0, 3.0, 4.0])
        self.assertAlmostEqual(mean, 2.5)
        self.assertAlmostEqual(half, 3.182 * statistics.stdev([1, 2, 3, 4]) / 2, places=6)
        self.assertEqual(ps.mean_ci([5.0])[1], math.inf)

    def test_pairing_removes_the_layout_offset(self):
        # layouts that differ by +-5% would bury a 1% gain unpaired; the paired ratios do not see them
        rng = random.Random(1)
        a, b = layouts(-0.01, 0.05, 0.001, range(1, 7), rng)
        change = ps.paired_change(a, b)
        self.assertLess(abs(change["change"] + 0.01), 0.002)
        self.assertLess(change["half"], 0.003)

    def test_a_real_gain_is_called_faster_and_a_real_loss_worse(self):
        rng = random.Random(2)
        work = {"change": -0.012, "half": 0.0002, "seeds": 6}
        for true, want in ((-0.015, "FASTER"), (0.015, "WORSE")):
            a, b = layouts(true, 0.02, 0.002, range(1, 7), rng)
            self.assertEqual(ps.verdict(work, ps.paired_change(a, b))[0], want)

    def test_nothing_is_claimed_for_an_effect_below_the_resolution(self):
        rng = random.Random(3)
        work = {"change": 0.0, "half": 0.0001, "seeds": 5}
        a, b = layouts(0.0, 0.02, 0.003, range(1, 6), rng)
        self.assertEqual(ps.verdict(work, ps.paired_change(a, b))[0], "INCONCLUSIVE")

    def test_less_work_with_an_unresolved_time_is_work_only(self):
        rng = random.Random(4)
        work = {"change": -0.013, "half": 0.0003, "seeds": 5}
        a, b = layouts(-0.002, 0.02, 0.004, range(1, 6), rng)
        self.assertEqual(ps.verdict(work, ps.paired_change(a, b))[0], "WORK-ONLY")

    def test_more_work_is_worse_whatever_the_time_says(self):
        rng = random.Random(5)
        work = {"change": 0.01, "half": 0.0003, "seeds": 5}
        a, b = layouts(0.0, 0.02, 0.004, range(1, 6), rng)
        self.assertEqual(ps.verdict(work, ps.paired_change(a, b))[0], "WORSE")

    def test_too_few_layouts_are_invalid(self):
        rng = random.Random(6)
        a, b = layouts(-0.05, 0.02, 0.001, range(1, 3), rng)
        work = {"change": -0.05, "half": 0.0, "seeds": 2}
        self.assertEqual(ps.verdict(work, ps.paired_change(a, b))[0], "INVALID")

    def test_runs_of_one_binary_that_disagree_are_invalid(self):
        self.assertEqual(ps.invalid_runs({1: [100.0, 100.01], 2: [100.0, 101.0]}), [2])

    def test_quick_check_judges_the_work_only(self):
        self.assertEqual(ps.work_verdict(107528.7, 106130.0)[0], "LESS-WORK")
        self.assertEqual(ps.work_verdict(107528.7, 108278.0)[0], "MORE-WORK")
        self.assertEqual(ps.work_verdict(107528.7, 107529.4)[0], "SAME-WORK")   # the run-to-run difference of one binary (0.0013%) is no change

    def test_false_alarm_rate_of_the_rule_with_no_change(self):
        # 400 experiments of an unchanged build: how often does the rule claim a time difference? (95% intervals: about 5% of the time, never systematically one way)
        rng = random.Random(7)
        claims = 0
        for _ in range(400):
            a, b = layouts(0.0, 0.02, 0.003, range(1, 7), rng)
            work = {"change": 0.0, "half": 0.0001, "seeds": 6}
            claims += ps.verdict(work, ps.paired_change(a, b))[0] in ("FASTER", "WORSE")
        self.assertLess(claims / 400, 0.08)


unittest.main(argv=[sys.argv[0]])

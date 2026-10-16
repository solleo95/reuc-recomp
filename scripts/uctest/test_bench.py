"""python -m unittest uctest.test_bench  (from scripts/): the bench statistics on synthetic fights."""
import random
import unittest

from . import bench


def fake_run(rng, factor, seconds=20, noise=0.01):
    """cost per frame = (8 + 0.015 * draws_per_frame) Mcycles * factor, the scene weight drifting between 900 and 1300 draws per frame."""
    rows = []
    for _ in range(seconds):
        draws_per_frame = rng.uniform(900, 1300)
        frames = rng.randint(100, 130)
        cost = (8 + 0.015 * draws_per_frame) * factor * (1 + rng.gauss(0, noise))
        rows.append((frames, int(draws_per_frame * frames), cost * frames))
    return rows


class BenchStatistics(unittest.TestCase):
    def test_a_five_percent_faster_build_is_found_despite_scene_drift(self):
        rng = random.Random(1)
        report = bench.analyse({"a": [fake_run(rng, 1.00) for _ in range(3)], "b": [fake_run(rng, 0.95) for _ in range(3)]})
        self.assertIn("FASTER", report)

    def test_identical_builds_show_no_difference(self):
        rng = random.Random(2)
        report = bench.analyse({"a": [fake_run(rng, 1.00) for _ in range(3)], "b": [fake_run(rng, 1.00) for _ in range(3)]})
        self.assertIn("no difference", report)

    def test_log_parsing_drops_the_partial_last_second(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as folder:
            log = "\n".join(f"[bench] presents 100 draws {1000 + i} guest_mcycles 5.00" for i in range(30)) + "\n"
            (Path(folder) / "run.log").write_text(log)
            rows = bench.read_seconds(Path(folder), 20)
        self.assertEqual(len(rows), 20)
        self.assertEqual(rows[-1][1], 1028)


if __name__ == "__main__":
    unittest.main()

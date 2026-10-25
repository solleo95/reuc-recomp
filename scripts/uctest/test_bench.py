"""python -m unittest uctest.test_bench  (from scripts/): the bench statistics on synthetic fights."""
import random
import unittest

from . import bench, demo


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
    def test_demo_cpu_is_fixed_even_with_stale_local_profile(self):
        import json
        import tempfile
        from pathlib import Path
        from unittest.mock import patch

        with tempfile.TemporaryDirectory() as folder, patch.object(demo, "ROOT", Path(folder)):
            config = Path(folder) / "build" / "bench-machine.json"
            config.parent.mkdir()
            args = demo.replay_args("game", "fight", 100, "saves")
            self.assertEqual(args[args.index("--guest-cpu")+1], "26")
            config.write_text(json.dumps({"guest_cpu": 4}))
            args = demo.replay_args("game", "fight", 100, "saves")
            self.assertEqual(args[args.index("--guest-cpu")+1], "26")
            args = demo.replay_args("game", "fight", 100, "saves", ["--guest-cpu", "26"])
            self.assertEqual(args.count("--guest-cpu"), 1)
            self.assertEqual(args[-2:], ["--guest-cpu", "26"])
            for invalid in (["--guest-cpu", "4"], ["--guest-cpu", "-1"], ["--guest-cpu"], ["--guest-cpu=4"]):
                with self.assertRaises(ValueError):
                    demo.replay_args("game", "fight", 100, "saves", invalid)

    def test_live_harness_pins_cpu_and_rejects_other_cpu_before_launch(self):
        import tempfile
        from pathlib import Path
        from unittest.mock import patch
        from .client import Game

        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); source=root/"source"; source.mkdir()
            for index, extra in enumerate(([], ["--guest-cpu", "26"])):
                run=root/str(index); run.mkdir()
                game=Game(run_dir=run, saves=source, game_args=extra)
                with patch("uctest.client.subprocess.Popen") as launch:
                    game._launch()
                command=launch.call_args.args[0]
                self.assertEqual(command.count("--guest-cpu"),1)
                self.assertEqual(command[command.index("--guest-cpu")+1],"26")
            run=root/"invalid"; run.mkdir()
            game=Game(run_dir=run, saves=source, game_args=["--guest-cpu","28"])
            with patch("uctest.client.subprocess.Popen") as launch:
                with self.assertRaises(ValueError):
                    game._launch()
                launch.assert_not_called()

    def test_demo_replay_records_binary_demo_and_launch_arguments(self):
        import hashlib
        import json
        import tempfile
        from pathlib import Path
        from unittest.mock import patch

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            exe, recording, meta = root/"game.exe", root/"fight.demo", root/"fight.json"
            exe.write_bytes(b"test executable"); recording.write_bytes(b"test demo")
            meta.write_text(json.dumps({"from": 0, "to": 60}))
            source, run = root/"source", root/"run"
            source.mkdir(); run.mkdir()
            with patch.object(demo, "paths", return_value=(recording, meta)), \
                    patch("uctest.client.SAVE_SOURCE", source), patch.object(demo.subprocess, "run") as launch:
                demo.replay_once(f"{exe}|--guest-cpu|26", "fight", run)
            manifest = json.loads((run/"launch.json").read_text())
            self.assertEqual(manifest["command"], launch.call_args.args[0])
            self.assertEqual(launch.call_args.kwargs.get("creationflags"), getattr(demo.subprocess,"CREATE_NO_WINDOW",0))
            self.assertEqual(manifest["command"][-2:], ["--guest-cpu", "26"])
            self.assertEqual(manifest["exe_sha256"], hashlib.sha256(exe.read_bytes()).hexdigest())
            self.assertEqual(manifest["demo_sha256"], hashlib.sha256(recording.read_bytes()).hexdigest())

    def test_netplay_pair_uses_distinct_cores_without_changing_offline_pin(self):
        from .client import guest_cpu_args
        self.assertEqual(guest_cpu_args([]), ["--guest-cpu", "26"])
        self.assertEqual(guest_cpu_args(["--net-host"]), ["--guest-cpu", "26"])
        self.assertEqual(guest_cpu_args(["--net-join", "127.0.0.1"]), ["--guest-cpu", "28"])
        self.assertEqual(guest_cpu_args(["--net-join", "127.0.0.1", "--guest-cpu", "28"]), [])
        with self.assertRaises(ValueError):
            guest_cpu_args(["--net-join", "127.0.0.1", "--guest-cpu", "26"])
        with self.assertRaises(ValueError):
            guest_cpu_args(["--guest-cpu", "28"])

    def test_demo_peak_fps_uses_elapsed_wall_time_and_complete_fight_intervals(self):
        log = "\n".join([
            "[fps] window_ms 1000.000 retraces 0:60 presents 500 fps 500.000",
            "[fps] window_ms 1000.000 retraces 6800:6900 presents 300 fps 300.000",
            "[fps] window_ms 1005.000 retraces 6900:6960 presents 150 fps 149.254",
            "[fps] window_ms 1200.000 retraces 6960:7020 presents 180 fps 150.000",
            "[fps] window_ms 1000.000 retraces 8400:8500 presents 250 fps 250.000",
        ])
        self.assertEqual(demo.read_peak_fps(log, {"from": 6843, "to": 8463}), 150.0)

    def test_demo_peak_fps_excludes_short_bursts(self):
        log = "[fps] window_ms 20.000 retraces 6900:6902 presents 100 fps 5000.000"
        self.assertIsNone(demo.read_peak_fps(log, {"from": 6843, "to": 8463}))

    def test_demo_peak_fps_missing_in_old_logs_is_unavailable(self):
        self.assertIsNone(demo.read_peak_fps("[perf] presents 160", {"from": 0, "to": 60}))

    def test_demo_slower_wall_with_higher_peak_is_marked_for_investigation(self):
        import contextlib
        import io
        import tempfile
        import types
        from pathlib import Path
        from unittest.mock import patch

        def replay(exe, name, folder):
            frames = 150 if exe == "a" else 160
            (folder / "run.log").write_text(f"[fps] window_ms 1000.000 retraces 6900:6960 presents {frames} fps {frames}.000")
            rows = [(6900 + i * 60, 100, 1, 1000 if exe == "a" else 1100) for i in range(27)]
            return rows, {"from": 6843, "to": 8463}

        output = io.StringIO()
        with tempfile.TemporaryDirectory() as folder, contextlib.redirect_stdout(output):
            root = Path(folder)
            with patch.object(demo, "ROOT", root), patch.object(demo, "DEMOS", root / "demos"), \
                    patch.object(demo, "replay_once", side_effect=replay), patch.object(demo.time, "time", side_effect=[1, 2]):
                self.assertEqual(demo.bench(types.SimpleNamespace(exes=["a", "b"], runs=1, demo="fight3")), 0)
        self.assertIn("150.0 -> 160.0", output.getvalue())
        self.assertIn("mixed result: retain for investigation", output.getvalue())

    def test_a_five_percent_faster_build_is_found_despite_scene_drift(self):
        rng = random.Random(1)
        report = bench.analyse({"a": [fake_run(rng, 1.00) for _ in range(3)], "b": [fake_run(rng, 0.95) for _ in range(3)]})
        self.assertIn("FASTER", report)

    def test_demo_rejects_different_work_even_with_equal_total_draws(self):
        import contextlib
        import io
        import tempfile
        import types
        from pathlib import Path
        from unittest.mock import patch

        def replay(exe, name, folder):
            (folder / "run.log").write_text("")
            rows = [(6900 + i*60, 100 + (1 if i==0 else -1 if i==1 else 0)*(exe=="b"), 1, 1000) for i in range(27)]
            return rows, {"from": 6843, "to": 8463}

        output = io.StringIO()
        with tempfile.TemporaryDirectory() as folder, contextlib.redirect_stdout(output):
            root = Path(folder)
            with patch.object(demo, "ROOT", root), patch.object(demo, "DEMOS", root / "demos"), \
                    patch.object(demo, "replay_once", side_effect=replay), patch.object(demo.time, "time", side_effect=[1, 2]):
                self.assertEqual(demo.bench(types.SimpleNamespace(exes=["a", "b"], runs=1, demo="fight3")), 1)
        self.assertIn("INVALID: guest work differs", output.getvalue())
        self.assertNotIn("-> FASTER", output.getvalue())

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

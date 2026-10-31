import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "scripts"))
from uctest import runsummary


def second(fps, p50, p99, scene, efb, copies, bloom, upscale, blur, render, retraces=(1,61), window_ms=1000.1):
    return (f"[fps] window_ms {window_ms} retraces {retraces[0]}:{retraces[1]} presents {int(fps)} fps {fps}\n"
            f"[gpu] passes: upscale 0.09 ms, smaa 0.00 ms, occlusion 0.00 ms, shadow 0.00 ms, motion blur {blur} ms (0 runs), rt_as (last packet) 0.00 ms, rt_shadows 0.00 ms, bloom 0.50 ms\n"
            f"[gpu-sum] per second: scene draws {scene} ms, efb draws {efb} ms, texture copies {copies} ms, bloom {bloom} ms, upscale {upscale} ms\n"
            f"[perf] presents {int(fps)} render {render} ms texture creates 0 CPU decodes 0 late-aim draws 0 cache evictions 0 cache MB 90\n"
            f"[frametime] n {int(fps)} avg 14.3 p50 {p50} p99 {p99} max {p99} ms\n")


LOG = ("noise line\n" + second(10.0, 99.0, 99.0, 1, 1, 1, 1, 1, 9.0, 1)   # outside the last 3 seconds
       + second(70.0, 14.0, 16.0, 300.0, 90.0, 80.0, 40.0, 6.0, 1.2, 400.0)
       + second(80.0, 12.0, 20.0, 330.0, 100.0, 90.0, 44.0, 8.0, 1.4, 440.0)
       + second(60.0, 16.0, 18.0, 360.0, 110.0, 100.0, 48.0, 10.0, 1.0, 480.0))


class RunSummaryTest(unittest.TestCase):
    def test_measurement_keeps_earliest_full_second_and_its_pacing(self):
        def block(fps, start, end, p99):
            return second(fps,12,p99,300,90,80,40,6,1,400,retraces=(start,end))
        log=block(250,60,120,99)+block(83,120,180,21)+block(70,180,240,18)+block(500,240,300,88)
        measured=runsummary.measurement_log(log,120,240)
        summary=runsummary.fps_summary(measured,10)
        self.assertEqual((summary["fps_min"],summary["fps_max"]),(70,83))
        self.assertEqual(summary["frame_p99_worst"],21)
        self.assertEqual(runsummary.gpu_summary(measured,10)["scene"],300)

    def test_measurement_excludes_partial_windows_and_missing_intervals(self):
        partial=second(250,2,3,1,1,1,1,1,1,1,retraces=(120,180),window_ms=999.5)
        self.assertEqual(runsummary.measurement_log(partial,120,180),"")
        self.assertEqual(runsummary.measurement_log(LOG,200,300),"")
        self.assertEqual(runsummary.measurement_log("",0,100),"")

    def test_fps_summary_covers_the_last_seconds_only(self):
        s = runsummary.fps_summary(LOG, 3)
        self.assertAlmostEqual(s["fps_mean"], 70.0)
        self.assertEqual((s["fps_min"], s["fps_max"]), (60.0, 80.0))
        self.assertAlmostEqual(s["frame_p50"], 14.0)
        self.assertAlmostEqual(s["frame_p99_mean"], 18.0)
        self.assertEqual(s["frame_p99_worst"], 20.0)

    def test_gpu_summary_means_per_category_and_total(self):
        s = runsummary.gpu_summary(LOG, 3)
        for key, value in {"scene": 330.0, "efb": 100.0, "copies": 90.0, "bloom": 44.0, "upscale": 8.0, "blur_ms_frame": 1.2, "render_ms_s": 440.0, "gpu_total_ms_s": 656.0}.items():
            self.assertAlmostEqual(s[key], value, msg=key)

    def test_no_runs_folder_content_gives_none(self):
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            self.assertIsNone(runsummary.newest_run(Path(folder)))
            self.assertIsNone(runsummary.newest_run(Path(folder) / "missing"))
            (Path(folder) / "a").mkdir()
            self.assertEqual(runsummary.newest_run(Path(folder)).name, "a")

    def test_empty_log_gives_an_empty_summary(self):
        self.assertEqual(runsummary.fps_summary("nothing here\n", 3), {})
        self.assertEqual(runsummary.gpu_summary("nothing here\n", 3), {})


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])

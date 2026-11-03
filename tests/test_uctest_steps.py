import collections, sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "scripts"))
from uctest import steps, menus
from uctest.client import StepFailed

class ScriptedGame:
    """Frames advance on every press; screens change according to a script."""
    def __init__(self, screens):
        self.screens = list(screens); self.presses = []; self.n = 0; self.counters = collections.Counter()
        self.frame = self._frame()
    def _frame(self):
        sig = menus.SCREENS[self.screens[0]].signature if self.screens else {}
        ui = [-1] * 12; menu = [0] * 4
        for key, value in sig.items():
            if isinstance(key, int): ui[key] = value
            else: menu[int(key[1:])] = 1 if value == "nz" else value
        return {"n": self.n, "retraces": self.n, "context": "menu", "ui": ui, "menu": menu, "speed": 1}
    def advance(self, frames): self.n += frames; self.frame = self._frame(); return self.frame
    def aim(self, x, y, settle_frames=0): self.n += settle_frames; self.frame = self._frame(); return self.n
    def press(self, button, frames=2, x=0.0, y=0.0):
        self.presses.append((button, x, y)); self.n += frames
        if len(self.screens) > 1 and not (len(self.presses) == 1 and getattr(self, "drop_first", False)): self.screens.pop(0)
        self.frame = self._frame(); return self.n
    def wait(self, predicate, frames=None, seconds=60, awaited=""):
        for _ in range(3):
            if predicate(self.frame): return self.frame
            self.n += 1; self.frame = self._frame()
        raise StepFailed("timeout " + awaited, {"awaited": awaited})
    def fail(self, message, awaited=""): raise StepFailed(message, {"awaited": awaited})

class CheckpointTest(unittest.TestCase):
    def test_perf_rejects_windows_containing_non_gameplay(self):
        from contextlib import nullcontext
        from types import SimpleNamespace
        from unittest.mock import Mock, patch
        from uctest import scenarios
        g=SimpleNamespace(frame={"retraces":120,"speed":1,"context":"gameplay"},run_dir=Path("."),
                          set_speed=Mock(),wait=Mock(),step=lambda name:nullcontext(),capture=lambda:"frame.png",
                          fail=Mock(side_effect=StepFailed("non-active gameplay",{})))
        with patch.object(scenarios,"reach_gameplay"),patch.object(steps,"measure_fps",return_value={"fps_mean":85,"non_active_windows":1200}):
            with self.assertRaises(StepFailed): scenarios.perf(g,SimpleNamespace(chapter=1,measure=30))

    def test_perf_records_bounds_after_capture_and_around_measurement(self):
        from contextlib import nullcontext
        from types import SimpleNamespace
        from unittest.mock import Mock, call, patch
        from uctest import scenarios
        g=SimpleNamespace(frame={"retraces":120,"speed":1,"context":"gameplay"},run_dir=Path("."),
                          set_speed=Mock(),wait=Mock(),step=lambda name:nullcontext())
        def capture():
            g.frame["retraces"]=125
            return "frame.png"
        def measure(game,seconds,csv_path):
            game.frame["retraces"]=725
            return {"fps_mean":77}
        g.capture=capture
        with patch.object(scenarios,"reach_gameplay"),patch.object(steps,"measure_fps",side_effect=measure):
            result=scenarios.perf(g,SimpleNamespace(chapter=1,measure=10))
        self.assertEqual((result["measure_from_retrace"],result["measure_to_retrace"]),(125,725))
        self.assertEqual((result["capture"],result["fps_mean"]),("frame.png",77))
        self.assertEqual(g.set_speed.call_args_list,[call(8),call(1)])

    def test_route_goes_to_the_requested_chapter(self):
        from uctest import scenarios
        self.assertEqual(scenarios.route(1)[3], "chapter_1")
        self.assertEqual(scenarios.route(2)[3], "chapter_2")
        with self.assertRaises(ValueError): scenarios.route(9)
    def test_resolve_saves_accepts_none_a_checkpoint_name_or_a_directory(self):
        import tempfile
        from uctest import client
        self.assertIsNone(client.resolve_saves(None))
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(client.resolve_saves(d), Path(d))
            (Path(d) / "after-ch1").mkdir()
            old, client.CHECKPOINTS = client.CHECKPOINTS, Path(d)
            try:
                self.assertEqual(client.resolve_saves("after-ch1"), Path(d) / "after-ch1")
                with self.assertRaises(ValueError): client.resolve_saves("missing")
            finally: client.CHECKPOINTS = old

class PauseSceneTest(unittest.TestCase):
    def test_camera_roundoff_keeps_scene_frozen(self):
        from uctest import scenarios
        a = {"view": [18.2863064, -0.033859171], "enemies": [(160, [507.08, -0.5, 1.491], 20)], "life": 1}
        b = dict(a, view=[18.2863102, -0.0338591784])
        self.assertTrue(scenarios._same_scene(a, b))
        self.assertTrue(scenarios._same_scene(dict(a, view=None), dict(a, view=None)))

    def test_pause_still_detects_motion_damage_and_missing_camera(self):
        from uctest import scenarios
        a = {"view": [18.2863064], "enemies": [(160, [507.08, -0.5, 1.491], 20)], "life": 1}
        for b in (dict(a, view=[18.287]), dict(a, view=None), dict(a, view=[]),
                  dict(a, life=.9), dict(a, enemies=[(160, [507.09, -0.5, 1.491], 20)]),
                  dict(a, enemies=[(160, [507.08, -0.5, 1.491], 10)]), dict(a, view=[float("nan")])):
            with self.subTest(scene=b):
                self.assertFalse(scenarios._same_scene(a, b))

class MissionGame(ScriptedGame):
    """The mission screen with the pre-mission cutscene running (player not valid); the first press of `end_with` ends it. A tutorial page can be up instead."""
    def __init__(self, end_with, tutorial=False):
        self.end_with, self.tutorial, self.done = end_with, tutorial, False
        super().__init__(["mission"])
    def _frame(self):
        frame = super()._frame()
        frame.update(context="gameplay", player={"valid": self.done}, tutorial_loaded=self.tutorial and not self.done, tutorial=5)
        return frame
    def press(self, button, frames=2, x=0.0, y=0.0, **kw):
        self.presses.append((button, x, y)); self.press_log = getattr(self, "press_log", []) + [(button, frames, self.n)]; self.n += frames
        self.done = self.done or (button == self.end_with and len(self.presses) >= getattr(self, "needed", 1))
        self.frame = self._frame(); return self.n

class WaitGameplayTest(unittest.TestCase):
    def test_pre_mission_cutscene_is_skipped_with_b(self):
        g = MissionGame("B")
        steps.wait_gameplay(g)
        self.assertEqual(g.presses[0][0], "B")
    def test_cutscene_skip_presses_are_spaced_and_not_held(self):
        # On the Deck B pressed every 6 ticks skipped the cutscene only now and then, and a held B never did (measured: 408 / 1188 / 3573 ticks and never);
        # 4-sample presses 24 ticks apart skipped it every time in about 50-90 ticks.
        g = MissionGame("B"); g.needed = 3
        steps.wait_gameplay(g)
        presses = [p for p in g.press_log if p[0] == "B"]
        self.assertEqual(len(presses), 3)
        self.assertTrue(all(frames == 4 for _, frames, _ in presses), presses)
        self.assertTrue(all(b[2] - a[2] >= 24 for a, b in zip(presses, presses[1:])), presses)
    def test_a_mission_page_that_only_a_ends_gets_a_after_b_has_done_nothing(self):
        g = MissionGame("A")
        steps.wait_gameplay(g, seconds=5)
        buttons = [p[0] for p in g.presses]
        self.assertEqual(buttons[0], "B")
        self.assertEqual(buttons[-1], "A")
        self.assertEqual(buttons[:10], ["B"] * 10)   # B alone first: it ends the cutscene in 2-4 presses
    def test_tutorial_page_is_dismissed_with_a(self):
        g = MissionGame("A", tutorial=True)
        steps.wait_gameplay(g)
        self.assertEqual(g.presses[0][0], "A")

class StepsTest(unittest.TestCase):
    def test_select_presses_target_and_sees_change(self):
        t = menus.TARGETS["save_slot_1"]; g = ScriptedGame([t.screen, "scenario"])
        steps.select(g, "save_slot_1")
        self.assertEqual(g.presses, [("A", t.x, t.y)])
    def test_select_retries_when_screen_does_not_change(self):
        t = menus.TARGETS["save_slot_1"]; g = ScriptedGame([t.screen, "scenario"]); g.drop_first = True
        steps.select(g, "save_slot_1")
        self.assertEqual(len(g.presses), 2)
    def test_select_wrong_screen_fails(self):
        g = ScriptedGame(["scenario"])
        with self.assertRaises(StepFailed): steps.select(g, "save_slot_1")
    def test_measure_rejects_speed(self):
        g = ScriptedGame(["scenario"]); g.frame["speed"] = 4
        with self.assertRaises(StepFailed): steps.measure_fps(g, 1)

if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])

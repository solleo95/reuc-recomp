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

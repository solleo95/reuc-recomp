import json, sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / "scripts"))
from uctest.autoplay import pick_target, Autoplay
from uctest import vision

def frame(n, context="gameplay", enemies=(), magazine=6, ui=None, tutorial=0x00100000, **extra):
    return {"n": n, "context": context, "speed": 1, "ui": ui or [-1]*12, "tutorial": tutorial, "tutorial_loaded": False,
            "player": {"valid": True, "magazine": magazine, "life": 1.0}, "enemies": list(enemies), **extra}

class FakeGame:
    def __init__(self, frames): self.frames = frames; self.i = 0; self.inputs = []
    @property
    def frame(self): return self.frames[min(self.i, len(self.frames) - 1)]
    def aim(self, x, y, settle_frames=0, **kw): self.inputs.append(("aim", round(x, 3), round(y, 3))); self.i += 1; return self.i
    def press(self, button, frames=2, x=0.0, y=0.0, **kw): self.inputs.append(("press", button, round(x, 3), round(y, 3))); self.i += 1; return self.i
    def key(self, code, frames=2, **kw): self.inputs.append(("key", code)); self.i += 1; return self.i
    def capture(self): return "c.png"
    def advance(self, ticks): self.i += 1; return self.frame
    def wait(self, predicate, frames=None, seconds=60, awaited=""): self.i += 1; return self.frame

E = lambda id, hx, hy, alive=True, visible=True: {"id": id, "alive": alive, "visible": visible, "head": [hx, hy], "body": [hx, hy + 0.2]}
PROJECTION = [1.47055817, 0, 0, 0, 0, -2.37642193, 0, 0, 0, 0, -1.00000906, -0.010000091, 0, 0, -1, 0]
IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0]

class VisionTest(unittest.TestCase):
    def test_project_centre_right_up_behind(self):
        x, y, depth = vision.project(IDENTITY, PROJECTION, (0, 0, -10)); self.assertAlmostEqual(x, 0); self.assertAlmostEqual(y, 0); self.assertAlmostEqual(depth, 10)
        self.assertAlmostEqual(vision.project(IDENTITY, PROJECTION, (1, 0, -10))[0], 0.1176, places=3)
        self.assertAlmostEqual(vision.project(IDENTITY, PROJECTION, (0, 1, -10))[1], -0.1426, places=3)   # above the axis is up (negative aim y)
        self.assertIsNone(vision.project(IDENTITY, PROJECTION, (0, 0, 5)))
    def test_enrich_aims_at_head_then_torso(self):
        raw = {"view": IDENTITY, "enemies": [{"id": 1, "root": [0, -8.25, -30], "head": True}, {"id": 2, "root": [0, -8.25, -30], "head": False}, {"id": 3, "root": [0, 0, 5], "head": True}]}
        e = vision.enrich(raw, PROJECTION)["enemies"]
        self.assertTrue(e[0]["visible"] and e[0]["head"][1] < e[1]["head"][1])   # head point is above the torso point
        self.assertFalse(e[2]["visible"] or e[2]["alive"] is False)            # behind the camera: not visible
        self.assertIsNone(e[2]["head"])

class AutoplayTest(unittest.TestCase):
    def test_pick_nearest_visible_alive(self):
        f = frame(1, enemies=[E(1, 0.8, 0.0), E(2, 0.1, 0.1), E(3, 0.0, 0.0, alive=False), E(4, 0.05, 0.0, visible=False)])
        self.assertEqual(pick_target(f, (0, 0))["id"], 2)
    def test_ignored_enemy_is_skipped(self):
        f = frame(1, enemies=[E(1, 0.0, 0.0), E(2, 0.5, 0.5)])
        self.assertEqual(pick_target(f, (0, 0), ignore={1})["id"], 2)
    def test_prefers_closer_enemy_at_equal_aim_distance(self):
        near, far = dict(E(1, 0.2, 0.0), depth=20), dict(E(2, -0.2, 0.0), depth=60)
        self.assertEqual(pick_target(frame(1, enemies=[far, near]), (0, 0))["id"], 1)
    def test_gives_up_on_an_enemy_after_thirty_shots(self):
        g = FakeGame([frame(n, enemies=[E(7, 0.1, 0.1)]) for n in range(1, 400, 7)])
        with tempfile.TemporaryDirectory() as d:
            bot = Autoplay(g, Path(d) / "a.jsonl")
            for _ in range(60): bot.step()
            self.assertTrue(any(json.loads(l)["event"] == "ignore" for l in (Path(d) / "a.jsonl").read_text().splitlines()))
    def test_enrich_marks_zombies_with_no_hp_left_dead(self):
        raw = {"view": IDENTITY, "enemies": [{"id": 1, "root": [0, -8.25, -30], "head": True, "hp": 10.0}, {"id": 2, "root": [0, -8.25, -30], "head": False, "hp": -0.0}, {"id": 3, "root": [0, -8.25, -30], "head": True}]}
        e = vision.enrich(raw, PROJECTION)["enemies"]
        self.assertEqual([x["alive"] for x in e], [True, False, True])   # hp <= 0 is dead; no hp field (other classes) stays alive
        self.assertIsNone(pick_target({"enemies": [e[1]]}, (0, 0)))
    def test_other_actor_classes_are_not_targets(self):
        prop = dict(E(1, 0.0, 0.0), cls=0x80408534)
        zombie = dict(E(2, 0.5, 0.5), cls=0x803F2B60)
        self.assertEqual(pick_target(frame(1, enemies=[prop, zombie]), (0, 0))["id"], 2)
        self.assertIsNone(pick_target(frame(1, enemies=[prop]), (0, 0)))
    def test_fires_at_the_screen_centre_when_no_target_is_visible(self):
        g = FakeGame([frame(n * 10) for n in range(1, 10)])
        with tempfile.TemporaryDirectory() as d:
            bot = Autoplay(g, Path(d) / "a.jsonl")
            for _ in range(6): bot.step()
            self.assertTrue(any(json.loads(l)["event"] == "centre" for l in (Path(d) / "a.jsonl").read_text().splitlines()))
        self.assertIn(("press", "B", 0.0, 0.0), g.inputs)
    def test_unknown_actors_are_not_aimed_at_the_bot_fires_at_the_centre(self):
        g = FakeGame([frame(n * 10, enemies=[dict(E(1, 0.4, 0.3), cls=0x80408534)]) for n in range(1, 40)])
        with tempfile.TemporaryDirectory() as d:
            bot = Autoplay(g, Path(d) / "a.jsonl")
            for _ in range(30): bot.step()
        shots = {(i[2], i[3]) for i in g.inputs if i[:2] == ("press", "B")}
        self.assertEqual(shots, {(0.0, 0.0)})
    def test_qte_context_keeps_shooting_instead_of_shaking_the_aim(self):
        g = FakeGame([frame(n * 10, context="qte", enemies=[E(5, 0.3, -0.4)] if n % 2 else []) for n in range(1, 30)])
        with tempfile.TemporaryDirectory() as d:
            bot = Autoplay(g, Path(d) / "a.jsonl")
            for _ in range(20): bot.step()
        self.assertEqual({i[:2] for i in g.inputs}, {("press", "B")})   # no sideways aim jerks, no A
        self.assertEqual({(i[2], i[3]) for i in g.inputs}, {(0.3, -0.4), (0.0, 0.0)})
    def test_shoots_head(self):
        g = FakeGame([frame(1, enemies=[E(5, 0.3, -0.4)])] * 3)
        with tempfile.TemporaryDirectory() as d:
            Autoplay(g, Path(d) / "a.jsonl").step()
        self.assertIn(("press", "B", 0.3, -0.4), g.inputs)
    def test_reloads_when_empty(self):
        g = FakeGame([frame(1, magazine=0)] * 3)
        with tempfile.TemporaryDirectory() as d:
            Autoplay(g, Path(d) / "a.jsonl").step()
        self.assertTrue(any(i[0] == "key" for i in g.inputs))
    def test_menu_advances(self):
        g = FakeGame([frame(1, context="menu")] * 3)
        with tempfile.TemporaryDirectory() as d:
            Autoplay(g, Path(d) / "a.jsonl").step()
        self.assertTrue(any(i[0] == "press" and i[1] == "A" for i in g.inputs))
    def test_chapter_select_after_a_mission_ends_the_run_as_chapter_done(self):
        select = frame(500, context="menu", ui=[-1, 9] + [-1] * 10, menu=[1, 0, 0, 1])   # chapter select after a finished chapter: ui[1]=9, stale page words
        g = FakeGame([frame(1, enemies=[]), dict(select, n=2, retraces=500)])
        with tempfile.TemporaryDirectory() as d:
            bot = Autoplay(g, Path(d) / "a.jsonl")
            self.assertIsNone(bot.step())
            self.assertEqual(bot.step(), "chapter_done")
        g2 = FakeGame([dict(select, n=3, retraces=900)])   # the same page without a mission played (a start from the menu) is not "done"
        with tempfile.TemporaryDirectory() as d:
            self.assertIsNone(Autoplay(g2, Path(d) / "a.jsonl").step())
    def test_menu_pages_get_their_own_target(self):
        character = frame(500, context="menu", ui=[-1, 4] + [-1] * 10, menu=[0, 1, 0, 0])
        g = FakeGame([character])
        with tempfile.TemporaryDirectory() as d:
            Autoplay(g, Path(d) / "a.jsonl").step()
        self.assertIn(("press", "A", -0.24, -0.067), g.inputs)
    def test_counts_kill_when_enemy_disappears(self):
        g = FakeGame([frame(1, enemies=[E(5, 0.3, -0.4)]), frame(2, enemies=[])])
        with tempfile.TemporaryDirectory() as d:
            bot = Autoplay(g, Path(d) / "a.jsonl"); bot.step(); bot.step()
            self.assertEqual(bot.kills, 1)
            self.assertTrue(any(json.loads(l)["event"] == "kill" for l in (Path(d) / "a.jsonl").read_text().splitlines()))
    def test_game_over_continues_and_counts_death(self):
        g = FakeGame([frame(1, context="menu", ui=[-1, -1, -1, -1, 7, -1, -1, -1, -1, -1, -1, 0])] * 3)
        with tempfile.TemporaryDirectory() as d:
            bot = Autoplay(g, Path(d) / "a.jsonl"); bot.step()
            self.assertEqual(bot.deaths, 1)
        self.assertTrue(any(i[0] == "press" and i[1] == "A" for i in g.inputs))
    def test_stuck_stops_with_reason(self):
        frames = [frame(n, enemies=[]) for n in range(0, 20000, 600)]
        g = FakeGame(frames)
        with tempfile.TemporaryDirectory() as d:
            bot = Autoplay(g, Path(d) / "a.jsonl", stuck_seconds=60)
            reason = None
            for _ in range(len(frames)):
                reason = bot.step()
                if reason: break
            self.assertEqual(reason, "stuck")
            lines = [json.loads(l) for l in (Path(d) / "a.jsonl").read_text().splitlines()]
            self.assertEqual(lines[-1]["event"], "stop")

if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])

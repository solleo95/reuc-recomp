"""Scripted bot: plays as far as it can from the game's own state (enemy heads from guest memory), logging every decision.

One `step()` = one decision from the latest frame. All timing uses game time (retraces), so it works at speed 1..8.
"""
import json
import time

from . import vision
from .menus import TARGETS, screen_of
from .steps import KEYS, PRESS_FRAMES

SHOT_FRAMES = 2    # trigger held 2 KPAD frames, released 1: measured at speed 8, 14 of 15 pulls fire (a 1-frame hold fires 12 of 26); beyond that only more game frames per second help (TD-050)
SHOT_CYCLE = SHOT_FRAMES + 1
MENU_FLOW = {"title": "title_start", "save_select": "save_slot_1", "scenario": "scenario_train", "character": "character_rebecca",
             "loadout": "loadout_confirm", "start_dialog": "start_yes"}   # page -> the button that moves on towards the next mission
ENEMY_CLASSES = {0x803F2B60}   # actor classes with a known position layout (train zombies, G22); other actors are bosses/props: not aimed at
CAPTURE_EVERY_TICKS = 1800


def pick_target(frame, crosshair, ignore=()):
    """Alive, visible, not ignored enemy whose aim point is nearest to the crosshair; closer enemies (smaller depth) are preferred."""
    best, best_key = None, None
    for e in frame.get("enemies") or []:
        if not e.get("alive", True) or not e.get("visible") or not e.get("head") or e["id"] in ignore or e.get("cls") not in (None, *ENEMY_CLASSES):
            continue
        dx, dy = e["head"][0] - crosshair[0], e["head"][1] - crosshair[1]
        key = dx * dx + dy * dy + 0.01 * (e.get("depth") or 0)
        if best_key is None or key < best_key:
            best, best_key = e, key
    return best


class Autoplay:
    def __init__(self, game, log_path, *, stuck_seconds=300, max_failures=5, projection=None, god=False, speed=1):
        self.g, self.stuck_seconds, self.max_failures, self.god, self.speed = game, stuck_seconds, max_failures, god, speed
        self.log_path = log_path
        open(log_path, "w").close()
        self.projection = projection
        self.aim = (0.0, 0.0)
        self.kills = self.deaths = self.shots = self.frames = self.centres = 0
        self.last_words = None
        self.in_mission = False   # index of the chapter being played; set once gameplay was seen
        self.furthest = None
        self.marker, self.marker_t = None, 0.0
        self.deaths_without_progress = 0
        self.seen = set()
        self.last_menu_tick = -10**9
        self.next_capture = CAPTURE_EVERY_TICKS
        self.stopped = None
        self.last_n = None
        self.shots_at = {}       # enemy id -> shots without a kill
        self.ignore_until = {}   # enemy id -> tick until which it is skipped (unreachable or already down)
        self.last_shot_n = self.last_tutorial_n = -10**9

    # logging ---------------------------------------------------------
    def event(self, kind, **fields):
        f = self.g.frame
        with open(self.log_path, "a") as log:   # reopened per line: flushed and never holds the file (Windows)
            log.write(json.dumps({"t": f.get("n", 0), "tick": f.get("retraces"), "event": kind, **fields}) + "\n")

    def stop(self, reason):
        self.event("stop", reason=reason, kills=self.kills, deaths=self.deaths)
        self.stopped = reason
        return reason

    # one decision ----------------------------------------------------
    def step(self):
        f = self.g.frame
        if not f:
            return None
        if f["n"] == self.last_n and hasattr(self.g, "request"):   # decide once per game frame: input commands are scheduled per frame
            f = self.g.wait(lambda fr: fr["n"] != self.last_n, seconds=30, awaited="next game frame")
        self.last_n = f["n"]
        self.frames = f["n"]
        tick = f.get("retraces", f["n"])
        now = tick / 60
        screen = screen_of(f)
        self.furthest = screen or self.furthest
        marker = (screen, self.kills)
        if marker != self.marker:
            if marker[1] != (self.marker or (None, 0))[1]:
                self.deaths_without_progress = 0
            self.marker, self.marker_t = marker, now
        elif now - self.marker_t >= self.stuck_seconds:
            return self.stop("stuck")
        if self.projection is None and f.get("context") == "gameplay" and hasattr(self.g, "request"):
            self.projection = self._fetch_projection()
        f = vision.enrich(f, self.projection)
        context = f.get("context")
        if f.get("tutorial_loaded") and f.get("tutorial") not in (0, 0x100000):   # a tutorial page is up (the context may still read gameplay)
            if f["n"] - self.last_tutorial_n >= 8:
                self.last_tutorial_n = f["n"]
                self.event("tutorial", state=f.get("tutorial"))
                self.g.press("A", PRESS_FRAMES, wait=False)
            return None
        if tick >= self.next_capture:
            self.next_capture = tick + CAPTURE_EVERY_TICKS
            seen = [[e["id"], e.get("cls") and hex(e["cls"]), e.get("visible"), e.get("depth") and round(e["depth"], 1), e.get("head") and [round(v, 2) for v in e["head"]]] for e in f.get("enemies") or []]
            self.event("capture", path=self.g.capture(), enemies=seen)   # [id, class, visible, depth, head aim]: what the bot saw at the time of the screenshot
        self.in_mission |= context in ("gameplay", "qte")
        if context in ("gameplay", "qte"):   # a grab (qte) is answered by shooting too, never by jerking the aim
            self._gameplay(f)
        elif screen == "game_over":
            self._game_over(f, tick)
        elif context == "menu":
            reason = self._menu(f, tick)
            if reason:
                return reason
        else:   # inactive: movie or loading
            if tick - self.last_menu_tick >= 30:
                self.last_menu_tick = tick
                self.g.press("A", PRESS_FRAMES, wait=False)
        if self.deaths_without_progress >= self.max_failures:
            return self.stop("stuck")
        return None

    def _fetch_projection(self):
        try:
            projection = self.g.request({"cmd": "draws"})["projection"]
            return projection or None
        except Exception:   # no projection yet; try again next step
            return None

    def _gameplay(self, f):
        ids = {e["id"] for e in f.get("enemies") or [] if e.get("alive", True)}
        for gone in sorted(self.seen - ids):
            self.kills += 1
            self.event("kill", id=gone)
        self.seen = ids
        magazine = (f.get("player") or {}).get("magazine")
        if magazine == 0:
            self.event("reload")
            self.g.key(KEYS["reload"], wait=False)
            return
        tick = f.get("retraces", f["n"])
        ignore = {i for i, until in self.ignore_until.items() if until > tick}
        target = pick_target(f, self.aim, ignore)
        if target:
            x, y = (max(-1.0, min(1.0, v)) for v in target["head"])
            self.aim = (x, y)
            if f["n"] - self.last_shot_n >= SHOT_CYCLE:   # one trigger pull per cycle; in between only the aim follows the target
                self.last_shot_n = f["n"]
                self.g.press("B", SHOT_FRAMES, x, y, wait=False)
                self.shots += 1
                self.shots_at[target["id"]] = self.shots_at.get(target["id"], 0) + 1
                if self.shots_at[target["id"]] >= 30:   # 30 shots without a kill: unreachable or down; try another for 10 s
                    self.shots_at[target["id"]] = 0
                    self.ignore_until[target["id"]] = tick + 600
                    self.event("ignore", id=target["id"])
                self.event("shot", id=target["id"], aim=[round(x, 3), round(y, 3)])
            else:
                self.g.aim(x, y, wait=False)
        elif f["n"] - self.last_shot_n >= SHOT_CYCLE:   # no target we can see: keep firing at the screen centre (enemies the memory scan misses are usually there)
            self.last_shot_n = f["n"]
            self.aim = (0.0, 0.0)
            self.g.press("B", SHOT_FRAMES, 0.0, 0.0, wait=False)
            self.shots += 1
            self.centres += 1
            if self.centres % 20 == 1:   # logged sparsely: one line per ~2 game seconds of centre fire
                self.event("centre", shots=self.centres)
        else:
            self.g.aim(0.0, 0.0, wait=False)

    def _game_over(self, f, tick):
        if tick - self.last_menu_tick < 60:
            return
        self.last_menu_tick = tick
        self.deaths += 1
        self.deaths_without_progress += 1
        self.event("death", deaths=self.deaths)
        t = TARGETS["game_over_continue"]
        self.g.aim(t.x, t.y, wait=False)
        self.g.press("A", PRESS_FRAMES, t.x, t.y, wait=False)

    def _menu(self, f, tick):
        if tick - self.last_menu_tick < 30:
            return
        self.last_menu_tick = tick
        screen = screen_of(f)
        words = (f.get("ui"), f.get("menu"))
        if screen is None and words != self.last_words:   # unknown page: log its raw state once per change so a signature can be added to menus.py
            self.last_words = words
            self.event("unknown_menu", ui=words[0], menu=words[1])
        self.event("menu", screen=screen)
        if screen == "chapter_done" and self.in_mission:   # the chapter is finished: later chapters start from this run's saves (--save-as, then --from NAME --chapter N)
            return self.stop("chapter_done")
        name = MENU_FLOW.get(screen)
        if name:   # a known page: aim at its button (repeated every 30 ticks until the page changes)
            t = TARGETS[name]
            self.g.aim(t.x, t.y, wait=False)
            self.g.press("A", PRESS_FRAMES, t.x, t.y, wait=False)
        else:      # results, loading and other pages: A
            self.g.press("A", PRESS_FRAMES, wait=False)

    # loop ------------------------------------------------------------
    def run(self, max_minutes):
        from .client import StepFailed
        deadline = time.monotonic() + max_minutes * 60
        reason, artifacts = None, {}
        try:
            while reason is None:
                if time.monotonic() > deadline:
                    reason = self.stop("time_limit")
                    break
                reason = self.step()
        except StepFailed as error:
            artifacts = error.artifacts
            reason = self.stop("crashed" if getattr(self.g, "exited", True) else "harness_error")
        report = {"reason": reason, "frames": self.frames, "kills": self.kills, "deaths": self.deaths, "shots": self.shots,
                  "furthest_screen": self.furthest, "god": self.god, "speed": self.speed}
        if artifacts.get("crash_report"):
            report["crash_report"] = artifacts["crash_report"]
        self.event("report", **report)
        return report

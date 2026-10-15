"""High-level steps built on Game primitives."""
import time

from .client import StepFailed
from .menus import TARGETS, screen_of

AIM_SETTLE_TICKS = 3   # the pointer must be sampled once or twice before the page sees the new hover
PRESS_FRAMES = 3       # KPAD samples A is held
SCALE = 1.0            # multiplies the settle (tuning knob)
# SDL scancodes of default_bindings() in common/input_bindings.cpp: reload R, grenade G, knife V, pause Escape.
KEYS = {"reload": 21, "grenade": 10, "knife": 25, "pause": 41}
ACTION_EVENTS = {"reload": "Reload", "grenade": "Grenade", "knife": "Knife"}


def skip_movies(g, *, seconds=30):
    deadline = time.monotonic() + seconds
    while screen_of(g.frame) != "title":
        if time.monotonic() > deadline:
            g.fail("opening movies did not reach the title screen", "title")
        g.press("A")
        n = g.frame.get("n", 0)
        g.wait(lambda f: screen_of(f) == "title" or f["n"] > n + 90, frames=300, seconds=10, awaited="title")


def select(g, target, *, retries=30):
    """Aim at a menu target and press A until the game shows the next page (state, not time)."""
    t = TARGETS[target]
    g.wait(lambda f: screen_of(f) == t.screen, frames=3000, seconds=60, awaited=t.screen)
    for _ in range(retries + 1):
        g.aim(t.x, t.y, settle_frames=max(1, int(AIM_SETTLE_TICKS * SCALE)))
        g.press("A", PRESS_FRAMES, t.x, t.y)
        t0 = g.frame["retraces"]
        try:   # the retry deadline is game time, so it scales with --speed; seconds is only a watchdog
            g.wait(lambda f: screen_of(f) == t.next or f["retraces"] >= t0 + t.retry_ticks, seconds=30, awaited=t.next)
        except StepFailed:
            pass
        if screen_of(g.frame) == t.next:
            return
        g.counters["press_retries"] += 1
    g.fail(f"{target} had no effect after {retries + 1} presses", t.next)


def wait_gameplay(g, *, seconds=60, stable_ticks=20):
    """Dismiss mission-intro and tutorial pages with A until the player is controllable."""
    deadline = time.monotonic() + seconds
    since = None
    while True:
        f = g.frame
        tutorial_up = f.get("tutorial_loaded") and f.get("tutorial") not in (0, 0x100000)   # a tutorial page can be up while the context reads gameplay
        if f.get("context") == "gameplay" and f["player"]["valid"] and not tutorial_up:
            since = f["retraces"] if since is None else since
            if f["retraces"] - since >= stable_ticks:
                return f
        else:
            since = None
            if screen_of(f) == "game_over":
                g.fail("the player died before gameplay was reached", "gameplay")
            if tutorial_up or screen_of(f) == "mission":
                g.press("A", PRESS_FRAMES, wait=False)
        if time.monotonic() > deadline:
            g.fail("gameplay not reached", "gameplay")
        g.advance(6)


def _toggle_pause(g, state, seconds):
    """Press the pause key until the game reaches `state` (2 = paused); the pause panel drops keys while it opens."""
    for _ in range(10):
        g.key(KEYS["pause"])
        t0 = g.frame["retraces"]
        g.wait(lambda f: (f["pause"] == 2) == (state == 2) or f["retraces"] >= t0 + 40, seconds=seconds, awaited=f"pause state {state}")
        if (g.frame["pause"] == 2) == (state == 2):
            return g.frame
    g.fail(f"pause state {state} not reached", f"pause state {state}")


def perform(g, action, *, seconds=10):
    if action == "fire":
        g.press("B")
        return g.frame
    if action == "pause":
        _toggle_pause(g, 2, seconds)
        return _toggle_pause(g, 1, seconds)
    name = ACTION_EVENTS[action]
    g.actions()   # drop stale events
    g.key(KEYS[action])
    found = []

    def accepted(_frame):
        found.extend(a for a in g.actions() if a["name"] == name and a["accepted"])
        return bool(found)
    g.wait(accepted, seconds=seconds, awaited=f"{name} accepted")
    return found[0]


def measure_fps(g, seconds):
    if g.frame.get("speed") != 1:
        g.fail("performance is measured at speed 1 only", "speed 1")
    samples = []
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        start, presents = time.monotonic(), g.frame["presents"]
        contexts = {g.frame["context"]}
        while time.monotonic() - start < 1.0:
            time.sleep(0.05)
            contexts.add(g.frame["context"])
        elapsed = time.monotonic() - start
        if contexts == {"gameplay"}:   # discard seconds with a context change
            samples.append((g.frame["presents"] - presents) / elapsed)
    if not samples:
        g.fail("no clean gameplay seconds to measure", "gameplay")
    return {"fps_mean": sum(samples) / len(samples), "fps_min": min(samples), "samples": len(samples)}

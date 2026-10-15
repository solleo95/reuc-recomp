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


def _tutorial_up(f):
    return bool(f.get("tutorial_loaded") and f.get("tutorial") not in (0, 0x100000))


def play_state(f):
    """What the game is doing right now: only 'active' is real gameplay (a tutorial page or the pause panel stops the game while the context still reads gameplay)."""
    if f.get("context") != "gameplay":
        return screen_of(f) or "menu"
    if _tutorial_up(f):
        return "tutorial"
    if f.get("pause") == 2:
        return "paused"
    if not f["player"]["valid"]:
        return "loading"
    return "active"


WINDOW = 0.05   # seconds per FPS sample


def _windows(events, begin, end):
    """Bin frame events (arrival time, frame) into WINDOW-second samples: presentation rate, longest gap between events, game state."""
    rows, previous_presents = [], None
    index = int((end - begin) / WINDOW)
    by_bin = [[] for _ in range(index)]
    for t, f in events:
        b = int((t - begin) / WINDOW)
        if 0 <= b < index:
            by_bin[b].append((t, f))
    last_t = None
    for b, items in enumerate(by_bin):
        if not items:
            continue
        gaps = [items[i][0] - items[i - 1][0] for i in range(1, len(items))]
        if last_t is not None:
            gaps.append(items[0][0] - last_t)
        last_t = items[-1][0]
        presents = items[-1][1]["presents"]
        fps = None if previous_presents is None else (presents - previous_presents) / WINDOW
        previous_presents = presents
        states = {play_state(f) for _, f in items}
        rows.append({"t": round(b * WINDOW, 2), "fps": None if fps is None else round(fps, 1), "gap_ms": round(max(gaps) * 1000, 1) if gaps else None,
                     "state": "+".join(sorted(states)), "n": len(items)})
    return [row for row in rows if row["fps"] is not None]


def measure_fps(g, seconds, *, max_seconds=None, csv_path=None):
    """FPS in 50 ms windows from the first call on. Tutorial/mission pages are dismissed with A; only windows that are active gameplay throughout count
    towards the statistics, and the loop runs until `seconds` of them were collected (or max_seconds, default 3x). All windows are returned."""
    if g.frame.get("speed") != 1:
        g.fail("performance is measured at speed 1 only", "speed 1")
    max_seconds = max_seconds or seconds * 3
    begin = time.perf_counter()
    mark = len(g.timing)
    next_tick, active_time = begin, 0.0
    while active_time < seconds and time.perf_counter() - begin < max_seconds:
        next_tick += WINDOW
        time.sleep(max(0.0, next_tick - time.perf_counter()))
        state = play_state(g.frame)
        if state in ("tutorial", "mission"):   # pages over the scene: dismiss them, they are not gameplay
            g.press("A", PRESS_FRAMES, wait=False)
        elif state == "active":
            active_time += WINDOW
    end = time.perf_counter()
    events = [(t, f) for t, f in list(g.timing)[mark:]]
    rows = _windows(events, begin, end)
    rows = [row for row in rows if row["t"] >= 0.5]   # the first half second follows the speed switch
    active = [row for row in rows if row["state"] == "active"]
    if len(active) < 20:
        g.fail("no active gameplay seconds to measure", "gameplay")
    active = active[:int(seconds / WINDOW)]
    fps = sorted(row["fps"] for row in active)
    gaps = sorted(row["gap_ms"] for row in active if row["gap_ms"] is not None)
    if csv_path:
        with open(csv_path, "w", encoding="utf-8") as out:
            out.write("t,fps,gap_ms,state\n")
            for row in rows:
                out.write(f"{row['t']},{row['fps']},{row['gap_ms']},{row['state']}\n")
    pick = lambda values, q: values[min(len(values) - 1, int(len(values) * q))]
    return {"fps_mean": sum(fps) / len(fps), "fps_min": fps[0], "fps_p5": pick(fps, 0.05), "fps_p50": pick(fps, 0.5), "gap_p99_ms": pick(gaps, 0.99), "gap_max_ms": gaps[-1],
            "samples": len(active), "non_active_windows": len(rows) - len(active), "series": rows}

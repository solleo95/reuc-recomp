"""Scenarios: each takes (Game, argparse.Namespace) and returns a result dict; failures raise StepFailed."""
from . import steps

ROUTE = ["title_start", "save_slot_1", "scenario_train", "chapter_1", "character_rebecca", "loadout_confirm", "start_yes"]


def route(chapter=1):
    """Menu route from the title to gameplay of `chapter` (2..4 need a save where they are unlocked, see --from)."""
    if chapter not in (1, 2, 3, 4):
        raise ValueError(f"chapter {chapter}: 1..4")
    return [f"chapter_{chapter}" if t == "chapter_1" else t for t in ROUTE]


def boot_to_title(g, args):
    with g.step("skip_movies"):
        steps.skip_movies(g)
    return {"capture": g.capture()}


def reach_gameplay(g, chapter=1):
    with g.step("skip_movies"):
        steps.skip_movies(g)
    for target in route(chapter):
        with g.step(target):
            steps.select(g, target)
    with g.step("wait_gameplay"):
        steps.wait_gameplay(g)


def train_gameplay(g, args):
    reach_gameplay(g, args.chapter)
    return {"capture": g.capture()}


def no_tutorials(g, args):
    """With --skip-tutorials no tutorial page may open: 20 s of game time from the mission start, then the player must be controllable.
    Without the flag this fails at the first tutorial page, which shows the check can see one."""
    with g.step("skip_movies"):
        steps.skip_movies(g)
    for target in route(args.chapter):
        with g.step(target):
            steps.select(g, target)
    with g.step("watch"):
        t0, controllable = g.frame["retraces"], 0
        while g.frame["retraces"] - t0 < 1200:
            f = g.frame
            if f.get("tutorial_loaded") and f.get("tutorial") not in (0, 0x100000):
                g.fail(f"a tutorial page opened (state {f['tutorial']:08X})", "no tutorial")
            if steps.screen_of(f) == "mission":
                g.press("A", steps.PRESS_FRAMES, wait=False)   # mission intro pages are not tutorials
            controllable += f.get("context") == "gameplay" and f["player"]["valid"]
            g.advance(6)
        if not controllable:
            g.fail("gameplay was never reached", "gameplay")
    return {"capture": g.capture(), "controllable_samples": controllable}


def controls(g, args):
    reach_gameplay(g, args.chapter)   # the player is attacked from the first second: act before the chapter is lost
    result = {}
    accepted = []
    for action in ("fire", "reload", "grenade", "knife", "pause"):
        with g.step(f"wait_before_{action}"):
            steps.wait_gameplay(g, stable_ticks=1)
        with g.step(action):
            for _ in range(3 if action == "fire" else 1):   # Reload is refused on a full magazine: spend a few shots first
                steps.perform(g, action)
                if action == "fire":
                    g.advance(20)
        accepted.append(action)
    return {**result, "accepted": accepted}


def perf(g, args):
    g.set_speed(8)   # menus and loading at 8x; the measurement itself runs at speed 1
    reach_gameplay(g, args.chapter)
    with g.step("normal_speed"):
        g.set_speed(1)
        g.wait(lambda f: f.get("speed") == 1 and f.get("context") == "gameplay", seconds=10, awaited="speed 1 in gameplay")
    result = {"capture": g.capture()}
    with g.step("measure_fps"):
        result.update(steps.measure_fps(g, args.measure or 15, csv_path=g.run_dir / "fps50.csv"))
    return result


def fight(g, args):
    """Skip to the heavy train fight and measure it. The bot plays at speed 8; at the slowdown point the game drops to speed 1 with an idle god-mode
    player and the FPS is measured for `--measure` seconds. The point is learned from the owner: press F5 when the fight is on screen (a global key
    state read, so the game window keeps its non-focus and gets no input) and the scene fingerprint (game second, actors, draws, classes, scene ids,
    capture) is saved to scripts/uctest/marks/fight.json. The wave of 30+ actors starts a clock; the saved point is the seconds since that start, so later runs
    slow down by themselves at the same place of the wave whatever speed the bot played at; `--mark`
    ignores the saved point and waits for F5 again (F5 always works and replaces the saved point). With UC_CPU_SAMPLE_FLAG=<file> set for the run,
    the game's CPU sampler starts at the slowdown (the scenario creates that file). Tutorials are skipped; fight_trace.csv lists actors, living
    enemies and draws per game second."""
    import collections
    import ctypes
    import json
    import os
    import time
    from pathlib import Path
    from .autoplay import Autoplay
    mark_path = Path(__file__).parent / "marks" / "fight.json"
    mark = None
    if not args.mark and not getattr(args, "slow_at", 0) and mark_path.exists():
        mark = json.loads(mark_path.read_text(encoding="utf-8"))
        if getattr(args, "wave_offset", None) is not None:   # --wave-offset: the slowdown point relative to the 30-actor wave start, instead of the learned F5 point
            mark["wave_offset"] = args.wave_offset
    key_state = ctypes.windll.user32.GetAsyncKeyState
    VK_F5 = 0x74
    pressed_before = bool(key_state(VK_F5) & 0x8000)

    def f5_pressed():   # a new press since the last look (the key being held when the scenario starts does not count)
        nonlocal pressed_before
        state = key_state(VK_F5)
        down = bool(state & 0x8000)
        fresh = (down and not pressed_before) or bool(state & 1)
        pressed_before = down
        return fresh

    g.set_speed(8)
    reach_gameplay(g, args.chapter)
    bot = Autoplay(g, g.run_dir / "autoplay.jsonl", speed=8, god=True)
    deadline = time.monotonic() + args.max_minutes * 60
    listed = lambda f: len(f.get("enemies") or [])
    alive = lambda f: sum(1 for e in f.get("enemies") or [] if e.get("hp", 1) > 0)   # dead zombies stay in the list until the wave ends
    WAVE_ACTORS = 30   # the train wave lists 32 actors (living and dead) at once
    trace_path, next_tick, wave_since = g.run_dir / "fight_trace.csv", 0, None
    trace_path.write_text("game_second,listed,alive,draws\n")
    if mark and not getattr(args, "slow_at", 0):
        print(f"fight: slowdown point from {mark_path.name}: {mark['wave_offset']} game seconds after the {WAVE_ACTORS}-actor wave starts (F5 replaces it)", flush=True)
    else:
        print("fight: no saved slowdown point; the bot plays at speed 8, press F5 when the fight is on screen (no focus needed)", flush=True)
    pressed = False
    with g.step("skip_to_fight"):
        while True:
            if f5_pressed():
                pressed = True
                break
            if time.monotonic() > deadline:
                g.fail("the slowdown point was not reached before the time limit", "fight")
            reason = bot.step()
            if reason:
                g.fail(f"autoplay stopped before the slowdown point: {reason}", "fight")
            tick = g.frame.get("retraces", 0)
            if tick >= next_tick:   # once per game second
                next_tick = tick + 60
                draws = len(g.draws())
                with open(trace_path, "a") as out:
                    out.write(f"{tick // 60},{listed(g.frame)},{alive(g.frame)},{draws}\n")
                wave_since = (wave_since if wave_since is not None else tick // 60) if listed(g.frame) >= WAVE_ACTORS else None
                if mark and wave_since is not None and tick // 60 - wave_since >= mark["wave_offset"]:
                    break
                if getattr(args, "slow_at", 0) and tick // 60 >= getattr(args, "slow_at", 0):   # --slow-at: an absolute game second instead of the learned point (e.g. where leeches appear)
                    break
    frame = g.frame
    classes = collections.Counter(hex(e["cls"]) for e in frame.get("enemies") or [] if e.get("cls"))
    second = frame.get("retraces", 0) // 60
    marked = {"how": "F5" if pressed else "saved point", "game_second": second, "wave_offset": None if wave_since is None else second - wave_since,
              "listed": listed(frame), "alive": alive(frame), "draws": len(g.draws()), "kills": bot.kills, "scene_mode": frame.get("scene_mode"), "player_scene": (frame.get("player") or {}).get("scene"),
              "classes": dict(classes)}
    print(f"fight: slowdown at game second {marked['game_second']} ({marked['how']}): {marked['listed']} actors, {marked['alive']} alive, {marked['draws']} draws", flush=True)
    if pressed and marked["wave_offset"] is None:
        print(f"fight: F5 came before the {WAVE_ACTORS}-actor wave, nothing saved", flush=True)
    elif pressed:
        mark_path.parent.mkdir(parents=True, exist_ok=True)
        mark_path.write_text(json.dumps(marked, indent=1) + "\n", encoding="utf-8")
        print(f"fight: saved {mark_path}", flush=True)
    with g.step("normal_speed"):
        g.set_speed(1)
        g.wait(lambda f: f.get("speed") == 1 and f.get("context") == "gameplay", seconds=10, awaited="speed 1 in gameplay")
    flag = os.environ.get("UC_CPU_SAMPLE_FLAG")
    if flag:
        Path(flag).write_text("go")
    result = {"marked": marked, "capture": g.capture()}
    window_from = g.frame.get("retraces", 0)
    with g.step("measure_fps"):
        result.update(steps.measure_fps(g, args.measure or 15, csv_path=g.run_dir / "fps50.csv"))
    result["demo_window"] = {"from": window_from, "to": g.frame.get("retraces", 0)}   # retrace range a demo replay measures (uctest demo / bench --demo)
    return result


def autoplay(g, args):
    """Play from the train chapter start as far as the bot gets; the report says where and why it stopped."""
    from .autoplay import Autoplay
    reach_gameplay(g, args.chapter)
    bot = Autoplay(g, g.run_dir / "autoplay.jsonl", speed=args.speed, god=args.god)
    with g.step("autoplay"):
        report = bot.run(args.max_minutes)
    if report["reason"] in ("crashed", "harness_error"):
        g.fail(f"autoplay stopped: {report['reason']}", capture=False)
    if report["reason"] == "chapter_done":   # the chapter-select page has only just appeared: the game must live on in the menu (the page builds its layouts after the signature shows)
        with g.step("menu_after_chapter"):
            g.advance(3000)
        report["menu_capture"] = g.capture()
    return {"autoplay": report}


def _window_of(pid):
    """Visible top-level window of the game process (window management only, no input, no activation)."""
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    found = []
    callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)(
        lambda hwnd, _: (found.append(hwnd) if _owner(user32, hwnd) == pid and user32.IsWindowVisible(hwnd) else None) or True)
    user32.EnumWindows(callback, 0)
    return found[0] if found else None


def _owner(user32, hwnd):
    import ctypes
    from ctypes import wintypes
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def resize(g, args):
    """The game keeps running while the window changes size every few milliseconds (drag/resize used to stall it)."""
    import ctypes
    import time
    reach_gameplay(g)
    hwnd = _window_of(g.process.pid)
    if not hwnd:
        g.fail("game window not found", "window")
    flags = 0x0002 | 0x0004 | 0x0010   # SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE

    def rates(seconds, storm):
        f0, t0, k = dict(g.frame), time.monotonic(), 0
        while time.monotonic() - t0 < seconds:
            if storm:
                k += 1
                ctypes.windll.user32.SetWindowPos(hwnd, 0, 0, 0, 900 + (k * 37) % 500, 560 + (k * 23) % 300, flags)
            time.sleep(0.01)
        f1, dt = g.frame, time.monotonic() - t0
        return {"frames_per_s": round((f1["n"] - f0["n"]) / dt, 1), "ticks_per_s": round((f1["retraces"] - f0["retraces"]) / dt, 1), "resizes": k}

    with g.step("idle"):
        idle = rates(3, False)
    with g.step("storm"):
        storm = rates(5, True)
    with g.step("recover"):
        after = rates(3, False)
    if storm["ticks_per_s"] < 0.8 * idle["ticks_per_s"] or storm["frames_per_s"] < 0.7 * idle["frames_per_s"]:
        g.fail(f"game stalled while the window was resized: idle {idle}, storm {storm}", "resize")
    if after["ticks_per_s"] < 0.9 * idle["ticks_per_s"]:
        g.fail(f"game did not recover after the resize: idle {idle}, after {after}", "resize")
    return {"idle": idle, "storm": storm, "after": after}


SCENARIOS = {"boot_to_title": boot_to_title, "train_gameplay": train_gameplay, "no_tutorials": no_tutorials, "controls": controls, "perf": perf, "fight": fight, "autoplay": autoplay, "resize": resize}

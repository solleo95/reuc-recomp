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
    """Skip to the heavy train fight: the bot plays at speed 8 until `--enemies` actors are in the scene, then the game runs at speed 1 with an idle
    player (god mode) and the FPS is measured. With UC_CPU_SAMPLE_FLAG=<file> set for the run, the game's CPU sampler starts when this scenario
    creates that file, so a profile covers only the fight."""
    import os
    import time
    from pathlib import Path
    from .autoplay import Autoplay
    g.set_speed(8)
    reach_gameplay(g, args.chapter)
    bot = Autoplay(g, g.run_dir / "autoplay.jsonl", speed=8, god=True)
    deadline = time.monotonic() + args.max_minutes * 60
    with g.step("skip_to_fight"):
        while len(g.frame.get("enemies") or []) < args.enemies:
            if time.monotonic() > deadline:
                g.fail(f"the scene never reached {args.enemies} actors", "fight")
            reason = bot.step()
            if reason:
                g.fail(f"autoplay stopped before the fight: {reason}", "fight")
    with g.step("normal_speed"):
        g.set_speed(1)
        g.wait(lambda f: f.get("speed") == 1 and f.get("context") == "gameplay", seconds=10, awaited="speed 1 in gameplay")
    flag = os.environ.get("UC_CPU_SAMPLE_FLAG")
    if flag:
        Path(flag).write_text("go")
    result = {"actors": len(g.frame.get("enemies") or []), "capture": g.capture()}
    with g.step("measure_fps"):
        result.update(steps.measure_fps(g, args.measure or 15, csv_path=g.run_dir / "fps50.csv"))
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

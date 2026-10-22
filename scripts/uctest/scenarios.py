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
    burst = int(os.environ.get("UC_CAPTURE_BURST", "0"))   # UC_CAPTURE_BURST=N: N captures one game tick apart after the measurement (ghosting and smear checks of the upscalers)
    if burst:
        result["burst"] = []
        for _ in range(burst):
            result["burst"].append(g.capture())
            g.advance(1)
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


def _same_picture(a, b):
    """Equal up to 0.05% of the pixels: a paused zombie's hand can still creep by a sub-pixel (some per-call bone update), invisible on screen."""
    from .overlay import read_png
    w, h, ch, rows_a = read_png(a)
    rows_b = read_png(b)[3]
    changed = sum(1 for y in range(h) if rows_a[y] != rows_b[y] for x in range(w) if rows_a[y][x * ch:x * ch + 3] != rows_b[y][x * ch:x * ch + 3])
    return changed <= w * h // 2000


def _scene(f):
    return {"view": f.get("view"), "enemies": [(e["id"], e["root"], e["hp"]) for e in f.get("enemies") or []], "life": (f.get("player") or {}).get("life")}


def _same_scene(a, b):
    import math
    if a["enemies"] != b["enemies"] or a["life"] != b["life"]:
        return False
    av, bv = a["view"], b["view"]
    if av is None or bv is None:
        return av is bv
    # Recomputing the paused camera can round its floats by a few millionths.
    return len(av) == len(bv) and all(math.isclose(x, y, rel_tol=0, abs_tol=1e-5) for x, y in zip(av, bv))


def _check_frozen(g, where):
    """While paused: the picture and the actors stand still although the game keeps looping (retraces advance)."""
    g.advance(20)
    first, scene, ticks = g.capture(), _scene(g.frame), g.frame["retraces"]
    g.advance(180)
    second = g.capture()
    if g.frame["retraces"] - ticks < 180:
        g.fail(f"{where}: the game stopped looping while paused", "pause")
    if not _same_scene(_scene(g.frame), scene):
        g.fail(f"{where}: actors or camera moved while paused: {scene} -> {_scene(g.frame)}", "pause")
    if not _same_picture(first, second):
        g.fail(f"{where}: the picture changed while paused", "pause")
    return [first, second]


def pause(g, args):
    """P pauses by stepping the game by zero: on the title screen and in gameplay the picture and actors freeze while the game keeps looping;
    the menu (Insert) opens over the paused game, P does nothing there, Escape closes it; P again resumes."""
    result = {}
    with g.step("skip_movies"):
        steps.skip_movies(g)
    with g.step("pause_title"):
        g.host_key(g.P)
        g.wait(lambda f: f.get("paused"), seconds=5, awaited="paused")
        result["title"] = _check_frozen(g, "title")
        g.host_key(g.P)
        g.wait(lambda f: not f.get("paused"), seconds=5, awaited="resumed")
    g.set_speed(8)
    for target in route(args.chapter):
        with g.step(target):
            steps.select(g, target)
    with g.step("wait_gameplay"):
        steps.wait_gameplay(g)
    g.set_speed(1)
    g.wait(lambda f: f.get("speed") == 1 and f.get("context") == "gameplay", seconds=10, awaited="speed 1 in gameplay")
    g.advance(120)
    with g.step("pause_gameplay"):
        g.host_key(g.P)
        g.wait(lambda f: f.get("paused"), seconds=5, awaited="paused")
        result["gameplay"] = _check_frozen(g, "gameplay")
        frozen = _scene(g.frame)
    with g.step("menu_over_pause"):
        g.host_key(g.INSERT)
        g.wait(lambda f: f.get("menu_open"), seconds=5, awaited="menu open")
        g.advance(10)
        result["menu"] = g.capture()
        g.host_key(g.P)
        g.advance(10)
        if not g.frame.get("paused"):
            g.fail("P toggled the pause while the menu was open", "menu")
        g.host_key(g.ESCAPE)
        g.wait(lambda f: not f.get("menu_open"), seconds=5, awaited="menu closed")
        if not _same_scene(_scene(g.frame), frozen):
            g.fail("the game moved while paused under the menu", "menu")
    with g.step("resume"):
        g.host_key(g.P)
        g.wait(lambda f: not f.get("paused"), seconds=5, awaited="resumed")
        g.advance(120)
        if _same_scene(_scene(g.frame), frozen):
            g.fail("the game did not move after resuming", "resume")
        result["resumed"] = g.capture()
    return result


def overlay(g, args):
    """Insert opens settings; every setting applies live, the game keeps running, and the run's config.json saves each value."""
    import json
    import time
    g.set_speed(8)
    reach_gameplay(g, args.chapter)
    g.set_speed(1)
    g.wait(lambda f: f.get("speed") == 1 and f.get("context") == "gameplay", seconds=10, awaited="speed 1 in gameplay")
    result = {"plain": g.capture(), "settings": []}
    with g.step("open_menu"):
        g.host_key(g.INSERT)
        g.wait(lambda f: f.get("menu_open"), seconds=5, awaited="menu open")
        g.advance(10)
        result["menu"] = g.capture()
    changes = [dict(hdr=True), dict(hdr_peak_nits=600), dict(hdr_paper_white_nits=300), dict(hdr_ui_nits=120),   # HDR stays on for the switches below (SDR fallback when Windows HDR is off)
               dict(render_scale=1), dict(render_scale=1.5),   # 1.5: neither the start value nor a 1440p monitor's 2x
               dict(bloom=False), dict(bloom=True), dict(bloom_strength=150), dict(bloom_strength=0), dict(bloom_strength=100), dict(prompt_style="sculpted"), dict(prompt_style="monochrome"),
               dict(fps_limit=60), dict(fps_limit=144), dict(fps_limit=0), dict(upscaler="fsr"), dict(upscale_mode="performance"), dict(upscale_mode="native"),
               dict(upscaler="dlss"), dict(sharpening=True), dict(sharpness=.75), dict(sharpening=False),
               dict(upscaler="off"), dict(upscaler="dlss"), dict(upscaler="off"), dict(upscaler="dlss"),
               dict(frame_generation=True), dict(frame_generation=False), dict(frame_generation=True),
               dict(upscaler="fsr"), dict(frame_generation=False), dict(frame_generation=True), dict(frame_generation=False),
               dict(upscaler="off"), dict(upscaler="fsr"), dict(upscaler="off"), dict(hdr=False)]
    for change in changes:
        name = ",".join(f"{k}={v}" for k, v in change.items())
        with g.step(f"set_{name}"):
            ticks, began = g.frame["retraces"], time.monotonic()
            g.set_options(**change)
            if "upscaler" in change:
                g.wait(lambda f: f.get("upscaler") == change["upscaler"], seconds=15, awaited=f"active upscaler {change['upscaler']}")
            if "frame_generation" in change:
                g.wait(lambda f: f.get("frame_generation") == change["frame_generation"], seconds=15, awaited=f"frame generation {change['frame_generation']}")
            g.advance(60)
            saved = json.loads(g.config_path.read_text(encoding="utf-8"))["graphics"]
            wrong = {k: saved.get(k) for k, v in change.items() if saved.get(k) != v}
            if wrong:
                g.fail(f"{name}: config.json has {wrong}", "settings")
            if not g.frame.get("menu_open") or g.frame.get("context") != "gameplay":
                g.fail(f"{name}: the menu closed or gameplay ended", "settings")
            result["settings"].append({"change": name, "seconds": round(time.monotonic() - began, 2), "ticks": g.frame["retraces"] - ticks, "capture": g.capture()})
    with g.step("scale_after_restarts"):   # the 1.5x render scale picked before the upscaler switches survives every renderer replacement
        renders = [line for line in (g.run_dir / "run.log").read_text(encoding="utf-8", errors="replace").splitlines() if line.startswith("Native render:")]
        if not renders or "multiplier 1.5x" not in renders[-1]:
            g.fail(f"render scale lost after the renderer restarts: {renders[-1] if renders else 'no render line'}", "settings")
    with g.step("hdr_mode"):   # each HDR switch picked a mode: HDR10, or SDR with the reason
        modes = [line for line in (g.run_dir / "run.log").read_text(encoding="utf-8", errors="replace").splitlines() if line.startswith("HDR output:")]
        result["hdr_modes"] = modes
        if not any(line != "HDR output: off" for line in modes) or modes[-1] != "HDR output: off":
            g.fail(f"HDR switches not reflected in the log: {modes}", "settings")
    with g.step("close_menu"):
        g.host_key(g.ESCAPE)
        g.wait(lambda f: not f.get("menu_open"), seconds=5, awaited="menu closed")
        result["closed"] = g.capture()
    return result


MODS_SMOKE_MODS = ["infinite_ammo", "god_mode", "pistol_autofire", "faster_handgun"]


MODS_TAB, FIRST_GEAR = (717, 83), (769, 330)   # window pixels in the harness's 1280x720 window: the gear of the second smoke mod (Бессмертие / god_mode: settings and free drawing)


def mods_smoke(g, args):
    """Mods in the real game (the example mods, copied next to the exe for this run): the held trigger fires but the magazine never drops (infinite ammo),
    the handgun record is automatic with the data patch's 30 rounds, the damage meter stays at zero (god mode), and the menu shows the Mods tab and the
    mods' tabs. Nothing here uses system input."""
    import struct
    reach_gameplay(g)
    g.advance(60)

    def peek(addr, size):
        return bytes.fromhex(g.request({"cmd": "peek", "addr": addr, "size": size})["hex"])

    result = {}
    with g.step("mods_loaded"):
        log = (g.run_dir / "run.log").read_text(encoding="utf-8", errors="replace")
        if "[mods] 4 enabled" not in log:
            g.fail("the example mods were not loaded: " + [line for line in log.splitlines() if "[mods]" in line or "[mod " in line][:4].__repr__(), "mods")
    with g.step("weapon_records"):
        record = peek(0x8046A86C, 168)
        mode, capacity = struct.unpack_from(">I", record, 0x10)[0], struct.unpack_from(">I", record, 0x14)[0]
        result["handgun_record"] = {"mode": mode, "capacity": capacity}
        if mode != 0 or capacity != 30:
            g.fail(f"handgun record is mode {mode}, capacity {capacity}; the mods set automatic fire and 30 rounds", "weapon record")
    with g.step("held_trigger"):
        before = g.state()["player"]["magazine"]
        g.press("B", 90)
        g.advance(30)
        after = g.state()["player"]["magazine"]
        result["magazine"] = {"before": before, "after": after}
        if after < before:
            g.fail(f"the magazine dropped from {before} to {after} with infinite ammo on", "infinite ammo")
    with g.step("god_mode"):
        g.advance(240)
        damage = g.state()["player"]["damage"]
        result["damage_meter"] = damage
        if damage != 0:
            g.fail(f"the damage meter is {damage} with god mode on", "god mode")
    with g.step("menu"):
        g.host_key(g.INSERT)
        g.wait(lambda f: f.get("menu_open"), seconds=5, awaited="menu open")
        g.advance(10)
        result["menu"] = g.capture()   # look at it: the built-in tabs and the Mods tab
        g.menu_click(*MODS_TAB)
        result["menu_mods"] = g.capture()   # the manager: one row per mod, gear + order arrows flush right
        g.menu_click(*FIRST_GEAR)
        result["menu_settings"] = g.capture()   # the second mod's settings window (a modal over the manager)
        g.host_key(g.ESCAPE)
        g.wait(lambda f: not f.get("menu_open"), seconds=5, awaited="menu closed")
    return result


def hook_probe(g, args):
    """Hooks in the real game: a mod's hook on GameWeaponUpdate is called with the weapon object, and clearing the 'allowed' flag (r4) from a hook changes
    what the game does. Reads debug/mods.log next to the exe; fire presses use the game's own pad input."""
    reach_gameplay(g)
    g.advance(240)
    log_path = g.exe.parent / "debug" / "mods.log"
    result = {}
    with g.step("hook_called"):
        text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
        lines = [line for line in text.splitlines() if "hook r3=" in line]
        result["hook_lines"] = lines[-3:]
        if not lines:
            g.fail("the hook was never called (no 'hook r3=' line in debug/mods.log)", "hook")
        r3 = int(lines[-1].split("r3=")[1].split()[0], 16)
        result["r3"] = hex(r3)
        if not 0x80000000 <= r3 < 0x81800000:
            g.fail(f"r3 {r3:#x} is not a guest address", "hook")
    return result


def hook_block(g, args):
    """A hook that clears r4 of GameWeaponUpdate: the held trigger must not fire. (Without the mod the same press takes the magazine from 15 to 14, checked
    on 2026-10-22; r4 is the weapon's 'allowed' flag.)"""
    reach_gameplay(g)
    g.advance(60)
    result = {}
    with g.step("held_trigger"):
        before = g.state()["player"]["magazine"]
        g.press("B", 90)
        g.advance(30)
        after = g.state()["player"]["magazine"]
        result["magazine"] = {"before": before, "after": after}
        if after != before:
            g.fail(f"the magazine went from {before} to {after} although the hook cleared r4", "hook r4")
    return result


def weapon_damage(g, args):
    """A data-only mod sets the handgun damage to 100: the autoplay bot's hits must take 100 (body), 30 or 50 (other parts) off zombies; unmodded the same
    hits take 10, 3 and 5 (measured 2026-10-22). Enemy hp is sampled from the frame events while the bot plays."""
    import collections, threading, time
    from .autoplay import Autoplay
    reach_gameplay(g)
    last, drops, stop = {}, collections.Counter(), threading.Event()

    def sample():
        while not stop.is_set():
            for e in (g.frame or {}).get("enemies") or []:
                hp = e.get("hp")
                if hp is None:
                    continue
                if e["id"] in last and hp < last[e["id"]] and last[e["id"]] - hp < 1000:
                    drops[round(last[e["id"]] - hp)] += 1
                last[e["id"]] = hp
            time.sleep(0.003)
    thread = threading.Thread(target=sample, daemon=True)
    thread.start()
    with g.step("hits"):
        Autoplay(g, g.run_dir / "autoplay.jsonl", speed=args.speed).run(0.5)
    stop.set()
    thread.join()
    result = {"hp_drops": dict(drops.most_common(8))}
    with g.step("damage_applied"):
        if drops[100] < 5 or drops[10] > drops[100]:
            g.fail(f"hits did not take 100 off (drops: {result['hp_drops']})", "weapon damage")
    return result


def fire_rate(g, args):
    """weapon.fire_rate = 2 with automatic fire: a held trigger (150 frames) uses 11 to 15 rounds. Measured 2026-10-22 with the interval written by hand: the
    plain automatic handgun fires 7 times in that time, semi-automatic once, an interval of 0.5 (rate 2) 14 times."""
    reach_gameplay(g)
    g.advance(60)
    result = {}
    with g.step("held_trigger"):
        before = g.state()["player"]["magazine"]
        g.press("B", 150)
        g.advance(30)
        after = g.state()["player"]["magazine"]
        result["shots"] = before - after
        if not 11 <= before - after <= 15:
            g.fail(f"{before - after} rounds used with fire_rate 2 (expected 11 to 15; plain automatic is about 7)", "fire rate")
    return result


def hook_replace(g, args):
    """hook.replace on GameWeaponUpdate with an empty function: the original weapon update does not run, so a held trigger does not fire (plain: 15 -> 14), and
    hook.post still runs after each skipped call (it logs its call count)."""
    reach_gameplay(g)
    g.advance(120)
    result = {}
    with g.step("replaced"):
        before = g.state()["player"]["magazine"]
        g.press("B", 90)
        g.advance(30)
        after = g.state()["player"]["magazine"]
        result["magazine"] = {"before": before, "after": after}
        if after != before:
            g.fail(f"the magazine went from {before} to {after} although the weapon update was replaced by nothing", "hook replace")
    with g.step("post_ran"):
        log_path = g.exe.parent / "debug" / "mods.log"
        text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
        if "post calls=" not in text:
            g.fail("the post hook never ran after the replaced function", "hook post")
    return result


SCENARIOS = {"boot_to_title": boot_to_title, "train_gameplay": train_gameplay, "no_tutorials": no_tutorials, "controls": controls, "perf": perf, "fight": fight, "autoplay": autoplay, "resize": resize,
             "pause": pause, "overlay": overlay, "mods_smoke": mods_smoke, "hook_probe": hook_probe, "hook_block": hook_block, "weapon_damage": weapon_damage, "fire_rate": fire_rate, "hook_replace": hook_replace}
SCENARIO_MODS = {"mods_smoke": MODS_SMOKE_MODS, "hook_probe": ["hook_probe"], "hook_block": ["hook_block"], "weapon_damage": ["damage_big"], "fire_rate": ["fire_rate_2"], "hook_replace": ["hook_replace"]}   # example mods a scenario needs installed before the game starts (Game(mods=[...]))

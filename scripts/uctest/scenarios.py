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


INSPECTOR_GEAR = (769, 234)   # gear of the only mod in the manager (found by looking at a capture)
INSPECTOR_MISSION, INSPECTOR_ENEMIES = (640, 373), (640, 431)   # headers in the mod's window, as a capture showed them
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


def state_dump(g, args):
    """Spike (block 3 of the mod API): dumps the statistics area that the Gecko codes point at (0x805C5C00..0x805C6000, stars at 0x804C77C4) at several
    moments of the first chapter, with the trigger held in between, and lists the words that changed."""
    import struct
    reach_gameplay(g, args.chapter)

    def peek(addr, size):
        return bytes.fromhex(g.request({"cmd": "peek", "addr": addr, "size": size})["hex"])

    regions = [(0x805C5A00, 0x800), (0x804C7700, 0x200)]
    from .autoplay import Autoplay
    bot = Autoplay(g, g.run_dir / "autoplay.jsonl", speed=args.speed, god=True)
    snaps, next_at, start = [], 0, g.frame["retraces"]
    while len(snaps) < 8 and bot.step() is None:
        tick = g.frame["retraces"] - start
        if tick >= next_at:
            next_at = tick + 900   # every 15 s of game time
            st = g.state()
            snaps.append((f"t{tick // 60}s kills={bot.kills}", st, [peek(a, n) for a, n in regions]))
    out = []
    for (la, _, ra), (lb, _, rb) in zip(snaps, snaps[1:]):
        out.append(f"--- {la} -> {lb}")
        for (addr, n), a, b in zip(regions, ra, rb):
            for i in range(0, n, 4):
                if a[i:i + 4] != b[i:i + 4]:
                    out.append(f"{addr + i:08X}: {a[i:i + 4].hex()} -> {b[i:i + 4].hex()}  (f32 {struct.unpack('>f', b[i:i + 4])[0]:.4g}, u32 {struct.unpack('>I', b[i:i + 4])[0]})")
    (g.run_dir / "state_dump.txt").write_text("\n".join(out), encoding="utf-8")
    return {"dump": str(g.run_dir / "state_dump.txt"), "state": snaps[-1][1]}


def player_dump(g, args):
    """Spike (block 7 of the mod API): raw dumps of the player object, the weapon table and the ammo table in the first chapter -> player_dump.json."""
    import json
    reach_gameplay(g, args.chapter)
    g.advance(120)

    def peek(addr, size):
        return bytes.fromhex(g.request({"cmd": "peek", "addr": addr, "size": size})["hex"])

    state = g.state()["player"]
    p1 = int.from_bytes(peek(0x806C3AD8, 4), "big")
    p2 = int.from_bytes(peek(0x806C3ADC, 4), "big")
    dump = {"p1": p1, "p2": p2, "player1": peek(p1, 0x900).hex(), "player2": peek(p2, 0x900).hex() if p2 else "",
            "weapons": peek(0x8046A86C, 168 * 20).hex(), "ammo": peek(0x8056F300, 0x200).hex(), "state": state}
    (g.run_dir / "player_dump.json").write_text(json.dumps(dump), encoding="utf-8")
    return {"dump": str(g.run_dir / "player_dump.json")}


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


def camera_api(g, args):
    """camera.position / forward from the view matrix, in the real game: the eye is about 1.5 m (15 units) above the floor, forward is a unit vector and the nearest
    living zombie is in front of the camera (dot product of the direction to it with forward > 0.3)."""
    reach_gameplay(g)
    log_path = g.exe.parent / "debug" / "mods.log"
    result = {}
    with g.step("camera_logged"):
        lines = []
        for _ in range(40):
            g.advance(60)
            text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
            lines = [line for line in text.splitlines() if "camera eye_y=" in line]
            if lines:
                break
        if not lines:
            g.fail("the camera probe never saw a living zombie (no 'camera eye_y=' line in debug/mods.log)", "camera")
        fields = dict(part.split("=") for part in lines[-1].split("] ")[-1].split()[1:])
        eye_y, ahead = float(fields["eye_y"]), float(fields["ahead"])
        forward = [float(v) for v in fields["forward"].split(",")]
        result.update(eye_y=eye_y, ahead=ahead, forward=forward)
        if not 8 <= eye_y <= 25:
            g.fail(f"eye height {eye_y} units is not about 15", "camera")
        if abs(sum(v * v for v in forward) - 1) > 0.02:
            g.fail(f"forward {forward} is not a unit vector", "camera")
        if ahead < 0.3:
            g.fail(f"the nearest zombie is not in front of the camera (dot {ahead})", "camera")
    return result


def player_api(g, args):
    """player.weapon / health / reserve in the real game: the handgun (id 0 or 19), full health, an infinite reserve (nil); then a script sets health to 0.5
    and the damage meter reads 25 (of 50)."""
    reach_gameplay(g)
    log_path = g.exe.parent / "debug" / "mods.log"
    result = {}
    with g.step("player_logged"):
        line = ""
        for _ in range(40):
            g.advance(60)
            text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
            lines = [l for l in text.splitlines() if "player weapon=" in l]
            if lines:
                line = lines[-1]
                break
        if not line:
            g.fail("no 'player weapon=' line in debug/mods.log", "player api")
        fields = dict(part.split("=") for part in line.split("] ")[-1].split()[1:])
        result["logged"] = fields
        if fields["weapon"] not in ("0", "19") or fields["health"] != "1.00" or fields["reserve"] != "nil":
            g.fail(f"unexpected player values {fields}", "player api")
    with g.step("health_set"):
        g.advance(300)
        damage = g.state()["player"]["damage"]
        result["damage_meter"] = damage
        if abs(damage - 25) > 0.01:
            g.fail(f"health 0.5 should give a damage meter of 25, it is {damage}", "player api")
    return result


def input_force(g, args):
    """input.force_held = B: the harness holds nothing, yet the game sees the trigger (the mod logs fire_held) and the handgun fires (15 -> 14 or lower)."""
    reach_gameplay(g)
    log_path = g.exe.parent / "debug" / "mods.log"
    result = {}
    with g.step("forced_trigger"):
        before = g.state()["player"]["magazine"]
        g.advance(240)
        after = g.state()["player"]["magazine"]
        result["magazine"] = {"before": before, "after": after}
        text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
        if "input fire_held=true" not in text:
            g.fail("the mod never saw fire_held although it forced the trigger", "input force")
        if after >= before:
            g.fail(f"the magazine went from {before} to {after}: the forced trigger did not fire", "input force")
    return result


def input_block(g, args):
    """input.block_held = B: the harness holds the trigger for 90 frames, the game does not see it (no shot, fire_held never true)."""
    reach_gameplay(g)
    g.advance(60)
    log_path = g.exe.parent / "debug" / "mods.log"
    result = {}
    with g.step("blocked_trigger"):
        before = g.state()["player"]["magazine"]
        g.press("B", 90)
        g.advance(30)
        after = g.state()["player"]["magazine"]
        result["magazine"] = {"before": before, "after": after}
        text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
        if after != before or "input fire_held=SEEN" in text:
            g.fail(f"the blocked trigger reached the game (magazine {before} -> {after})", "input block")
    return result


def enemy_api(g, args):
    """uc.enemy(id).hp = 0 on the nearest living zombie: the mod reads alive = false at once and the game keeps the zombie at hp 0 (dead, as after a kill); the
    capture shows the scene a moment later."""
    reach_gameplay(g)
    log_path = g.exe.parent / "debug" / "mods.log"
    result = {}
    with g.step("enemy_killed"):
        line = ""
        for _ in range(40):
            g.advance(30)
            text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
            lines = [l for l in text.splitlines() if "enemy killed id=" in l]
            if lines:
                line = lines[-1]
                break
        if not line:
            g.fail("the mod never found a living zombie to kill", "enemy api")
        if "alive_after=false" not in line:
            g.fail(f"the mod still sees the zombie alive: {line}", "enemy api")
        enemy_id = int(line.split("id=")[1].split()[0])
    with g.step("stays_dead"):
        g.advance(60)
        hp = [e["hp"] for e in g.state().get("enemies") or [] if e["id"] == enemy_id]
        result["hp_after"] = hp
        if hp and hp[0] > 0:
            g.fail(f"the zombie came back to hp {hp[0]}", "enemy api")
        text = log_path.read_text(encoding="utf-8", errors="replace")
        result["spawn_events"] = text.count("event spawned id=")
        if f"event killed id={enemy_id}" not in text:
            g.fail(f"no on_enemy_killed event for zombie {enemy_id}", "enemy api")
        result["capture"] = g.capture()
    return result


def game_api(g, args):
    """uc.game.time / paused and the hit counters in the real game: the clock rises while the mission runs, the hit counter rises while the trigger is
    held at the zombies, and the game's pause menu (the + button) shows as paused = true."""
    reach_gameplay(g)
    log_path = g.exe.parent / "debug" / "mods.log"

    def rows():
        text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
        return [dict(kv.split("=") for kv in l.split("game ", 1)[1].split()) for l in text.splitlines() if "game time=" in l]

    result = {}
    with g.step("clock_and_hits"):
        g.advance(120)
        first = rows()
        if not first:
            g.fail("the probe mod logged nothing", "game api")
        from .autoplay import Autoplay
        bot = Autoplay(g, g.run_dir / "autoplay.jsonl", speed=args.speed, god=True)
        start = g.frame["retraces"]
        while g.frame["retraces"] - start < 1500 and bot.step() is None:   # 25 s of game time, shooting the zombies
            pass
        g.advance(30)
        last = rows()[-1]
        result["first"], result["last"] = first[0], last
        if int(last["time"]) <= int(first[0]["time"]):
            g.fail(f"the mission clock did not move: {first[0]['time']} -> {last['time']}", "game api")
        if int(last["hits"]) <= int(first[0]["hits"]):
            g.fail(f"the hit counter did not rise: {first[0]['hits']} -> {last['hits']}", "game api")
    with g.step("pause"):
        steps._toggle_pause(g, 2, 10)   # the game's pause key (Escape)
        g.advance(60)
        result["pause_word"] = g.frame.get("pause")
        seen = any(r["paused"] == "true" for r in rows()[-6:])
        result["paused_in_log"] = seen
        result["capture"] = g.capture()
        if not seen:
            g.fail(f"uc.game.paused never read true (pause word {result['pause_word']})", "game api")
    return result


def timers_api(g, args):
    """setTimeout / setInterval in the real game: an interval of one game second ticks about once per second of play, the timeout fires once, and the
    game's pause menu stops them (they count game steps)."""
    reach_gameplay(g)
    log_path = g.exe.parent / "debug" / "mods.log"

    def lines():
        text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
        return [l for l in text.splitlines() if "timer " in l]

    result = {}
    with g.step("ticks"):
        start = g.frame["retraces"]
        before = len(lines())
        g.advance(600)   # ten seconds at 60 retraces a second
        seconds = (g.frame["retraces"] - start) / 60
        got = lines()[before:]
        ticks = [l for l in got if "tick=" in l]
        result["seconds"], result["ticks"], result["once"] = seconds, len(ticks), sum("timer once" in l for l in got)
        if not (0.6 * seconds <= len(ticks) <= 1.2 * seconds):
            g.fail(f"{len(ticks)} ticks in {seconds:.1f} s of play", "timers api")
        if "every" not in ticks[-1]:
            g.fail("the extra argument did not reach the callback", "timers api")
    with g.step("pause"):
        steps._toggle_pause(g, 2, 10)
        g.advance(60)
        count = len(lines())
        g.advance(240)   # four seconds in the pause menu
        result["ticks_in_pause"] = len(lines()) - count
        result["capture"] = g.capture()
        if result["ticks_in_pause"]:
            g.fail(f"{result['ticks_in_pause']} timer lines while the game was paused", "timers api")
    return result


def _title_audio(g, ticks):
    """Seconds of the title screen; returns the 'audio stats' of the exit log: (frames, nonzero samples). The title screen plays only 00_Title.brstm, so
    the nonzero count is that stream's (the original music gives about a million in 900 ticks)."""
    import re
    with g.step("skip_movies"):
        steps.skip_movies(g)
    with g.step("listen"):
        g.advance(ticks)
    log = g.finish()
    found = re.search(r"audio stats: native frames (\d+) nonzero samples (\d+)", log)
    if not found:
        g.fail("the exit log has no audio statistics", "audio stats", capture=False)
    return int(found.group(1)), int(found.group(2))


def audio_replace(g, args):
    """The audio_tone mod replaces the title music with a 1 s, 22.05 kHz mono WAV looped over its first half (converted by the port to the original's
    32 kHz stereo PCM16 BRSTM). 15 s of game time at the title keep sounding well past the loop length, so the converted stream plays and loops."""
    frames, nonzero = _title_audio(g, 900)
    if nonzero < 500_000:
        g.fail(f"the converted stream stopped early or never played: {nonzero} nonzero samples in {frames} audio frames", "audio replace", capture=False)
    return {"audio_frames": frames, "nonzero_samples": nonzero}


def audio_replace_silent(g, args):
    """Control for audio_replace: the audio_silent mod replaces the title music with silence, so (almost) nothing may sound (proves the title music is the
    converted file and not the original; audio_replace alone could not tell, the original gives a similar count)."""
    frames, nonzero = _title_audio(g, 900)
    if nonzero > 100_000:   # the original music gives about 1 000 000; what is left is the opening movie's sound before it is skipped, a few thousand to tens of thousands
        g.fail(f"the original title music still plays: {nonzero} nonzero samples", "audio replace", capture=False)
    return {"audio_frames": frames, "nonzero_samples": nonzero}


def _find_in_guest_memory(g, needle, start=0x80400000, size=2 << 20):
    """The first guest address in [start, start + size) where `needle` bytes occur, or None (by peek, 256 KB at a time: a scan of all memory takes minutes)."""
    chunk = 262144
    tail = b""
    for offset in range(0, size, chunk):
        data = tail + bytes.fromhex(g.request({"cmd": "peek", "addr": start + offset, "size": chunk}, seconds=60)["hex"])
        at = data.find(needle)
        if at >= 0:
            return start + offset - len(tail) + at
        tail = data[-(len(needle) - 1):]
    return None


def _record_500_bytes(g):
    """The first line of record 500 of the game's own mes_us.txtres as UTF-8 bytes (what the unpatched file holds), or None when the file cannot be found."""
    import importlib.util
    import os
    from pathlib import Path
    candidates = [os.environ.get("UC_GAME_DIR"), r"S:\Games\Umbrella Chronicles\game", str(g.exe.parent / "game")]
    for folder in candidates:
        path = Path(folder or "") / "DATA" / "files" / "Data" / "message" / "mes_us.txtres"
        if folder and path.exists():
            spec = importlib.util.spec_from_file_location("dump_text", Path(__file__).resolve().parents[2] / "tools" / "dump_text" / "dump_text.py")
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            for record_id, lines in module.read_texts(path.read_bytes()):
                if record_id == 500 and lines:
                    return lines[0].encode("utf-8")[:24]
    return None


def text_replace(g, args):
    """The text_marker mod sets records 500, 550 and 553 (the handgun's name in the HUD, which shows UCTEXTMARKER in the capture) for every language. The game reads the whole text file into MEM2
    (around 0x91240000): the scenario finds the original text of record 500 in that window (which proves the window holds the file) and requires the marker
    in it exactly when the mod is on (text_replace_plain runs without the mod)."""
    needle = _record_500_bytes(g)
    if needle is None:
        return {"skipped": "the game's mes_us.txtres was not found (set UC_GAME_DIR to the game folder that holds DATA)"}
    reach_gameplay(g)
    g.advance(60)
    window = {"start": 0x91000000, "size": 8 << 20}
    capture = g.capture()   # before the scan: the player is not defended while the game runs on
    with g.step("scan_memory"):
        original = _find_in_guest_memory(g, needle, **window)
        marker = _find_in_guest_memory(g, b"UCTEXTMARKER" + bytes([0]), **window)
    if original is None:
        g.fail("the game's text file was not found in memory (is mes_us the Russian one? the scan window may be wrong)", "text replace")
    marked = "text_marker" in (getattr(g, "mods", None) or [])
    if marked and marker is None:
        g.fail("the marker text is not in the game's memory: the patched text file was not used", "text replace")
    if not marked and marker is not None:
        g.fail(f"the marker text is in memory at {marker:08X} without the mod", "text replace")
    return {"original_at": f"{original:08X}", "marker_at": None if marker is None else f"{marker:08X}", "capture": capture}


ARC_FILE, ARC_MEMBER, ARC_MARKER = "layout/game_us.arc", "lyt_data/timg/2SHT_B.tpl", b"UCARCMARKER" + bytes([0])


def _game_files_dir():
    """<game folder>/DATA/files/Data of the owner's install (UC_GAME_DIR, the usual place, or next to the exe), or None."""
    import os
    from pathlib import Path
    for folder in (os.environ.get("UC_GAME_DIR"), r"S:\Games\Umbrella Chronicles\game"):
        path = Path(folder or "") / "DATA" / "files" / "Data"
        if folder and (path / ARC_FILE).exists():
            return path
    return None


def _arc_tool():
    import importlib.util
    from pathlib import Path
    spec = importlib.util.spec_from_file_location("arc_tool", Path(__file__).resolve().parents[2] / "tools" / "arc_tool" / "arc_tool.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def prepare_arc_member():
    """Writes the test-only mod arc_member (git-ignored) from the install: the HUD layout archive's member 2SHT_B.tpl as the game has it, followed by a marker
    text. Returns a reason to skip the run when the install cannot be found."""
    import json
    from pathlib import Path
    root = _game_files_dir()
    if root is None:
        return "the game's layout/game_us.arc was not found (set UC_GAME_DIR to the game folder that holds DATA)"
    original = dict(_arc_tool().parse((root / ARC_FILE).read_bytes()))[ARC_MEMBER]
    mod = Path(__file__).resolve().parent / "mods" / "arc_member"
    member = mod / "files" / "Data" / "layout" / "game_us.arc!" / Path(*ARC_MEMBER.split("/"))
    member.parent.mkdir(parents=True, exist_ok=True)
    member.write_bytes(original + ARC_MARKER + bytes(64))
    (mod / "mod.json").write_text(json.dumps({"id": "arc-member", "name": "Arc member", "version": "1.0", "api": "1.1"}))
    return None


def _find_in_game_memory(g, needle):
    """First guest address of `needle` in MEM1 (0x80400000..0x81700000) or MEM2, scanned in 8 MB windows; None when absent."""
    windows = [(0x90000000 + i * (8 << 20), 8 << 20) for i in range(7)] + [(0x80400000, 8 << 20), (0x80C00000, 8 << 20), (0x81400000, 3 << 20)]
    for start, size in windows:
        found = _find_in_guest_memory(g, needle, start=start, size=size)
        if found is not None:
            return found
    return None


def arc_member(g, args):
    """The arc_member mod replaces one member of the HUD layout archive (layout/game_us.arc) with the original bytes plus a marker text, through the member
    path files/Data/layout/game_us.arc!/lyt_data/timg/2SHT_B.tpl. The game loads the archive whole into memory: the original member's first bytes prove the
    archive was found, and the marker must follow it exactly when the mod is on (arc_member_plain runs without the mod)."""
    root = _game_files_dir()
    if root is None:
        return {"skipped": "the game's layout/game_us.arc was not found (set UC_GAME_DIR to the game folder that holds DATA)"}
    original = dict(_arc_tool().parse((root / ARC_FILE).read_bytes()))[ARC_MEMBER]
    reach_gameplay(g)
    g.advance(60)
    capture = g.capture()
    with g.step("scan_memory"):
        member_at = _find_in_game_memory(g, original[:48])
        if member_at is None:
            g.fail("the layout archive's member was not found in the game's memory (the archive may live elsewhere)", "arc member")
        after = bytes.fromhex(g.request({"cmd": "peek", "addr": member_at + len(original), "size": len(ARC_MARKER)}, seconds=60)["hex"])   # what follows the member
    marked = "arc_member" in (getattr(g, "mods", None) or [])
    if marked and after != ARC_MARKER:
        g.fail(f"the marker does not follow the member at {member_at:08X}: the rebuilt archive was not used", "arc member")
    if not marked and after == ARC_MARKER:
        g.fail(f"the marker follows the member at {member_at:08X} without the mod", "arc member")
    return {"member_at": f"{member_at:08X}", "after_member": after.hex(), "capture": capture}


def camera_capture(g, args):
    """Reaches the train chapter, lets the opening fade finish and captures twice (3 s apart); for looking at what a camera change does."""
    reach_gameplay(g)
    g.advance(300)
    first = g.capture()
    g.advance(180)
    return {"capture": first, "later": g.capture()}


# The game's own weapon table in the train chapter (id: damage, capacity, reload) and the hit points of its first four zombies, read in the game 2026-10-23.
ORIGINAL_WEAPONS = {0: (10, 15, 1.0), 1: (9, 6, 2.3), 2: (4, 50, 0.5), 3: (5, 30, 1.0), 4: (180, 4, 2.7), 5: (100, 1, 1.0), 6: (12, 0, 10.0), 7: (120, 4, 2.3), 8: (100, 6, 1.7), 9: (7, 6, 2.0),
                    10: (7, 50, 0.4), 11: (80, 6, 2.0), 12: (60, 8, 1.2), 13: (120, 3, 2.3), 14: (8, 8, 1.7), 15: (8, 40, 0.7), 16: (180, 1, 2.3), 17: (300, 3, 2.0), 18: (350, 1, 2.5), 19: (10, 15, 1.0)}
ORIGINAL_ZOMBIES = {160: 20, 161: 10, 162: 30, 163: 20}


def randomizer_live(g, args):
    """The randomizer example mod (seed 1, strength 100, start weapon on) in the real train chapter: the weapon table differs from the game's own in several records
    and stays within a third to triple of it, the first zombies' hit points differ within the same range, and player 1 holds a gun from the table (not id 6).
    Run without the mod (randomizer_plain) the same reads must give the game's own values: that is the baseline."""
    import struct
    reach_gameplay(g)
    g.advance(300)   # the start weapon is given 3 game seconds into the mission
    marked = "randomizer" in (getattr(g, "mods", None) or [])
    raw = bytes.fromhex(g.request({"cmd": "peek", "addr": 0x8046A86C, "size": 168 * 20})["hex"])
    table = {}
    for i in range(20):
        r = raw[i * 168:(i + 1) * 168]
        table[i] = (struct.unpack(">f", r[0x24:0x28])[0], struct.unpack(">I", r[0x14:0x18])[0], struct.unpack(">f", r[0x38:0x3C])[0])
    zombies = {e["id"]: e["hp"] for e in g.frame.get("enemies", []) if e["id"] in ORIGINAL_ZOMBIES}
    player = g.frame["player"]["address"]
    weapon = struct.unpack(">I", bytes.fromhex(g.request({"cmd": "peek", "addr": player + 0x4C8, "size": 4})["hex"]))[0]
    result = {"changed_records": sum(1 for i, o in ORIGINAL_WEAPONS.items() if abs(table[i][0] - o[0]) > 1e-3 * max(1, o[0]) or table[i][1] != o[1]), "zombies": zombies, "weapon": weapon}
    with g.step("weapons"):
        for i, (damage, capacity, reload) in ORIGINAL_WEAPONS.items():
            now = table[i]
            if not marked:
                if abs(now[0] - damage) > 1e-3 * max(1, damage) or now[1] != capacity or abs(now[2] - reload) > 1e-3:
                    g.fail(f"weapon {i} is {now} in the unmodified game, expected {(damage, capacity, reload)}", "randomizer")
                continue
            if i == 6:   # not a gun (no magazine): left alone
                if now[1] != 0 or abs(now[0] - damage) > 1e-3 * max(1, damage) or abs(now[2] - reload) > 1e-3:
                    g.fail(f"record 6 changed: {now}", "randomizer")
                continue
            if not (damage / 3 - 0.01 <= now[0] <= damage * 3 + 0.01 and 1 <= now[1] <= capacity * 3 + 1 and reload / 3 - 0.01 <= now[2] <= reload * 3 + 0.01):
                g.fail(f"weapon {i} left the range: {now} from {(damage, capacity, reload)}", "randomizer")
        if marked and result["changed_records"] < 3:
            g.fail(f"only {result['changed_records']} weapon records changed", "randomizer")
    with g.step("zombies"):
        if len(zombies) != 4:
            g.fail(f"expected the four zombies of the first scene, saw {zombies}", "randomizer")
        for zombie_id, original in ORIGINAL_ZOMBIES.items():
            hp = zombies[zombie_id]
            if not marked and abs(hp - original) > 1e-3:
                g.fail(f"zombie {zombie_id} has {hp} hit points in the unmodified game, expected {original}", "randomizer")
            if marked and not original / 3 - 0.01 <= hp <= original * 3 + 0.01:
                g.fail(f"zombie {zombie_id} has {hp} hit points, outside a third to triple of {original}", "randomizer")
        if marked and all(abs(zombies[i] - o) < 1e-3 for i, o in ORIGINAL_ZOMBIES.items()):
            g.fail("no zombie changed", "randomizer")
    with g.step("start_weapon"):
        if not marked and weapon not in (0, 19):
            g.fail(f"the unmodified game starts with weapon {weapon}", "randomizer")
        if marked and (weapon == 0 or ORIGINAL_WEAPONS.get(weapon, (0, 0, 0))[1] == 0):
            g.fail(f"player 1 holds weapon {weapon}: expected a random gun other than the handgun (seed 1)", "randomizer")
    result["capture"] = g.capture()
    g.advance(180)
    result["later"] = g.capture()
    result["magazine"] = g.frame["player"]["magazine"]
    return result


def inspector_live(g, args):
    """The inspector example mod with log_every = 1 in the real train chapter: 25 s of play must give a snapshot line of valid JSON about every second in
    debug/mods.log, the mission clock must rise, the camera must have a position, player 1 must be present with a health between 0 and 1, and the enemy list
    must be a list. Then the Insert menu is opened on the mod's window and captured (look at it)."""
    import json
    reach_gameplay(g)
    log_path = g.exe.parent / "debug" / "mods.log"

    def snapshots():
        text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
        return [line.split("] ", 1)[1] for line in text.splitlines() if line.startswith("[inspector] {")]

    result = {}
    with g.step("window_in_mission"):   # the mod's window while the player is alive: the enemy and mission sections opened (bottom one first: opening one moves those below it)
        g.advance(60)
        g.host_key(g.INSERT)
        g.wait(lambda f: f.get("menu_open"), seconds=5, awaited="menu open")
        g.advance(10)
        g.menu_click(*MODS_TAB)
        g.menu_click(*INSPECTOR_GEAR)
        g.advance(10)
        g.menu_click(*INSPECTOR_ENEMIES)
        g.menu_click(*INSPECTOR_MISSION)
        g.advance(10)
        result["window_in_mission"] = g.capture()
        g.host_key(g.ESCAPE)
        g.wait(lambda f: not f.get("menu_open"), seconds=5, awaited="menu closed")
    before = len(snapshots())
    g.advance(1500)
    lines = snapshots()[before:]
    result["snapshots"] = len(lines)
    with g.step("snapshots"):
        if len(lines) < 10:
            g.fail(f"{len(lines)} snapshot lines in 25 s of play (log_every 1)", "inspector")
        rows = [json.loads(line) for line in lines]
        for row in rows:
            if not isinstance(row["enemies"], list):
                g.fail("enemies is not a list", "inspector")
        live = [row for row in rows if row["players"][0]]   # the first scene of the chapter ends after about 20 s of play: later lines say "not in a mission"
        if len(live) < 5:
            g.fail(f"player 1 is in only {len(live)} of {len(rows)} snapshots", "inspector")
        first, last = live[0], live[-1]
        position = last["camera"] and last["camera"]["position"]
        if not (isinstance(position, list) and len(position) == 3 and all(isinstance(v, (int, float)) for v in position)):
            g.fail(f"no camera position in a snapshot with a player: {last['camera']}", "inspector")
        player = last["players"][0]
        if not 0 <= player["health"] <= 1 or not isinstance(player["magazine"], int):
            g.fail(f"player 1 in a snapshot: {player}", "inspector")
        if not last["game"]["time"] > first["game"]["time"]:
            g.fail(f"the mission clock did not rise: {first['game']['time']} -> {last['game']['time']}", "inspector")
        if not any(row["enemies"] and all(isinstance(e["hp"], (int, float)) and isinstance(e["x"], (int, float)) for e in row["enemies"]) for row in live):
            g.fail("no zombie in any snapshot of the chapter's first scene", "inspector")
        result.update(first=first["game"], last=last["game"], most_enemies=max(len(r["enemies"]) for r in live), camera=position, player=player)
    with g.step("window"):
        g.host_key(g.INSERT)
        g.wait(lambda f: f.get("menu_open"), seconds=5, awaited="menu open")
        g.advance(10)
        g.menu_click(*MODS_TAB)
        result["menu_mods"] = g.capture()
        g.menu_click(*INSPECTOR_GEAR)
        g.advance(10)
        result["menu_inspector"] = g.capture()
        g.host_key(g.ESCAPE)
        g.wait(lambda f: not f.get("menu_open"), seconds=5, awaited="menu closed")
    return result


def _sfd_tool():
    import importlib.util
    import sys
    from pathlib import Path
    path = Path(__file__).resolve().parents[2] / "tools" / "sfd_tool" / "sfd_tool.py"
    spec = importlib.util.spec_from_file_location("sfd_tool", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules["sfd_tool"] = module
    spec.loader.exec_module(module)
    return module


def prepare_movie_replace():
    """Writes the test-only mod movie_replace (git-ignored): the opening movie replaced by a 9 s clip made with tools/sfd_tool (3 s of red, 3 s green,
    3 s blue with noise on top (so that the picture needs a real bitrate) and a 440 Hz tone). Returns a reason to skip when ffmpeg is missing."""
    import json
    import shutil
    import subprocess
    import tempfile
    from pathlib import Path
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        return "ffmpeg is not installed (tools/sfd_tool needs it)"
    mod = Path(__file__).resolve().parent / "mods" / "movie_replace"
    target = mod / "files" / "Data" / "movie" / "MV_000_0.sfd"
    target.parent.mkdir(parents=True, exist_ok=True)
    (mod / "mod.json").write_text(json.dumps({"id": "movie-replace", "name": "Movie replace", "version": "1.0", "api": "1.1"}))
    with tempfile.TemporaryDirectory() as folder:
        clip = Path(folder) / "clip.mp4"
        inputs = []
        for color in ("red", "lime", "blue"):   # "green" is (0, 128, 0) in ffmpeg: lime is the full green
            inputs += ["-f", "lavfi", "-i", f"color=c={color}:s=320x240:r=30000/1001:d=3"]
        inputs += ["-f", "lavfi", "-i", "sine=frequency=440:duration=9"]
        result = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", *inputs, "-filter_complex", "[0][1][2]concat=n=3:v=1:a=0,noise=alls=30:allf=t[v]", "-map", "[v]", "-map", "3:a",
                                 "-c:v", "mpeg4", "-c:a", "aac", str(clip)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError("ffmpeg could not make the test clip: " + result.stderr[-300:])
        _sfd_tool().make(clip, target, None, 2500, "pad", True)
    return None


def movie_replace(g, args):
    """The movie_replace mod puts a 9 s red, green, blue clip (made by tools/sfd_tool) in place of the opening movie: the capture of the movie's centre must be
    red, later green, later blue, and after the movie the game must go on (a last capture of none of the three: the title screen), with the clip's sound
    in the audio statistics."""
    seen = []

    def colour(path):
        from .overlay import read_png
        width, height, channels, rows = read_png(path)
        block = [rows[y][x * channels:x * channels + 3] for y in range(height // 2 - 16, height // 2 + 16) for x in range(width // 2 - 16, width // 2 + 16)]
        r, g_, b = (sum(p[i] for p in block) // len(block) for i in range(3))   # the picture is noisy (a real bitrate for the decoder): average a block
        for name, own, others in (("red", r, (g_, b)), ("green", g_, (r, b)), ("blue", b, (r, g_))):
            if own >= 130 and max(others) <= 100:
                return name
        return "other"

    for _ in range(26):
        g.advance(60)
        seen.append(colour(g.capture()))
    order = [seen.index(c) if c in seen else None for c in ("red", "green", "blue")]
    if None in order or not order[0] < order[1] < order[2]:
        g.fail(f"the movie's red, green and blue were not seen in that order: {seen}", "movie replace")
    if seen[-1] != "other":
        g.fail(f"the game did not go on after the movie: {seen}", "movie replace")
    import re
    log = g.finish()
    match = re.search(r"audio stats: native frames (\d+) nonzero samples (\d+)", log)
    if not match or int(match.group(2)) < 100000:
        g.fail(f"the clip's sound did not play: {match.group(0) if match else 'no audio statistics'}", "movie replace")
    return {"colours": seen, "audio": match.group(0)}


def movie_probe(g, args):
    """Spike M7: boots without skipping the opening movie and takes a capture every 3 s of game time, so a replacement of Data/movie/MV_000_0.sfd (the
    test-only mod movie_probe, git-ignored, written by hand) can be looked at."""
    captures = []
    for _ in range(8):
        g.advance(180)
        captures.append(g.capture())
    return {"captures": captures, "retraces": g.frame["retraces"]}


def guest_api(g, args):
    """uc.guest.call and uc.game.give_weapon / select_weapon in the real game: the weapon-record accessor returns the table address, and a submachine gun
    given and selected from a mod becomes the player's weapon; a held trigger then uses its magazine (automatic fire)."""
    reach_gameplay(g)
    log_path = g.exe.parent / "debug" / "mods.log"

    def lines(prefix):
        text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
        return [l.split("guest ", 1)[1] for l in text.splitlines() if "guest " + prefix in l]

    result = {}
    with g.step("call_and_switch"):
        for _ in range(20):
            g.advance(30)
            if lines("record="):
                break
        done = lines("record=")
        if not done:
            g.fail("the probe mod never ran its calls (see mods.log)", "guest api")
        result["calls"] = done[0]
        if "record=8046A9BC" not in done[0] or "current=true" not in done[0]:   # 0x8046A86C + 2 * 168
            g.fail(f"unexpected call results: {done[0]}", "guest api")
    with g.step("weapon_in_hand"):
        g.advance(60)
        last = lines("weapon=")[-1]
        result["after"] = last
        if "weapon=2" not in last:
            g.fail(f"the submachine gun is not the player's weapon: {last}", "guest api")
        before = int(last.split("magazine=")[1])
        g.press("B", 60)
        g.advance(30)
        after = int(lines("weapon=")[-1].split("magazine=")[1])
        result["magazine"] = [before, after]
        if after >= before:
            g.fail(f"the held trigger did not use the magazine: {before} -> {after}", "guest api")
        result["capture"] = g.capture()
    return result


SCENARIOS = {"boot_to_title": boot_to_title, "train_gameplay": train_gameplay, "no_tutorials": no_tutorials, "controls": controls, "perf": perf, "fight": fight, "autoplay": autoplay, "resize": resize,
             "pause": pause, "overlay": overlay, "mods_smoke": mods_smoke, "hook_probe": hook_probe, "hook_block": hook_block, "weapon_damage": weapon_damage, "fire_rate": fire_rate, "hook_replace": hook_replace, "camera_api": camera_api, "player_api": player_api, "input_force": input_force, "input_block": input_block, "enemy_api": enemy_api, "state_dump": state_dump, "player_dump": player_dump, "game_api": game_api, "timers_api": timers_api, "arc_grow": train_gameplay, "arc_member": arc_member, "movie_probe": movie_probe, "movie_replace": movie_replace, "inspector_live": inspector_live, "randomizer_live": randomizer_live, "randomizer_plain": randomizer_live, "camera_capture": camera_capture, "arc_member_plain": arc_member, "guest_api": guest_api, "text_replace": text_replace, "text_replace_plain": text_replace, "audio_replace": audio_replace, "audio_replace_silent": audio_replace_silent}
SCENARIO_MODS = {"mods_smoke": MODS_SMOKE_MODS, "hook_probe": ["hook_probe"], "hook_block": ["hook_block"], "weapon_damage": ["damage_big"], "fire_rate": ["fire_rate_2"], "hook_replace": ["hook_replace"], "camera_api": ["camera_probe"], "player_api": ["player_probe"], "input_force": ["input_force"], "input_block": ["input_block"], "enemy_api": ["enemy_kill"], "game_api": ["game_probe"], "timers_api": ["timer_probe"], "arc_grow": ["arc_grow"], "arc_member": ["arc_member"], "movie_probe": ["movie_probe"], "movie_replace": ["movie_replace"], "inspector_live": ["inspector"], "randomizer_live": ["randomizer"], "guest_api": ["guest_probe"], "text_replace": ["text_marker"], "audio_replace": ["audio_tone"], "audio_replace_silent": ["audio_silent"]}   # example mods a scenario needs installed before the game starts (Game(mods=[...]))
SCENARIO_PREPARE = {"arc_member": prepare_arc_member, "movie_replace": prepare_movie_replace}   # run before the game starts: writes what the scenario needs; a returned text means 'skip'
SCENARIO_MOD_SETTINGS = {"inspector_live": {"inspector": {"log_every": 1}}, "randomizer_live": {"randomizer": {"seed": 1, "strength": 100, "start_weapon": True}}}   # stored settings of the mods a scenario installs

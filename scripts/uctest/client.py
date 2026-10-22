"""Drives uc.exe --test-server: waits on game state, sends input, fails with artifacts."""
import collections
import contextlib
import json
import os
import re
import shutil
import subprocess
import threading
import time
from pathlib import Path

from .transport import PipeTransport

ROOT = Path(__file__).resolve().parents[2]
LIVE_SAVES = Path(r"S:\Games\Umbrella Chronicles\saves")
# Tests start from a pinned snapshot, not the live saves: the owner plays on those, and a mid-chapter checkpoint or another progress state changes the
# intro, the loading and the menu flow (and with it every recorded demo). UC_SAVE_SOURCE overrides; without a snapshot the live saves are copied.
PINNED_SAVES = Path(__file__).resolve().parents[2] / "build" / "autotest" / "saves-pinned"
SAVE_SOURCE = Path(os.environ["UC_SAVE_SOURCE"]) if os.environ.get("UC_SAVE_SOURCE") else (PINNED_SAVES if PINNED_SAVES.is_dir() else LIVE_SAVES)
CHECKPOINTS = ROOT / "build" / "autotest" / "checkpoints"
GUEST_CPU = 26


def guest_cpu_args(extra):
    # Keep every harness entry point on the same measured CPU, including explicit game arguments.
    for index, arg in enumerate(extra):
        if arg.startswith("--guest-cpu=") or (arg == "--guest-cpu" and
                (index+1 == len(extra) or extra[index+1] != str(GUEST_CPU))):
            raise ValueError(f"test harness requires --guest-cpu {GUEST_CPU}")
    return [] if "--guest-cpu" in extra else ["--guest-cpu", str(GUEST_CPU)]



def resolve_saves(spec):
    """None = the real saves (always copied, never used in place); a checkpoint name (see --save-as); or a saves directory such as build/autotest/<run>/saves."""
    if spec is None:
        return None
    if Path(spec).is_dir():
        return Path(spec)
    if (CHECKPOINTS / spec).is_dir():
        return CHECKPOINTS / spec
    raise ValueError(f"no checkpoint or saves directory '{spec}' (checkpoints live in {CHECKPOINTS})")


class StepFailed(Exception):
    """A step did not reach its condition. artifacts: awaited, frame, capture, log_tail, crash_report."""

    def __init__(self, message, artifacts):
        super().__init__(message)
        self.artifacts = artifacts


class Game:
    BUTTONS = {"A": 0x0800, "B": 0x0400, "minus": 0x0010, "plus": 0x1000}

    def __init__(self, *, exe="dist/uc.exe", run_dir=None, speed=1, skip_intro=True, seconds=600, transport_factory=None, launch=True, god=False, skip_tutorials=False, saves=None, game_args=(), mods=False):
        self.exe = (ROOT / exe).resolve()
        self.run_dir = Path(run_dir) if run_dir else ROOT / "build" / "autotest" / str(time.time_ns())
        self.name = re.sub(r"[^A-Za-z0-9_-]", "_", self.run_dir.name)[-32:] or "run"
        self.speed, self.skip_intro, self.seconds, self.launch, self.god = speed, skip_intro, seconds, launch, god
        self.skip_tutorials, self.mods = skip_tutorials, mods
        self.transport_factory, self.saves, self.game_args = transport_factory, saves, list(game_args)
        self.process = None
        self.transport = None
        self.frame = {}
        self.history = collections.deque(maxlen=600)
        self.timing = collections.deque(maxlen=40000)   # (arrival time, frame event) of every frame event, for fine-grained rate measurements
        self.exited = False
        self.steps = []
        self.counters = collections.Counter()   # e.g. press_retries: menu presses the game dropped
        self._cond = threading.Condition()
        self._replies = {}
        self._ignored = set()   # ids of fire-and-forget commands: their replies are dropped
        self._actions = []
        self._next_id = 0
        self._failing = False
        self._started = time.time()
        self._reader = None

    @contextlib.contextmanager
    def step(self, name):
        start = time.monotonic()
        try:
            yield
        finally:
            self.steps.append({"name": name, "seconds": round(time.monotonic() - start, 2)})

    # lifecycle -------------------------------------------------------
    def __enter__(self):
        self.run_dir.mkdir(parents=True, exist_ok=True)
        try:
            if self.launch:
                self._launch()
            factory = self.transport_factory or (lambda: PipeTransport(self.name, alive=lambda: self.process.poll() is None))
            self.transport = factory()
        except Exception as error:
            self._cleanup()
            self.fail(f"game did not start: {error}", capture=False)
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        return self

    def __exit__(self, *exc):
        self._cleanup()

    def _launch(self):
        pin = guest_cpu_args(self.game_args)
        saves = self.run_dir / "saves"
        if not saves.exists():
            shutil.copytree(self.saves or SAVE_SOURCE, saves)
        config = self.run_dir / "config.json"   # the in-game menu saves settings: a run writes its own copy, never the dist config
        if not config.exists() and (self.exe.parent / "config.json").exists():
            shutil.copyfile(self.exe.parent / "config.json", config)
        self._installed_mods = []
        if isinstance(self.mods, (list, tuple)):
            try:
                self._install_mods(config)
            except Exception:
                self._cleanup()   # __exit__ does not run when __enter__ fails: take back the folders already copied
                raise
        args = [str(self.exe), "--test-server", self.name, "--save-root", str(saves), "--capture-dir", str(self.run_dir),
                "--seconds", str(self.seconds), "--speed", str(self.speed), "--width", "1280", "--height", "720"]   # 720p window, not maximized
        if "--config" not in self.game_args:   # no dist config: the game starts from defaults and saves its first change here
            args.extend(["--config", str(config)])
        if "--window-mode" not in self.game_args:
            args.extend(["--window-mode", "windowed"])   # saved borderless settings must not cover the owner's desktop during tests
        if self.skip_intro:
            args.append("--skip-intro")
        if "--fps-limit" not in self.game_args:
            args.extend(["--fps-limit", "0"])   # as a player runs it (launcher default: unlimited): the game steps at its own pace, not the original 30 Hz
        args.extend(pin)
        args.extend(self.game_args)
        if not os.environ.get("UC_TEST_SOUND"):   # UC_TEST_SOUND=1 plays the audio
            args.append("--mute")   # tests run silent: the owner is usually at the machine
        if not os.environ.get("UC_TEST_LAYERS"):   # UC_TEST_LAYERS=1 keeps the implicit Vulkan layers (RTSS, OBS, ReShade...) on, as a player runs the game
            args.append("--no-vulkan-layers")   # OBS/RTSS/ReShade layers cost ~3 FPS and add hitches: tests measure the game, not the overlays
        if not self.mods:
            args.append("--no-mods")   # a mods folder next to the exe must not change what a test measures; the mod scenarios pass mods=True
        if self.god:
            args.append("--god")
        if self.skip_tutorials:
            args.append("--skip-tutorials")
        log = open(self.run_dir / "run.log", "wb")
        env = dict(os.environ)
        if "--frame-generation" in self.game_args:
            env["UC_FG_UNFOCUSED"] = "1"   # Streamline's frame generation only runs for a focused window: tell it the game's window is, without focusing it
        self.process = subprocess.Popen(args, stdout=log, stderr=log, cwd=self.exe.parent, env=env, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        log.close()

    def _cleanup(self):
        try:
            if self.transport and not self.exited:
                try:
                    self.transport.send_line(json.dumps({"id": self._take_id(), "cmd": "quit"}))
                except OSError:
                    pass
            if self.process:
                try:
                    self.process.wait(3)
                except subprocess.TimeoutExpired:
                    self.process.terminate()
                    try:
                        self.process.wait(5)
                    except subprocess.TimeoutExpired:
                        self.process.kill()
                        self.process.wait()
            if self.transport:
                self.transport.close()
        except Exception:
            pass   # cleanup must never mask the step failure
        for folder in getattr(self, "_installed_mods", []):   # the example mods this run copied next to the exe
            shutil.rmtree(folder, ignore_errors=True)
        for cache in getattr(self, "_installed_caches", []):   # what the game converted from them (audio), named by mod id
            shutil.rmtree(cache, ignore_errors=True)
        self._installed_mods = []
        self._installed_caches = []

    def finish(self, seconds=30):
        """Quits the game the normal way and returns its whole run.log (the exit statistics, e.g. 'audio stats: ...', are only in the log after the quit)."""
        if self.transport and not self.exited:
            try:
                self.transport.send_line(json.dumps({"id": self._take_id(), "cmd": "quit"}))
            except OSError:
                pass
        try:
            self.process.wait(seconds)
        except subprocess.TimeoutExpired:
            self.fail(f"the game did not quit within {seconds} s", "quit", capture=False)
        return (self.run_dir / "run.log").read_text(errors="replace")

    def _install_mods(self, config):
        """mods=[names]: copies those folders of mods-examples/ into <exe folder>/mods and enables them in this run's config copy; removed at cleanup."""
        target = self.exe.parent / "mods"
        ids = []
        for name in self.mods:
            source, folder = ROOT / "mods-examples" / name, target / name
            if not source.exists(): source = ROOT / "scripts" / "uctest" / "mods" / name   # test-only mods live beside the harness
            marker = folder / ".uctest-installed"
            if folder.exists():
                if not marker.exists():   # the owner's own mod with this folder name: never delete it
                    raise RuntimeError(f"{folder} already exists and was not installed by the harness; rename or remove it for the test run")
                shutil.rmtree(folder, ignore_errors=True)   # left behind by an earlier harness run
            shutil.copytree(source, folder)
            marker.write_text("installed by uctest; removed at cleanup", encoding="utf-8")
            self._installed_mods.append(folder)
            ids.append(json.loads((source / "mod.json").read_text(encoding="utf-8"))["id"])
            self._installed_caches = getattr(self, "_installed_caches", []) + [target / ".cache" / "_audio" / ids[-1], target / ".cache" / "_text"]
        data = json.loads(config.read_text(encoding="utf-8")) if config.exists() else {}
        data["mods"] = {"enabled": ids}
        config.write_text(json.dumps(data, indent=1), encoding="utf-8")

    # events ----------------------------------------------------------
    def _read_loop(self):
        while True:
            line = self.transport.read_line()
            if line is None:
                break
            try:
                message = json.loads(line)
            except ValueError:
                continue
            with self._cond:
                kind = message.get("type") if isinstance(message, dict) else None
                if kind == "frame":
                    self.frame = message
                    self.history.append(message)
                    self.timing.append((time.perf_counter(), message))
                elif kind == "action":
                    self._actions.append(message)
                elif kind == "exit":
                    self.exited = True
                elif isinstance(message, dict) and "id" in message:
                    if message["id"] in self._ignored:
                        self._ignored.discard(message["id"])
                    else:
                        self._replies[message["id"]] = message
                self._cond.notify_all()
        with self._cond:
            self.exited = True
            self._cond.notify_all()

    def _take_id(self):
        self._next_id += 1
        return self._next_id

    # primitives ------------------------------------------------------
    def request(self, cmd, seconds=30):
        with self._cond:
            request_id = self._take_id()
        self.transport.send_line(json.dumps({**cmd, "id": request_id}))
        deadline = time.monotonic() + seconds
        with self._cond:
            while request_id not in self._replies:
                if self.exited:
                    break
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                self._cond.wait(min(remaining, 0.1))
            reply = self._replies.pop(request_id, None)
        if reply is None:
            self.fail(f"no reply to {cmd.get('cmd')}" + (" (game exited)" if self.exited else ""), capture=False)
        if "error" in reply:
            self.fail(f"{cmd.get('cmd')} rejected: {reply['error']}", capture=False)
        return reply

    def wait(self, predicate, *, frames=None, seconds=60, awaited=""):
        start_frame = self.frame.get("n", 0)
        deadline = time.monotonic() + seconds
        with self._cond:
            while True:
                current = self.frame
                if current and predicate(current):
                    return current
                if self.exited:
                    break
                if frames is not None and current.get("n", 0) - start_frame > frames:
                    break
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                self._cond.wait(min(remaining, 0.05))
        self.fail(f"timeout waiting for {awaited or 'condition'}" + (" (game exited)" if self.exited else ""), awaited)

    def advance(self, ticks):
        """Let `ticks` of game time pass (1 tick = 1 VI retrace = 1/60 s of game time at any speed).
        Menu animations and loading follow game time; KPAD frames run faster than 60/s when speed > 1."""
        t0 = self.frame.get("retraces", 0)
        return self.wait(lambda f: f["retraces"] >= t0 + ticks, seconds=10 + ticks / 20, awaited="game time advance")

    def _input(self, cmd, wait):
        """Send an input command; wait=False returns at once (the game may not sample input during loads and cutscenes)."""
        if wait:
            return self.request(cmd)["consumed_frame"]
        with self._cond:
            request_id = self._take_id()
            self._ignored.add(request_id)
        self.transport.send_line(json.dumps({**cmd, "id": request_id}))
        return None

    def press(self, button, frames=2, x=0.0, y=0.0, wait=True):
        return self._input({"cmd": "input", "hold_frames": frames, "kpad": {"x": x, "y": y, "buttons": self.BUTTONS[button]}}, wait)

    def aim(self, x, y, settle_frames=0, wait=True):
        consumed = self._input({"cmd": "input", "kpad": {"x": x, "y": y, "buttons": 0}}, wait)
        if settle_frames:
            self.advance(settle_frames)
        return consumed

    def key(self, code, frames=2, wait=True):
        return self._input({"cmd": "input", "hold_frames": frames, "key": {"code": code, "down": True}}, wait)

    def capture(self):
        return self.request({"cmd": "capture"})["path"]

    def draws(self):
        """Screen-space bounding boxes of the last presented frame per draw callsite (aim coordinates)."""
        return self.request({"cmd": "draws"})["draws"]

    def state(self):
        return self.request({"cmd": "state"})["state"]

    def set_speed(self, factor):
        self.request({"cmd": "speed", "factor": factor})

    INSERT, P, ESCAPE = 73, 19, 41   # SDL scancodes of the port's host keys

    def host_key(self, code):
        """A key of the port itself (Insert: settings menu, P: pause), handled in the main loop's next iteration."""
        self.request({"cmd": "host_key", "code": code})

    def menu_click(self, x, y):
        """A left click on the in-game menu at window pixel (x, y) (the menu must be open; the harness window is 1280x720)."""
        start = (self.frame or {}).get("presents", 0)
        self.request({"cmd": "menu_click", "x": x, "y": y})
        # The game delivers the pointer move, a settle move, the press and the release one per presented frame: wait for them (game frames are no measure at speed 8).
        self.wait(lambda f: f.get("presents", 0) >= start + 8, seconds=15, awaited="menu click delivered")

    def set_options(self, **graphics):
        """Change graphics settings live, as the in-game menu does (config.json "graphics" keys); saved to the run's config copy."""
        self.request({"cmd": "options", "graphics": graphics})

    @property
    def config_path(self):
        return self.run_dir / "config.json"

    def actions(self):
        with self._cond:
            taken, self._actions = self._actions, []
        return taken

    # failure ---------------------------------------------------------
    def _log_tail(self):
        try:
            return (self.run_dir / "run.log").read_text(errors="replace").splitlines()[-80:]
        except OSError:
            return []

    def _crash_report(self):
        try:
            reports = [p for p in (self.exe.parent / "debug").glob("crash-*.txt") if p.stat().st_mtime >= self._started]
            return str(max(reports, key=lambda p: p.stat().st_mtime)) if reports else None
        except OSError:
            return None

    def fail(self, message, awaited="", capture=True):
        artifacts = {"awaited": awaited, "frame": dict(self.frame), "log_tail": self._log_tail(), "crash_report": self._crash_report(), "capture": None}
        if capture and not self.exited and not self._failing and self.transport:
            self._failing = True
            try:
                artifacts["capture"] = self.request({"cmd": "capture"}, seconds=5)["path"]
            except Exception:
                pass
            finally:
                self._failing = False
        try:
            (self.run_dir / "failure.json").write_text(json.dumps({"message": message, **artifacts}, indent=1))
        except OSError:
            pass
        raise StepFailed(message, artifacts)

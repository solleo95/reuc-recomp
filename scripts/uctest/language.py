"""Language acceptance harness for the real game: what the guest actually opens for a launcher language.

The expected pack is the verified native contract (docs/superpowers/plans/2026-11-12-game-language-task-1-results.md), not the runtime's own
diagnostic: Spanish resource index 6 opens Data/message/mes_sp.txtres, Data/sysres/font0..2/utf8_S.brfnt and
Data/layout/{dialog,game}_sp.arc; English index 2 opens the US pack; the tested pack-less launcher language (ru) keeps that English.
Netplay, deterministic, --det-out and demos share one guest state and must open English whatever the launcher says. Other launcher codes are
not silently mapped here (game_text rejects them); the runtime's own unit fixtures cover availability for those packs. Proof is the guest's
`[dvd] open` lines from `--log-dvd`; the runtime's `[language]` startup line is only a cross-check.

  cd scripts
  python -m uctest.language                       ordinary es, en, ru, deterministic, det-out, demo, netplay pair
  python -m uctest.language --case es             one case

Each case is an existing uctest.client.Game run: its own save copy and config copy, 1280x720 windowed, unlimited FPS, no system input.
"""
import argparse
import contextlib
import json
import re
import socket
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path

from . import scenarios, steps
from .client import ROOT, Game, StepFailed
from .netem import DelayRelay
from .netplay import coop_menu, rollback_log, word

# launcher/localization.cpp supported[]: a stored launcher.language must be one of these or "system".
LAUNCHER_LANGUAGES = ("en", "ru", "de", "fr", "es", "it", "pt-BR", "pl", "uk", "tr", "ja", "ko", "zh-Hans")
DEMO_GAME_HZ = 120   # deterministic demo rate: demo.DET_GAME_HZ (checked by the tests)
LOG_ARGS = ["--log-dvd"]   # opens and reads, not only the startup diagnostic


@dataclass(frozen=True)
class GameText:
    """One verified native pack: guest resource index, message suffix, font letter, layout suffix."""
    launcher: str
    index: int
    message: str
    font: str
    layout: str


ENGLISH = GameText("en", 2, "us", "E", "us")
SPANISH = GameText("es", 6, "sp", "S", "sp")   # the disc calls Spanish "sp"
# Only the launcher languages this acceptance actually proves; anything else must be covered by the runtime's own fixtures, not guessed here.
PACKS = {"en": ENGLISH, "es": SPANISH, "ru": ENGLISH}   # ru is the tested pack-less launcher language
FORCED_ENGLISH_MODES = frozenset(("netplay", "deterministic", "det-out", "demo-record", "demo-play"))
MODES = ("ordinary", *FORCED_ENGLISH_MODES)
DEFAULT_CASES = ("es", "en", "ru", "deterministic", "det-out", "demo", "netplay")
CASES = DEFAULT_CASES


def game_text(launcher):
    """The pack this harness can prove for a launcher code; a code it has no case for is rejected, never silently called English."""
    if launcher == "system":
        raise ValueError("launcher language 'system' must be resolved before the game's language can be predicted")
    if launcher not in PACKS:
        raise ValueError(f"launcher language {launcher!r} has no acceptance case here")
    return PACKS[launcher]


def expected_text(launcher, mode="ordinary"):
    if mode not in MODES:
        raise ValueError(f"unknown game language mode {mode!r}")
    return ENGLISH if mode in FORCED_ENGLISH_MODES else game_text(launcher)


def _other(text):
    return SPANISH if text == ENGLISH else ENGLISH


def message_path(text):
    return f"Data/message/mes_{text.message}.txtres"


def font_paths(text):
    return tuple(f"Data/sysres/font{index}/utf8_{text.font}.brfnt" for index in (0, 1, 2))


def layout_paths(text):
    return (f"Data/layout/dialog_{text.layout}.arc", f"Data/layout/game_{text.layout}.arc")


@dataclass(frozen=True)
class DvdRead:
    path: str
    offset: int
    length: int


@dataclass(frozen=True)
class DvdLog:
    opens: tuple[str, ...]
    reads: tuple[DvdRead, ...]
    malformed: tuple[str, ...] = ()

    def first_open(self, path):
        return next((index for index, opened in enumerate(self.opens) if opened == path), None)

    def was_read(self, path):
        return any(read.path == path and read.length > 0 for read in self.reads)


def parse_dvd_log(text):
    """runtime/hle/dvd.cpp's `[dvd] open <path>` and `[dvd] read <path> offset N length N` lines; a partial line is reported, never guessed."""
    opens, reads, malformed = [], [], []
    for line in text.splitlines():
        line = line.rstrip("\r")
        if line.startswith("[dvd] open"):
            path = line[len("[dvd] open"):].strip()
            if path:
                opens.append(path)
            else:
                malformed.append(line)
        elif line.startswith("[dvd] read "):
            path, separator, rest = line[len("[dvd] read "):].rpartition(" offset ")
            offset, length_separator, length = rest.partition(" length ")
            try:
                if not separator or not length_separator or not path:
                    raise ValueError
                reads.append(DvdRead(path, int(offset), int(length)))
            except ValueError:
                malformed.append(line)
    return DvdLog(tuple(opens), tuple(reads), tuple(malformed))


@dataclass(frozen=True)
class Check:
    text: GameText
    message_opened: bool
    message_read: bool
    problems: tuple[str, ...] = ()


def analyse(text, dvd, *, require_reads=False):
    """What the guest opened against the expected pack. A live judge requires the selected message to be read (`--log-dvd`)."""
    problems = []
    message = message_path(text)
    message_opened = dvd.first_open(message) is not None
    if not message_opened:
        problems.append(f"message not opened: {message}")
    elif (require_reads or dvd.reads) and not dvd.was_read(message):
        problems.append(f"message opened but not read: {message}")
    for path in font_paths(text):
        if dvd.first_open(path) is None:
            problems.append(f"font not opened: {path}")
    for path in layout_paths(text):
        if dvd.first_open(path) is None:
            problems.append(f"layout not opened: {path}")
    other = _other(text)
    wrong = [("message", other_message) for other_message in (message_path(other),)] + \
            [("font", path) for path in font_paths(other)] + [("layout", path) for path in layout_paths(other)]
    for label, path in wrong:
        if dvd.first_open(path) is not None:
            problems.append(f"other language {label} opened: {path}")
    return Check(text, message_opened, dvd.was_read(message), tuple(problems))


def parse_language_line(text):
    """The runtime's startup `[language] launcher=... guest=... message=... font=... pack=...` line as a dict, or None."""
    for line in text.splitlines():
        if line.startswith("[language] "):
            values = dict(token.split("=", 1) for token in line[len("[language] "):].split() if "=" in token)
            return values or None
    return None


def diagnostic_problems(text, info):
    """The diagnostic must agree with the pack the opens prove; a missing diagnostic is no failure."""
    if not info:
        return ()
    expected = {"guest": str(text.index), "message": text.message, "font": text.font}
    return tuple(f"startup diagnostic {key}={info[key]}, expected {value}"
                 for key, value in expected.items() if key in info and info[key] != value)


def prepare_config(run_dir, language, template=None):
    """Write this run's config copy with launcher.language, keeping the template's other settings; never a dist/install config."""
    if language != "system" and language not in LAUNCHER_LANGUAGES:
        raise ValueError(f"config: unsupported launcher language {language!r}")
    source = Path(template) if template is not None else ROOT / "dist" / "config.json"
    data = json.loads(source.read_text(encoding="utf-8")) if source.is_file() else {}
    if not isinstance(data, dict):
        raise ValueError("config: root must be an object")
    launcher = data.get("launcher", {})
    if not isinstance(launcher, dict):
        raise ValueError("config: launcher must be an object")
    launcher["language"] = language
    data["launcher"] = launcher
    path = Path(run_dir) / "config.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=1), encoding="utf-8")
    return path


def config_language(path):
    """The stored launcher language, with launcher/localization.cpp config_language() validation."""
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError("config: root must be an object")
    if "launcher" not in data:
        return "system"
    settings = data["launcher"]
    if not isinstance(settings, dict):
        raise ValueError("config: launcher must be an object")
    if "language" not in settings:
        return "system"
    language = settings["language"]
    if not isinstance(language, str):
        raise ValueError("config: launcher.language must be a string")
    if language != "system" and language not in LAUNCHER_LANGUAGES:
        raise ValueError("config: unsupported launcher language")
    return language


def mode_args(mode, run_dir, *, demo=None, stop=None, det_frames=1200):
    """The game arguments of one case, the same flags the existing deterministic/demo harness uses."""
    if mode not in MODES:
        raise ValueError(f"unknown game language mode {mode!r}")
    if mode == "netplay":
        raise ValueError("netplay peers are built by the pair case")
    if mode == "ordinary":
        return []
    if mode == "deterministic":
        return ["--deterministic"]
    if mode == "det-out":
        return ["--det-out", str(Path(run_dir) / "trace.det"), "--det-frames", str(det_frames)]
    if demo is None:
        raise ValueError(f"--{mode} needs a demo file")
    if mode == "demo-record":
        return ["--deterministic", "--demo-record", str(demo), "--game-hz", str(DEMO_GAME_HZ)]
    if stop is None:
        raise ValueError("--demo-play needs a stop retrace")
    return ["--deterministic", "--demo-play", str(demo), "--stop-retrace", str(stop), "--game-hz", str(DEMO_GAME_HZ)]


# cases ----------------------------------------------------------------------
def _new_run(folder, launcher, mode, *, demo=None, stop=None, seconds=240, game_args=()):
    game = Game(run_dir=folder, seconds=seconds,
                game_args=[*LOG_ARGS, *mode_args(mode, folder, demo=demo, stop=stop), *game_args])
    prepare_config(game.run_dir, launcher)   # before __enter__: the game starts from this config copy
    return game


def _to_title(game, captures, *, settle=120):
    with game.step("skip_movies"):
        steps.skip_movies(game)
    if settle:   # the title page animates in; a run bounded by --det-frames has no game time to spare
        with game.step("title_settle"):
            game.advance(settle)
    captures.append(game.capture())


def _to_gameplay(game, captures):
    _to_title(game, captures)
    for target in scenarios.route(1):
        with game.step(target):
            steps.select(game, target)
    with game.step("wait_gameplay"):
        steps.wait_gameplay(game)
    captures.append(game.capture())


def _judge(text, log, returns, captures, run):
    dvd = parse_dvd_log(log)
    check = analyse(text, dvd, require_reads=True)   # a live --log-dvd run must prove the guest read the selected message, not only opened it
    info = parse_language_line(log)
    problems = list(check.problems) + list(diagnostic_problems(text, info))
    problems += [f"game exit={code}, expected 0" for code in returns if code != 0]
    return {"ok": not problems, "run": str(run),
            "expected": {"guest": text.index, "message": text.message, "font": text.font, "layout": text.layout},
            "diagnostic": info, "message_read": check.message_read,
            "opens": list(dvd.opens), "reads": len(dvd.reads), "malformed": list(dvd.malformed),
            "captures": captures, "problems": problems}


def _single(folder, launcher, mode, *, gameplay=False):
    game = _new_run(folder, launcher, mode)
    captures = []
    with game:
        (_to_gameplay if gameplay else _to_title)(game, captures)
        log = game.finish()
        returns = [game.process.returncode]
    return _judge(expected_text(launcher, mode), log, returns, captures, folder)


def _det_out(folder):
    """--det-out hashes MEM1+MEM2 (88 MiB) at every retrace, so a run advances about one retrace a second and cannot reach --det-frames 1200 in a
    bounded acceptance run (measured: 28 retraces in 33 s). The case proves the trace mode is active and the English pack opens, then quits normally;
    reaching --det-frames is covered by scripts/det_check.py, not by this language acceptance."""
    game = _new_run(folder, "es", "det-out")
    captures = []
    with game:
        _to_title(game, captures, settle=0)
        retraces = game.frame.get("retraces", 0)
        log = game.finish()
        returns = [game.process.returncode]
    result = _judge(expected_text("es", "det-out"), log, returns, captures, folder)
    trace = folder / "trace.det"
    result["trace_bytes"] = trace.stat().st_size if trace.is_file() else 0
    result["retraces_before_quit"] = retraces
    if not result["trace_bytes"]:
        result["problems"].append(f"--det-out wrote no trace: {trace}")
        result["ok"] = False
    return result


def _demo(folder):
    """Record the title in deterministic mode, then replay it to the very retrace the recorded title was captured on."""
    demo = folder / "language.demo"
    recorder = _new_run(folder / "record", "es", "demo-record", demo=demo)
    captures = []
    with recorder:
        _to_title(recorder, captures)
        stop = recorder.frame.get("retraces", 0)
        record_log = recorder.finish()
        record_returns = [recorder.process.returncode]
    result = {"ok": False, "problems": [], "stop_retrace": stop,
              "record": _judge(ENGLISH, record_log, record_returns, captures, folder / "record")}
    result["problems"] += result["record"]["problems"]
    if not demo.is_file() or demo.stat().st_size == 0:
        result["problems"].append(f"demo record wrote no demo file: {demo}")
        return result
    if not stop:
        result["problems"].append("the recorded title produced no retrace to replay to")
        return result
    replay = _new_run(folder / "replay", "es", "demo-play", demo=demo, stop=stop, seconds=300)
    with replay:
        try:   # the game ends its own run at --stop-retrace, its normal exit being the termination signal
            replay.process.wait(300)
        except subprocess.TimeoutExpired:
            replay.fail(f"the demo replay did not reach --stop-retrace {stop} within 300 s", "demo replay")
        reached = replay.frame.get("retraces", 0)
        replay_log = replay.finish()
        replay_returns = [replay.process.returncode]
    if reached < stop:
        result["problems"].append(f"the demo replay ended at retrace {reached}, before --stop-retrace {stop}")
    result["reached_retrace"] = reached
    result["replay"] = _judge(ENGLISH, replay_log, replay_returns, [], folder / "replay")
    result["problems"] += result["replay"]["problems"]
    result["ok"] = not result["problems"]
    return result


def _confirmed(log):
    for marker in ("Network desync", "Network outage exceeded"):
        if marker in log:
            raise AssertionError(f"{marker} in a netplay run.log")
    return rollback_log(log)[0]


def _netplay(folder):
    """An English and a Spanish launcher config: both peers must load the disc English, survive a delayed input (netplay_history), reach coop
    gameplay and change the shared magazine by firing."""
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    relay = DelayRelay(port)
    try:
        with contextlib.ExitStack() as stack:
            host = Game(run_dir=folder / "host", seconds=420, game_args=[
                "--net-host", "--net-port", str(port), "--render-scale", "1", *LOG_ARGS])
            prepare_config(host.run_dir, "en")
            host = stack.enter_context(host)
            client = Game(run_dir=folder / "client", seconds=420, game_args=[
                "--net-join", "127.0.0.1", "--net-port", str(relay.port), "--render-scale", "1", *LOG_ARGS])
            prepare_config(client.run_dir, "es")
            client = stack.enter_context(client)
            logs = lambda: [(game.run_dir / "run.log").read_text(errors="replace") for game in (host, client)]
            confirmed = lambda: min(max(_confirmed(logs()[0]), default=0), max(_confirmed(logs()[1]), default=0))
            rewinds = lambda: [((rollback_log(text)[1] or {}).get("count", 0)) for text in logs()]
            host.wait(lambda _: confirmed() >= 240, seconds=40, awaited="both peers confirmed initial frames")
            before = confirmed()
            rewinds_before = rewinds()
            relay.set_profile(5)   # a bounded delayed input: the peers must roll back and recover, not exhaust their history
            client.aim(.3, .2, wait=False)
            started = time.monotonic()
            host.wait(lambda _: time.monotonic() - started >= 6, seconds=15, awaited="delayed input survived")
            for game in (host, client):
                assert game.process.poll() is None, "a peer exited during the input delay"
                _confirmed((game.run_dir / "run.log").read_text(errors="replace"))
            relay.set_profile(0)
            host.wait(lambda _: confirmed() >= before + 480, seconds=40, awaited="both peers resumed confirmation")
            host_log = logs()[0]
            assert rewinds()[0] > rewinds_before[0], \
                f"the delayed input did not rewind the host: rollback count {rewinds_before[0]} -> {rewinds()[0]}"
            relay.check()
            coop_menu(host, client)
            pointers = [word(host, 0x806CA620 - 27464 + index * 4) for index in range(2)]
            assert all(0x80000000 <= pointer < 0x81800000 for pointer in pointers) and pointers[0] != pointers[1], \
                f"coop players are missing: {pointers}"
            ammo = lambda: [word(host, pointer + 0x4D4) for pointer in pointers]
            controls, magazines = [], []
            for index, game in enumerate((host, client)):
                steps.wait_gameplay(host, stable_ticks=3)
                game.aim(-.5 if index == 0 else .5, -.3, settle_frames=3)
                loaded = ammo()
                if loaded[index] == 0:
                    steps.perform(game, "reload")
                    host.wait(lambda _: ammo()[index] > 0, seconds=15, awaited=f"P{index + 1} initial magazine")
                    loaded = ammo()
                controls.append(game.mouse_button(3, 3, wait=True))   # a physical right-mouse shot through the network
                host.advance(15)
                spent = ammo()
                assert spent[index] < loaded[index] and spent[1 - index] == loaded[1 - index], \
                    f"P{index + 1} shot: magazines {loaded} -> {spent}"
                magazines.append({"player": index + 1, "before": loaded, "after": spent})
            left, right = _confirmed(logs()[0]), _confirmed(logs()[1])
            common = sorted(left.keys() & right.keys())
            assert len(common) >= 5 and all(left[frame] == right[frame] for frame in common), \
                "confirmed hashes are missing or disagree at the same frame"
            english = [_judge(ENGLISH, logs()[0], [], [], host.run_dir), _judge(ENGLISH, logs()[1], [], [], client.run_dir)]
            result = {"ok": False, "configs": {"host": config_language(host.run_dir / "config.json"),
                                               "client": config_language(client.run_dir / "config.json")},
                      "controls_accepted_frames": controls, "magazines": magazines,
                      "confirmed_frames": {"common": len(common), "last": common[-1], "before_delay": before},
                      "english": english, "problems": [problem for peer in english for problem in peer["problems"]]}
            if result["configs"] != {"host": "en", "client": "es"}:
                result["problems"].append(f"peer configs are {result['configs']}, expected host en and client es")
            client.finish()
            host.process.wait(30)
            codes = {"host": host.process.returncode, "client": client.process.returncode}
            result["exit_codes"] = codes
            if codes != {"host": 5, "client": 0}:   # 5: the host's normal remote-shutdown code
                result["problems"].append(f"exit codes {codes}, expected host 5 and client 0")
            result["ok"] = not result["problems"]
            return result
    finally:
        relay.close()


RUNNERS = {
    "es": lambda folder: _single(folder, "es", "ordinary", gameplay=True),
    "en": lambda folder: _single(folder, "en", "ordinary"),
    "ru": lambda folder: _single(folder, "ru", "ordinary"),
    "deterministic": lambda folder: _single(folder, "es", "deterministic"),
    "det-out": _det_out,
    "demo": _demo,
    "netplay": _netplay,
}


def run_cases(names, folder, *, fail_fast=False):
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    report = {"ok": False, "folder": str(folder), "cases": {}}
    for name in names:
        case_folder = folder / name
        case_folder.mkdir(parents=True, exist_ok=True)
        started = time.monotonic()
        try:
            result = RUNNERS[name](case_folder)
        except StepFailed as error:
            result = {"ok": False, "error": str(error), "artifacts": error.artifacts}
        except Exception as error:   # a broken case must not lose the evidence of the others
            result = {"ok": False, "error": f"{type(error).__name__}: {error}"}
        result["seconds"] = round(time.monotonic() - started, 1)
        report["cases"][name] = result
        print(f"language {name}: {'ok' if result.get('ok') else 'FAILED'}", flush=True)
        if not result.get("ok") and fail_fast:
            break
    report["ok"] = bool(report["cases"]) and all(case.get("ok") for case in report["cases"].values())
    (folder / "report.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description="Act on the real game with different launcher languages (docs/testing/test-server.md).")
    parser.add_argument("--case", action="append", choices=CASES, help="run only this case (repeatable; default: all)")
    parser.add_argument("--fail-fast", action="store_true")
    args = parser.parse_args(argv)
    folder = ROOT / "build" / "autotest" / f"language-{time.time_ns()}"
    print(folder, flush=True)
    report = run_cases(args.case or DEFAULT_CASES, folder, fail_fast=args.fail_fast)
    for name, case in report["cases"].items():
        print(f"  {name}: {'ok' if case.get('ok') else 'FAILED'}"
              f"{'' if case.get('ok') else ' - ' + '; '.join(case.get('problems') or [case.get('error', 'failed')])}", flush=True)
    print(f"artifacts: {folder / 'report.json'}", flush=True)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

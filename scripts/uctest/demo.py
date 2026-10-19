"""Deterministic demos of the heavy fight.

  python -m uctest demo record <name>          the fight scenario runs live (bot, harness) in deterministic mode while the game records every input
                                                the guest reads; build/demos/<name>.demo plus <name>.json (the retrace window to measure) are written
  python -m uctest bench <exe> <exe> --demo <name> [--runs 2]
                                                each run replays the demo without bot or harness: identical guest work every time, so the wall time of
                                                the measured window compares builds directly

A demo stays valid while the guest code, the HLE behaviour that the guest can see and the saves it starts from stay the same; after a change that
alters guest-visible behaviour record it again (`python -m uctest demo check <name>` replays it twice and compares guest-memory hashes).
"""
import hashlib
import json
import os
import re
import shutil
import statistics
import subprocess
import time
import types
from pathlib import Path

from .bench import t95
from .client import guest_cpu_args

ROOT = Path(__file__).resolve().parents[2]
DEMOS = ROOT / "build" / "demos"
DETBENCH = re.compile(r"^\[detbench\] retrace (\d+) draws (\d+) guest_mcycles ([0-9.]+) wall_ms ([0-9.]+)")
FPS = re.compile(r"^\[fps\] window_ms ([0-9.]+) retraces (\d+):(\d+) presents (\d+)", re.MULTILINE)


def read_peak_fps(log, window):
    """Peak presentation rate over complete wall-second intervals inside the measured fight."""
    rates = [1000 * int(presents) / float(ms) for ms, first, last, presents in FPS.findall(log)
             if float(ms) >= 1000 and window["from"] <= int(first) <= int(last) <= window["to"]]
    return max(rates, default=None)


def paths(name):
    return DEMOS / f"{name}.demo", DEMOS / f"{name}.json"


def record(args, run_once):
    from .client import SAVE_SOURCE
    demo, meta = paths(args.name)
    DEMOS.mkdir(parents=True, exist_ok=True)
    trace = DEMOS / f"{args.name}.trace"
    trace.unlink(missing_ok=True)
    run = types.SimpleNamespace(exe=args.exe, speed=8, god=True, skip_tutorials=True, saves_from=None, save_as=None, chapter=1, mark=False,
                                measure=args.measure, max_minutes=args.max_minutes, repeat=1, slow_at=args.slow_at, wave_offset=args.wave_offset,
                                game_arg=["--deterministic", "--demo-record", str(demo), "--profile", "--game-hz", str(DET_GAME_HZ)]
                                + (["--det-out", str(trace), "--det-frames", "1000000"] if args.trace else []))   # hashing 88 MB per retrace slows the whole run (menus 5x), so only on request
    report, run_dir = run_once("fight", run)
    if not report["ok"]:
        print(f"demo: the recording run failed: {report.get('error')}")
        return 1
    window = report["result"]["demo_window"]
    window["to"] = min(window["to"], window["from"] + args.measure * 60)   # the measure step ran for wall seconds; the demo window is game time (the deterministic game runs faster than real time)
    meta.write_text(json.dumps({"from": window["from"], "to": window["to"], "run": run_dir.name, "marked": report["result"]["marked"]}, indent=1))
    print(f"demo: {demo} ({demo.stat().st_size // 1024} KiB), window retraces {window['from']}..{window['to']} ({(window['to'] - window['from']) / 60:.1f} game seconds)")
    return 0


DET_GAME_HZ = 120   # step rate of deterministic runs (virtual clock: every game second holds exactly this many steps, however slow the host). Real unlimited play steps as fast as the machine allows (about 120-190 Hz in the heavy fight); the 480 Hz ceiling would make a deterministic game second 16x the work of the original 30 Hz and the demo crawl
def replay_args(exe, demo, stop, saves, extra=()):
    pin = guest_cpu_args(extra)
    pin += [] if "--fps-limit" in extra else ["--fps-limit", "0"]   # every test runs the game unlocked, as a player does; a limit only by an explicit argument
    pin += [] if "--game-hz" in extra else ["--game-hz", str(DET_GAME_HZ)]
    if "--window-mode" not in extra: pin += ["--window-mode", "windowed"]
    return [str(exe), "--test-server", "demo", "--save-root", str(saves), "--width", "1280", "--height", "720", "--god", "--skip-tutorials",
            "--skip-intro", "--deterministic", "--demo-play", str(demo), "--stop-retrace", str(stop), "--mute", "--no-vulkan-layers", "--profile", *pin, *extra]


def replay_once(spec, name, run_dir):
    exe, *extra = spec.split("|")   # "dist/uc.exe|--early-draw-done": per-build game arguments after the path
    if not Path(exe).is_absolute() and not Path(exe).exists():
        exe = str(ROOT / exe)   # relative paths are meant from the repository root
    from .client import SAVE_SOURCE
    demo, meta = paths(name)
    window = json.loads(meta.read_text())
    saves = run_dir / "saves"
    if saves.exists():
        shutil.rmtree(saves)
    shutil.copytree(SAVE_SOURCE, saves)
    log_path = run_dir / "run.log"
    command = replay_args(Path(exe).resolve(), demo, window["to"], saves, extra)
    (run_dir / "launch.json").write_text(json.dumps({
        "command": command, "started_at_ms": time.time_ns() // 1000000,
        "exe_sha256": hashlib.sha256(Path(exe).read_bytes()).hexdigest(),
        "demo_sha256": hashlib.sha256(demo.read_bytes()).hexdigest(),
        "diagnostic_environment": {key: os.environ.get(key) for key in ("UC_CPU_SAMPLE", "UC_CPU_SAMPLE_FLAG", "UC_GX_CACHE_VERIFY", "LLVM_PROFILE_FILE")},
    }, indent=2))
    with open(log_path, "wb") as log:
        subprocess.run(command, stdout=log, stderr=log, cwd=Path(exe).resolve().parent, timeout=1800, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    rows = []
    for line in log_path.read_text(errors="replace").splitlines():
        m = DETBENCH.match(line)
        if m and window["from"] < int(m[1]) <= window["to"]:
            rows.append((int(m[1]), int(m[2]), float(m[3]), float(m[4])))
    return rows, window


def replay(args):
    """Replay the demo once (profile-guided optimization training: scripts/pgo.py)."""
    run_dir = ROOT / "build" / "autotest" / f"demo-replay-{int(time.time() * 1000)}"
    run_dir.mkdir(parents=True)
    rows, window = replay_once(args.exe, args.name, run_dir)
    print(f"replay: {len(rows)} game seconds measured, {sum(r[3] for r in rows) / 1000:.2f} s wall ({run_dir})")
    return 0


def profile(args):
    """Replay the demo with the CPU sampler of a profile build (build-prof/game/uc.exe copied next to dist data) running over the measured window;
    resolve the samples with scripts/resolve_cpu_samples.py <cpu-samples.txt> <exe>."""
    import os
    demo, meta = paths(args.name)
    window = json.loads(meta.read_text())
    exe = Path(args.exe).resolve()
    run_dir = ROOT / "build" / "autotest" / f"demo-profile-{int(time.time() * 1000)}"
    run_dir.mkdir(parents=True)
    from .client import SAVE_SOURCE
    shutil.copytree(SAVE_SOURCE, run_dir / "saves")
    flag = run_dir / "sample.flag"
    env = dict(os.environ, UC_CPU_SAMPLE="guest", UC_CPU_SAMPLE_FLAG=str(flag), UC_CPU_SAMPLE_RETRACE=str(getattr(args, "from_retrace", 0) or window["from"]))
    samples = exe.parent / "cpu-samples.txt"
    samples.unlink(missing_ok=True)
    with open(run_dir / "run.log", "wb") as log:
        subprocess.run(replay_args(exe, demo, window["to"], run_dir / "saves"), stdout=log, stderr=log, cwd=exe.parent, env=env, timeout=1800, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    kept = run_dir / "cpu-samples.txt"
    if samples.exists():
        shutil.copy(samples, kept)
    print(f"profile: samples {kept if kept.exists() else 'MISSING'} (resolve: python scripts/resolve_cpu_samples.py {kept} <exe with pdb> 45)")
    return 0 if kept.exists() else 1


def bench(args):
    DEMOS.mkdir(parents=True, exist_ok=True)
    results = {exe: [] for exe in args.exes}
    reference_work = reference_window = None
    for round_ in range(args.runs):
        for exe in (args.exes if round_ % 2 == 0 else args.exes[::-1]):   # A B, then B A: a slow drift of the machine hits both builds alike over two rounds
            run_dir = ROOT / "build" / "autotest" / f"demo-{int(time.time() * 1000)}"
            run_dir.mkdir(parents=True)
            rows, window = replay_once(exe, args.demo, run_dir)
            expected = (window["to"] - window["from"]) // 60
            if len(rows) < expected - 1:
                print(f"bench: only {len(rows)} of {expected} game seconds measured; see {run_dir / 'run.log'}")
                return 1
            work = [(r[0], r[1]) for r in rows]
            measured_window = (window["from"], window["to"])
            if reference_work is None:
                reference_work, reference_window = work, measured_window
            elif work != reference_work or measured_window != reference_window:
                print(f"bench: INVALID: guest work differs by retrace or draw count; no speed verdict; see {run_dir / 'run.log'}")
                return 1
            wall, cycles, draws = sum(r[3] for r in rows), sum(r[2] for r in rows), sum(r[1] for r in rows)
            peak = read_peak_fps((run_dir / "run.log").read_text(errors="replace"), window)
            peak_text = f"{peak:.1f}" if peak is not None else "unavailable (older executable)"
            results[exe].append((wall, cycles, draws, peak))
            print(f"bench: round {round_ + 1}/{args.runs} {exe.split('|', 1)[0].rsplit(chr(92), 1)[-1].rsplit('/', 1)[-1]}{'|' + exe.split('|', 1)[1] if '|' in exe else ''}: {wall / 1000:.2f} s wall for {len(rows)} game seconds ({len(rows) * 60 / (wall / 1000):.1f} retraces/s), "
                  f"{cycles:.0f} Mcycles, {draws} draws, peak FPS (1s): {peak_text}", flush=True)
    names = list(args.exes)
    lines = []
    for exe in names:
        walls = [r[0] for r in results[exe]]
        sd = 100 * statistics.stdev(walls) / statistics.fmean(walls) if len(walls) > 1 else 0.0
        peaks = [r[3] for r in results[exe] if r[3] is not None]
        peak_text = f"{max(peaks):.1f}" if peaks else "unavailable (older executable)"
        lines.append(f"  {exe}: {statistics.fmean(walls) / 1000:.3f} s wall (sd between runs {sd:.1f}%), draws {results[exe][0][2]} in every run: {len({r[2] for r in results[exe]}) == 1}, peak FPS (1s): {peak_text}")
    for exe in names[1:]:
        diffs = [100 * (b[0] / a[0] - 1) for a, b in zip(results[names[0]], results[exe])]
        mean = statistics.fmean(diffs)
        # one pair is the normal case (run-to-run sd is 1-2.6%): judge it against a 5% noise floor (the machine drifts); with more rounds use the paired interval
        half = t95(len(diffs) - 1) * statistics.stdev(diffs) / len(diffs) ** 0.5 if len(diffs) > 1 else 5.0
        verdict = "INCONCLUSIVE" if abs(mean) <= half else ("FASTER" if mean < 0 else "SLOWER")
        best = 100 * (min(r[0] for r in results[exe]) / min(r[0] for r in results[names[0]]) - 1)   # a pause of the machine only ever makes a run slower: the best run is the steadiest figure
        base_peaks = [r[3] for r in results[names[0]] if r[3] is not None]
        peaks = [r[3] for r in results[exe] if r[3] is not None]
        peak_comparison = ""
        if base_peaks and peaks and max(base_peaks) > 0:
            peak_change = 100 * (max(peaks) / max(base_peaks) - 1)
            peak_comparison = f"; peak FPS (1s): {max(base_peaks):.1f} -> {max(peaks):.1f} ({peak_change:+.1f}%)"
            if verdict == "SLOWER" and peak_change > 0:
                peak_comparison += "; mixed result: retain for investigation"
        lines.append(f"{exe} vs {names[0]}: {mean:+.1f}% wall time, {"95% CI" if len(diffs) > 1 else "noise floor"} [{mean - half:+.1f}%, {mean + half:+.1f}%] over {len(diffs)} paired run(s) -> {verdict}; best run vs best run {best:+.1f}%{peak_comparison}")
    print("\n".join(lines))
    return 0


def check(args):
    """Replay the demo twice with the hash trace and compare (the replay must reproduce the recorded run bit for bit)."""
    from . import bench as unused  # noqa: F401
    import sys
    sys.path.insert(0, str(ROOT / "scripts"))
    import det_check
    from .client import SAVE_SOURCE
    demo, meta = paths(args.name)
    window = json.loads(meta.read_text())
    folder = ROOT / "build" / "det"
    folder.mkdir(parents=True, exist_ok=True)
    traces = []
    for index in range(2):
        saves = folder / f"demo-saves{index}"
        if saves.exists():
            shutil.rmtree(saves)
        shutil.copytree(SAVE_SOURCE, saves)
        out = folder / f"demo{index}.bin"
        with open(out.with_suffix(".log"), "wb") as log:
            subprocess.run(replay_args(Path(args.exe).resolve(), demo, window["to"], saves, ["--det-out", str(out), "--det-frames", str(args.frames)]),
                           stdout=log, stderr=log, cwd=Path(args.exe).resolve().parent, timeout=3600, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        traces.append(det_check.read_trace(out))
        print(f"demo check: replay {index}: {len(traces[-1])} retraces", flush=True)
    recorded = DEMOS / f"{args.name}.trace"
    text, same = det_check.compare(traces[0], traces[1])
    print("replay 0 vs replay 1:\n" + text)
    if recorded.exists():
        text, same_recorded = det_check.compare(det_check.read_trace(recorded)[:len(traces[0])], traces[0])   # the live run went on a little past the window
        print("recorded run vs replay 0:\n" + text)
        same = same and same_recorded
    return 0 if same else 1

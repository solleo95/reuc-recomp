"""Deterministic demos of the heavy fight.

  python -m uctest demo record <name>          the fight scenario runs live (bot, harness) in deterministic mode while the game records every input
                                                the guest reads; build/demos/<name>.demo plus <name>.json (the retrace window to measure) are written
  python -m uctest bench <exe> <exe> --demo <name> [--runs 2]
                                                each run replays the demo without bot or harness: identical guest work every time, so the wall time of
                                                the measured window compares builds directly

A demo stays valid while the guest code, the HLE behaviour that the guest can see and the saves it starts from stay the same; after a change that
alters guest-visible behaviour record it again (`python -m uctest demo check <name>` replays it twice and compares guest-memory hashes).
"""
import json
import re
import shutil
import statistics
import subprocess
import time
import types
from pathlib import Path

from .bench import t95

ROOT = Path(__file__).resolve().parents[2]
DEMOS = ROOT / "build" / "demos"
DETBENCH = re.compile(r"^\[detbench\] retrace (\d+) draws (\d+) guest_mcycles ([0-9.]+) wall_ms ([0-9.]+)")


def paths(name):
    return DEMOS / f"{name}.demo", DEMOS / f"{name}.json"


def record(args, run_once):
    from .client import SAVE_SOURCE
    demo, meta = paths(args.name)
    DEMOS.mkdir(parents=True, exist_ok=True)
    trace = DEMOS / f"{args.name}.trace"
    trace.unlink(missing_ok=True)
    run = types.SimpleNamespace(exe=args.exe, speed=8, god=True, skip_tutorials=True, saves_from=None, save_as=None, chapter=1, mark=False,
                                measure=args.measure, max_minutes=args.max_minutes, repeat=1,
                                game_arg=["--deterministic", "--demo-record", str(demo), "--profile"]
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


GUEST_CPU = 4   # logical CPU the guest threads are pinned to in replays (its SMT sibling stays free): a process landing on another core or CCD ran up to 5% faster or slower


def replay_args(exe, demo, stop, saves, extra=()):
    pin = ["--guest-cpu", str(GUEST_CPU)] if GUEST_CPU >= 0 and "--guest-cpu" not in extra else []
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
    with open(log_path, "wb") as log:
        subprocess.run(replay_args(Path(exe).resolve(), demo, window["to"], saves, extra), stdout=log, stderr=log, cwd=Path(exe).resolve().parent, timeout=1800)
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
        subprocess.run(replay_args(exe, demo, window["to"], run_dir / "saves"), stdout=log, stderr=log, cwd=exe.parent, env=env, timeout=1800)
    kept = run_dir / "cpu-samples.txt"
    if samples.exists():
        shutil.copy(samples, kept)
    print(f"profile: samples {kept if kept.exists() else 'MISSING'} (resolve: python scripts/resolve_cpu_samples.py {kept} <exe with pdb> 45)")
    return 0 if kept.exists() else 1


def bench(args):
    DEMOS.mkdir(parents=True, exist_ok=True)
    results = {exe: [] for exe in args.exes}
    for round_ in range(args.runs):
        for exe in (args.exes if round_ % 2 == 0 else args.exes[::-1]):   # A B, then B A: a slow drift of the machine hits both builds alike over two rounds
            run_dir = ROOT / "build" / "autotest" / f"demo-{int(time.time() * 1000)}"
            run_dir.mkdir(parents=True)
            rows, window = replay_once(exe, args.demo, run_dir)
            expected = (window["to"] - window["from"]) // 60
            if len(rows) < expected - 1:
                print(f"bench: only {len(rows)} of {expected} game seconds measured; see {run_dir / 'run.log'}")
                return 1
            wall, cycles, draws = sum(r[3] for r in rows), sum(r[2] for r in rows), sum(r[1] for r in rows)
            results[exe].append((wall, cycles, draws))
            print(f"bench: round {round_ + 1}/{args.runs} {exe.split('|', 1)[0].rsplit(chr(92), 1)[-1].rsplit('/', 1)[-1]}{'|' + exe.split('|', 1)[1] if '|' in exe else ''}: {wall / 1000:.2f} s wall for {len(rows)} game seconds ({len(rows) * 60 / (wall / 1000):.1f} frames/s), "
                  f"{cycles:.0f} Mcycles, {draws} draws", flush=True)
    names = list(args.exes)
    lines = []
    for exe in names:
        walls = [r[0] for r in results[exe]]
        sd = 100 * statistics.stdev(walls) / statistics.fmean(walls) if len(walls) > 1 else 0.0
        lines.append(f"  {exe}: {statistics.fmean(walls) / 1000:.3f} s wall (sd between runs {sd:.1f}%), draws {results[exe][0][2]} in every run: {len({r[2] for r in results[exe]}) == 1}")
    for exe in names[1:]:
        diffs = [100 * (b[0] / a[0] - 1) for a, b in zip(results[names[0]], results[exe])]
        mean = statistics.fmean(diffs)
        # one pair is the normal case (run-to-run sd is 1-2.6%): judge it against a 5% noise floor (the machine drifts); with more rounds use the paired interval
        half = t95(len(diffs) - 1) * statistics.stdev(diffs) / len(diffs) ** 0.5 if len(diffs) > 1 else 5.0
        verdict = "no difference" if abs(mean) <= half else ("FASTER" if mean < 0 else "SLOWER")
        best = 100 * (min(r[0] for r in results[exe]) / min(r[0] for r in results[names[0]]) - 1)   # a pause of the machine only ever makes a run slower: the best run is the steadiest figure
        lines.append(f"{exe} vs {names[0]}: {mean:+.1f}% wall time, {"95% CI" if len(diffs) > 1 else "noise floor"} [{mean - half:+.1f}%, {mean + half:+.1f}%] over {len(diffs)} paired run(s) -> {verdict}; best run vs best run {best:+.1f}%")
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
            subprocess.run(replay_args(Path(args.exe).resolve(), demo, window["to"], saves, ["--det-out", str(out), "--det-frames", "1000000"]),
                           stdout=log, stderr=log, cwd=Path(args.exe).resolve().parent, timeout=3600)
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

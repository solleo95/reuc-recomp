"""Deterministic A/B benchmark ON the Steam Deck, in Game Mode (the owner can watch it): replays a recorded demo (build/demos/<name>.demo) with each build and compares the wall time of the
measured window, as `python -m uctest bench --demo` does on the PC.

  python scripts/linux/deck_bench.py --demo fight4 --runs 2 A=build-linux/game/uc B=other/uc [--game-arg=--x]

Every build is copied next to the staged game as ~/uc-run/uc_<label> (the baseline label A also replaces nothing: `uc` stays as staged); a run is started by Steam's shortcut of
uc-launcher.sh, which goes to uc.sh, which sources next-run.sh (UC_EXE, the arguments and the log redirect). The identical guest work of every run is checked by retrace and draw counts.
"""
import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from uctest.deck import REMOTE, RUNTIME, SHORTCUT_ID, SSH, _ssh, gamescope_test_limiter   # noqa: E402
from uctest.client import SAVE_SOURCE   # noqa: E402

DETBENCH = re.compile(r"^\[detbench\] retrace (\d+) draws (\d+) guest_mcycles ([0-9.]+) wall_ms ([0-9.]+)")
FPS = re.compile(r"^\[fps\] window_ms ([0-9.]+) retraces (\d+):(\d+) presents (\d+)", re.MULTILINE)
BENCH = f"{REMOTE}/bench"


def put(local, remote, executable=False):
    with open(local, "rb") as source:
        subprocess.run(SSH + [f"cat > {remote}" + (f" && chmod +x {remote}" if executable else "")], stdin=source, check=True)


def prepare(demo, builds):
    _ssh(f"mkdir -p {BENCH}", check=True)
    demos = ROOT / "build" / "demos"
    put(demos / f"{demo}.demo", f"{BENCH}/{demo}.demo")
    have = _ssh(f"test -d {BENCH}/saves-src", capture_output=True).returncode == 0
    if not have:
        with subprocess.Popen(["tar", "-cf", "-", "-C", str(SAVE_SOURCE), "."], stdout=subprocess.PIPE) as tar:
            _ssh(f"mkdir -p {BENCH}/saves-src", check=True)
            subprocess.run(SSH + [f"tar -xf - -C {BENCH}/saves-src"], stdin=tar.stdout, check=True)
    for label, path in builds.items():
        put(path, f"{REMOTE}/uc_{label}", executable=True)


def one_run(label, demo, window, extra, tag):
    remote = f"{BENCH}/run"
    script = "\n".join([
        f"export UC_EXE=./uc_{label}",
        *([f"export UC_CPU_SAMPLE={os.environ['UC_CPU_SAMPLE']}", f"export UC_CPU_SAMPLE_OUT={BENCH}/cpu-samples.txt".replace("~", "$HOME")] if os.environ.get("UC_CPU_SAMPLE") else []),
        *([f"export UC_CPU_SAMPLE_STACKS={os.environ['UC_CPU_SAMPLE_STACKS']}"] if os.environ.get("UC_CPU_SAMPLE_STACKS") else []),
        *([f"export UC_SKIP_CALLSITES={os.environ['UC_SKIP_CALLSITES']}"] if os.environ.get("UC_SKIP_CALLSITES") else []),   # diagnostic: leave those guest callsites' draws out
        "set -- --test-server demo --save-root bench/run/saves --width 1280 --height 720 --god --skip-tutorials --skip-intro --deterministic "
        f"--demo-play bench/{demo}.demo --stop-retrace {window['to']} --mute --no-vulkan-layers --profile --fps-limit 0 --game-hz 120 --window-mode windowed " + " ".join(extra),
        f"exec > {BENCH}/run.log 2>&1", ""])
    _ssh(f"rm -rf {remote} {BENCH}/run.log; mkdir -p {remote} && cp -r {BENCH}/saves-src {remote}/saves", check=True)
    _ssh(f"cat > {REMOTE}/next-run.sh", input=script.encode(), check=True)
    names = "'uc|uc_.*'"   # the build under test is uc_<label>
    monitored = bool(os.environ.get("UC_DECK_MONITOR"))
    if monitored:   # what the game uses while it replays (scripts/linux/deck_monitor.py), summarised below
        put(ROOT / "scripts" / "linux" / "deck_monitor.py", f"{BENCH}/deck_monitor.py")
        _ssh(f"systemctl --user stop uc-monitor 2>/dev/null; systemd-run --user --unit=uc-monitor --collect python3 {BENCH}/deck_monitor.py {BENCH}/monitor.csv", stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    launch = (gamescope_test_limiter(names) + f"pkill -x {names}; XDG_RUNTIME_DIR={RUNTIME} DISPLAY=:0 steam -ifrunning steam://rungameid/{SHORTCUT_ID} >/dev/null 2>&1; "
              f"for i in $(seq 1 240); do pgrep -x {names} >/dev/null && break; sleep 0.5; done; while pgrep -x {names} >/dev/null; do test_unlimited_tick; sleep 1; done")
    subprocess.run(SSH + [launch], stdout=subprocess.DEVNULL, timeout=1800, check=True)
    if monitored:
        time.sleep(3)
        csv_text = subprocess.run(SSH + [f"cat {BENCH}/monitor.csv"], capture_output=True, text=True).stdout
        Path(ROOT / "build" / f"deck-bench-{label}-monitor.csv").write_text(csv_text)
    if os.environ.get("UC_CPU_SAMPLE"):
        samples = subprocess.run(SSH + [f"cat {BENCH}/cpu-samples.txt"], capture_output=True, text=True).stdout
        Path(ROOT / "build" / f"deck-bench-{label}-cpu-samples.txt").write_text(samples)
    log = subprocess.run(SSH + [f"cat {BENCH}/run.log"], capture_output=True, text=True, errors="replace").stdout
    rows = []
    for line in log.splitlines():
        m = DETBENCH.match(line)
        if m and window["from"] < int(m[1]) <= window["to"]:
            rows.append((int(m[1]), int(m[2]), float(m[3]), float(m[4])))
    return rows, log


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--demo", default="fight4")
    parser.add_argument("--runs", type=int, default=1, help="rounds (the owner wants one run per build)")
    parser.add_argument("--seconds", type=int, default=10, help="game seconds of the demo's fight window that are measured; the run stops there")
    parser.add_argument("--game-arg", action="append", default=[])
    parser.add_argument("builds", nargs="+", help="LABEL=path-to-uc (the first is the baseline); LABEL|--arg adds a game argument for that build only")
    args = parser.parse_args()
    builds, extras = {}, {}
    for spec in args.builds:
        label, path = spec.split("=", 1)
        label, *flags = label.split("|")
        builds[label], extras[label] = path, flags
    window = json.loads((ROOT / "build" / "demos" / f"{args.demo}.json").read_text())
    window["to"] = min(window["to"], window["from"] + 60 * args.seconds)   # a shorter window: the replay stops earlier
    prepare(args.demo, builds)
    results = {label: [] for label in builds}
    reference = None
    labels = list(builds)
    for round_ in range(args.runs):
        for label in (labels if round_ % 2 == 0 else labels[::-1]):
            rows, log = one_run(label, args.demo, window, args.game_arg + extras[label], round_)
            expected = (window["to"] - window["from"]) // 60
            if len(rows) < expected - 1:
                print(f"{label}: only {len(rows)} of {expected} game seconds measured\n" + "\n".join(log.splitlines()[-8:]))
                return 1
            work = [(r[0], r[1]) for r in rows]
            reference = reference or work
            if work != reference:
                print(f"INVALID: the guest work of {label} differs (retrace or draw counts)")
            wall, cycles, draws = sum(r[3] for r in rows), sum(r[2] for r in rows), sum(r[1] for r in rows)
            peaks = [1000 * int(p) / float(ms) for ms, a, b, p in FPS.findall(log) if float(ms) >= 1000 and window["from"] <= int(a) <= int(b) <= window["to"]]
            results[label].append(wall)
            print(f"round {round_ + 1}/{args.runs} {label}: {wall / 1000:.2f} s wall, {cycles:.0f} Mcycles, {draws} draws, peak FPS {max(peaks, default=0):.1f}", flush=True)
    base = statistics.fmean(results[labels[0]])
    for label in labels:
        walls = results[label]
        sd = 100 * statistics.stdev(walls) / statistics.fmean(walls) if len(walls) > 1 else 0
        print(f"  {label}: {statistics.fmean(walls) / 1000:.3f} s wall (sd {sd:.1f}%, best {min(walls) / 1000:.3f}), {100 * (statistics.fmean(walls) / base - 1):+.1f}% vs {labels[0]}")
    _ssh(f"rm -rf {BENCH}/run {BENCH}/run.log {REMOTE}/next-run.sh")
    return 0


sys.exit(main())

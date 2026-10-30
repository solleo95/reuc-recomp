"""First-scene FPS on the Steam Deck in Game Mode, one run per build (the owner watches it on the Deck).

  python scripts/linux/deck_perf.py [--seconds 10] uc uc_tune ...

Each name is a binary next to the staged game on the Deck (~/uc-run/<name>; `uc` is the staged build). The harness plays the menus at x8, drops to x1 in gameplay and measures;
the result is the mean of the game's own once-a-second `[fps]` lines of the measured window (finer than the harness's 50 ms windows, which are quantised to the display's rhythm).
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LINE = re.compile(r"^\[fps\] window_ms ([0-9.]+) retraces \d+:\d+ presents (\d+)", re.MULTILINE)


def newest_run():
    runs = sorted((ROOT / "build" / "autotest").iterdir(), key=lambda p: p.stat().st_mtime)
    return runs[-1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--game-arg", action="append", default=[], help="a game argument for every run (repeat it for a value: --game-arg=--guest-cpu --game-arg=2)")
    parser.add_argument("builds", nargs="+", help="BUILD or BUILD|--arg|value: arguments for that run only")
    args = parser.parse_args()
    env = dict(os.environ, UC_DECK_GAMEMODE="1")
    for spec in args.builds:
        name, *own = spec.split("|")
        env["UC_EXE"] = f"./{name}"
        extra = [f"--game-arg={a}" for a in args.game_arg + own]
        result = subprocess.run([sys.executable, "-m", "uctest", "run", "perf", "--measure", str(args.seconds), "--deck", *extra], cwd=ROOT / "scripts", env=env, capture_output=True, text=True)
        run = newest_run()
        if not (run / "run.log").exists():
            print(f"{spec}: the game did not start: " + " | ".join((result.stdout + result.stderr).strip().splitlines()[-3:]))
            continue
        rows = [(float(ms), int(p)) for ms, p in LINE.findall((run / "run.log").read_text(errors="replace"))][-args.seconds:]
        if not rows or "PASS" not in result.stdout:
            print(f"{name}: the run failed ({run})")
            continue
        fps = [1000 * p / ms for ms, p in rows]
        print(f"{spec}: {sum(1000 * p for _, p in rows) / sum(ms for ms, _ in rows):.1f} FPS over {len(rows)} s (lowest second {min(fps):.0f}, highest {max(fps):.0f})", flush=True)


main()

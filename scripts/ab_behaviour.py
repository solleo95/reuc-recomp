"""Record enemy behaviour of one scene at 30 Hz and at a high update rate for scripts/ab_compare.py.

python ab_behaviour.py --scene train --chapter 1 --lead 60 --seconds 40 --hz 30 480 [--from after-ch1]
python ab_behaviour.py --scene episode --lead 0 --seconds 40 --hz 30 480

The harness rules apply: 1280x720 window, copied saves, muted, god mode, no system input. The scene is reached
deterministically at x8 (same lead in game seconds for every rate), then recorded at speed 1 with fps limit = hz while
the autoplay bot keeps playing. Each run writes build/autotest/ab-<scene><chapter>-<hz>-<time>/states.jsonl.
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from uctest import autoplay, scenarios, steps  # noqa: E402
from uctest.client import Game, resolve_saves  # noqa: E402


def start_episode(g):
    """Episode start used by the earlier monkey runs (build/episode_probe.py): checkpoint dialog, chapter 0."""
    steps.skip_movies(g)
    steps.select(g, "title_start")
    for _ in range(30):
        g.press("A", 2, x=-0.425, y=-0.05)
        g.advance(30)
        if g.frame.get("menu", [0, 0, 0, 0])[3] != 0:
            break
    else:
        g.fail("checkpoint dialog did not appear")
    for _ in range(30):
        g.press("A", 2, x=-0.35, y=0.2)
        g.advance(30)
        if g.frame.get("ui", [-1] * 6)[5] != 3:
            break
    else:
        g.fail("checkpoint dialog did not close")
    steps.wait_gameplay(g)


def record(args, hz):
    run = ROOT / "build" / "autotest" / f"ab-{args.scene}{args.chapter}-{hz}-{time.time_ns()}"
    saves = resolve_saves(args.from_) if args.from_ else None
    with Game(exe=args.exe, run_dir=run, speed=8, god=True, skip_tutorials=True, saves=saves, seconds=1800,
              game_args=["--deterministic", "--render-scale", "1", "--fps-limit", "120"]) as g:
        if args.scene == "episode":
            start_episode(g)
        else:
            scenarios.reach_gameplay(g, args.chapter)
        bot = autoplay.Autoplay(g, run / "autoplay.jsonl", speed=8, god=True)
        lead_end = g.frame["retraces"] + args.lead * 60
        while g.frame["retraces"] < lead_end:
            if bot.step():
                break
        g.set_speed(1)
        g.set_options(fps_limit=hz)
        g.wait(lambda f: f.get("speed") == 1, seconds=30)
        start = g.frame["retraces"]
        next_sample = start
        with (run / "states.jsonl").open("w") as out:
            while g.frame["retraces"] < start + args.seconds * 60:
                reason = bot.step()
                if g.frame["retraces"] >= next_sample:
                    out.write(json.dumps(g.frame) + "\n")
                    next_sample = g.frame["retraces"] + 3
                if reason:
                    out.write(json.dumps({"stop": reason}) + "\n")
                    break
        print(hz, run, flush=True)
    return run


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--scene", choices=["train", "episode"], default="train")
    p.add_argument("--chapter", type=int, default=1)
    p.add_argument("--from", dest="from_")
    p.add_argument("--lead", type=int, default=60, help="game seconds played at x8 before recording")
    p.add_argument("--seconds", type=int, default=40)
    p.add_argument("--hz", type=int, nargs="+", default=[30, 480])
    p.add_argument("--exe", default=os.environ.get("UC_PROBE_EXE", "dist/uc.exe"))
    args = p.parse_args()
    for hz in args.hz:
        record(args, hz)


if __name__ == "__main__":
    main()

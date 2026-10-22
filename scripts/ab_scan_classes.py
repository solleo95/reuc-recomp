"""Play the episode from the live saves at x8 and print when each enemy class first appears (game seconds after the checkpoint loaded).

python ab_scan_classes.py [--seconds 90] [--from <saves dir>]
Used to find the lead time for scripts/ab_behaviour.py. Saves are copied by the harness, never used in place.
"""
import argparse
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from uctest import autoplay  # noqa: E402
from uctest.client import Game, LIVE_SAVES  # noqa: E402
from ab_behaviour import start_episode  # noqa: E402

p = argparse.ArgumentParser()
p.add_argument("--seconds", type=int, default=90)
p.add_argument("--from", dest="from_", default=str(LIVE_SAVES))
p.add_argument("--exe", default="dist/uc.exe")
a = p.parse_args()
run = ROOT / "build" / "autotest" / f"ab-scan-{time.time_ns()}"
with Game(exe=a.exe, run_dir=run, speed=8, god=True, skip_tutorials=True, saves=Path(a.from_), seconds=1800,
          game_args=["--deterministic", "--render-scale", "1", "--fps-limit", "0"]) as g:
    start_episode(g)
    bot = autoplay.Autoplay(g, run / "autoplay.jsonl", speed=8, god=True)
    start = g.frame["retraces"]
    seen = {}
    while g.frame["retraces"] < start + a.seconds * 60:
        reason = bot.step()
        t = (g.frame["retraces"] - start) / 60
        for e in g.frame.get("enemies", []):
            if e["cls"] not in seen and e.get("hp", 0) != 0:
                seen[e["cls"]] = t
                print(f"{t:6.1f}s class {e['cls']:#010x} id {e['id']} hp {e['hp']} root {[round(v) for v in e['root']]} {g.capture()}", flush=True)   # the screenshot tells what the class is
        if reason:
            print("bot stopped:", reason)
            break
    print(run)

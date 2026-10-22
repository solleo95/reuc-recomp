"""Find where an actor class keeps its position: play to the first sighting of the class, then dump the actor object twice (0.5 s apart)
and print the float triples that changed smoothly (a moving position) next to the probe's root.

python ab_find_pos.py 0x803F7200 [--lead 140]
"""
import argparse
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from uctest import autoplay  # noqa: E402
from uctest.client import Game, LIVE_SAVES  # noqa: E402
from ab_behaviour import start_episode  # noqa: E402

p = argparse.ArgumentParser()
p.add_argument("cls", type=lambda v: int(v, 0))
p.add_argument("--lead", type=int, default=140)
p.add_argument("--size", type=int, default=0x2400)
a = p.parse_args()
run = ROOT / "build" / "autotest" / f"ab-findpos-{time.time_ns()}"


def peek(g, addr, size):
    return bytes.fromhex(g.request({"cmd": "peek", "addr": addr, "size": size})["hex"])


with Game(exe="dist/uc.exe", run_dir=run, speed=8, god=True, skip_tutorials=True, saves=LIVE_SAVES, seconds=900,
          game_args=["--deterministic", "--render-scale", "1", "--fps-limit", "0"]) as g:
    start_episode(g)
    bot = autoplay.Autoplay(g, run / "autoplay.jsonl", speed=8, god=True, aim_only=[a.cls])
    start = g.frame["retraces"]
    hit = None
    while g.frame["retraces"] < start + a.lead * 60 and not hit:
        bot.step()
        hit = next((e for e in g.frame.get("enemies", []) if e["cls"] == a.cls and e.get("hp", 1) > 0), None)
    if not hit:
        sys.exit("class never seen")
    g.set_speed(1)
    g.wait(lambda f: f.get("speed") == 1, seconds=30)
    table = peek(g, 0x804421C4, 8 * 200)
    obj = next(struct.unpack(">I", table[i * 8 + 4:i * 8 + 8])[0] for i in range(200) if struct.unpack(">I", table[i * 8:i * 8 + 4])[0] == hit["id"])
    print("class", hex(a.cls), "id", hit["id"], "object", hex(obj), "probe root", hit["root"], g.capture())
    shots = []
    for _ in range(3):
        shots.append((g.frame["retraces"], peek(g, obj, a.size), next(e["root"] for e in g.frame["enemies"] if e["id"] == hit["id"])))
        t = g.frame["retraces"]
        while g.frame["retraces"] < t + 20:
            bot.step()
    f = lambda blob, o: struct.unpack(">f", blob[o:o + 4])[0]
    first, last = shots[0][1], shots[-1][1]
    print("offsets whose xyz moved smoothly (off: first -> last):")
    for o in range(0, a.size - 12, 4):
        v0 = [f(first, o + 4 * k) for k in range(3)]
        v1 = [f(last, o + 4 * k) for k in range(3)]
        mid = [f(shots[1][1], o + 4 * k) for k in range(3)]
        if all(abs(x) < 5000 and x == x for x in v0 + v1) and any(abs(x - y) > 0.5 for x, y in zip(v0, v1)) and all(abs(x) > 1e-3 or True for x in v0):
            if all(min(x, y) - 0.01 <= m <= max(x, y) + 0.01 for x, y, m in zip(v0, v1, mid)):
                print(f"  +{o:#06x}: {[round(x, 1) for x in v0]} -> {[round(x, 1) for x in v1]}")

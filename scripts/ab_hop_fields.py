"""Which fields of an actor move while it hops, and by how much: the same scene as ab_behaviour (episode, aim-only enemies with id >= --min-id),
but the actor object is dumped ~20 times per game second and every float triple that moved is ranked by its range.

python ab_hop_fields.py --hz 30 --min-id 77 [--seconds 6] [--live]
Prints the position the probe reads (+0xF48) and the other triples whose range is above 2 units. Compare the output at 30 and 120 Hz.
"""
import argparse
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from uctest import autoplay, vision  # noqa: E402
from uctest.client import Game, LIVE_SAVES  # noqa: E402
from ab_behaviour import start_episode  # noqa: E402

CLS = 0x803F2B60
p = argparse.ArgumentParser()
p.add_argument("--hz", type=int, default=120)
p.add_argument("--min-id", type=int, default=77)
p.add_argument("--seconds", type=int, default=6)
p.add_argument("--size", type=int, default=0x2400)
p.add_argument("--live", action="store_true")
a = p.parse_args()
vision.HEIGHT_OVERRIDE[CLS] = 8.0
run = ROOT / "build" / "autotest" / f"ab-hopfields-{a.hz}-{time.time_ns()}"


def peek(g, addr, size):
    return bytes.fromhex(g.request({"cmd": "peek", "addr": addr, "size": size})["hex"])


with Game(exe="dist/uc.exe", run_dir=run, speed=8, god=True, skip_tutorials=True, saves=LIVE_SAVES, seconds=900,
          game_args=[*([] if a.live else ["--deterministic"]), "--render-scale", "1", "--fps-limit", "120"]) as g:
    start_episode(g)
    bot = autoplay.Autoplay(g, run / "autoplay.jsonl", speed=8, god=True, aim_only=[CLS])
    bot.aim_min_id = a.min_id
    hit = None
    while not hit:
        bot.step()
        hit = next((e for e in g.frame.get("enemies", []) if e["cls"] == CLS and e["id"] >= a.min_id and e.get("hp", 1) > 0), None)
    g.set_speed(1)
    g.set_options(fps_limit=a.hz)
    g.wait(lambda f: f.get("speed") == 1, seconds=30)
    table = peek(g, 0x804421C4, 8 * 200)
    obj = next(struct.unpack(">I", table[i * 8 + 4:i * 8 + 8])[0] for i in range(200) if struct.unpack(">I", table[i * 8:i * 8 + 4])[0] == hit["id"])
    print("hz", a.hz, "id", hit["id"], "object", hex(obj), flush=True)
    start = g.frame["retraces"]
    last_n, dumps = None, []
    while g.frame["retraces"] < start + a.seconds * 60:
        bot.step()
        if g.frame["n"] != last_n:   # one dump per game frame
            last_n = g.frame["n"]
            dumps.append((g.frame["retraces"], peek(g, obj, a.size)))
    print("dumps", len(dumps))
    import pickle
    (run / "dumps.pkl").write_bytes(pickle.dumps({"hz": a.hz, "obj": obj, "dumps": dumps}))
    print(run)

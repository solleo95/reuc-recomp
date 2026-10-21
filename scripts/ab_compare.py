"""Compare enemy behaviour per actor class between two states.jsonl recordings (low rate first).

python ab_compare.py <run 30 Hz> <run 480 Hz>

Per class: samples, enemies, median and p90 horizontal speed while alive (units/s), airborne fraction, median distance
to the camera, kills per minute. A class is flagged when a speed ratio leaves [0.75, 1.33] or the airborne fraction
or kill rate differs by more than 2x with enough samples.
"""
import json
import math
import sys
from collections import defaultdict


def load(run):
    frames = []
    for line in open(f"{run}/states.jsonl", encoding="utf-8"):
        f = json.loads(line)
        if "retraces" in f:
            frames.append(f)
    return frames


def camera(view):
    return [-sum(view[r * 4 + i] * view[r * 4 + 3] for r in range(3)) for i in range(3)] if view else None


def metrics(frames):
    by_class = defaultdict(lambda: {"speeds": [], "air": 0, "samples": 0, "dist": [], "ids": set(), "kills": 0})
    last = {}
    heights = defaultdict(list)
    for f in frames:
        for e in f.get("enemies", []):
            heights[e["id"]].append(e["root"][1])
    floor = {i: sorted(h)[len(h) // 10] for i, h in heights.items()}   # 10th percentile: the ground the enemy walks on
    duration = (frames[-1]["retraces"] - frames[0]["retraces"]) / 60 if len(frames) > 1 else 0
    for f in frames:
        cam = camera(f.get("view"))
        for e in f.get("enemies", []):
            m = by_class[e["cls"]]
            x, y, z = e["root"]
            alive = e.get("hp", 1) > 0
            prev = last.get(e["id"])
            if prev and prev[3] and not alive:
                m["kills"] += 1
            if alive:
                m["samples"] += 1
                m["ids"].add(e["id"])
                m["air"] += y > floor[e["id"]] + 2.0
                if cam:
                    m["dist"].append(math.hypot(x - cam[0], z - cam[2]))
                if prev and prev[3] and 0 < f["retraces"] - prev[4] <= 6:
                    m["speeds"].append(math.hypot(x - prev[0], z - prev[2]) / ((f["retraces"] - prev[4]) / 60))
            last[e["id"]] = (x, y, z, alive, f["retraces"])
    out = {}
    for cls, m in by_class.items():
        s = sorted(m["speeds"])
        q = lambda p: s[min(len(s) - 1, int(p * len(s)))] if s else 0.0
        d = sorted(m["dist"])
        out[cls] = {"samples": m["samples"], "enemies": len(m["ids"]), "speed_med": q(0.5), "speed_p90": q(0.9),
                    "air": m["air"] / m["samples"] if m["samples"] else 0.0, "dist_med": d[len(d) // 2] if d else 0.0,
                    "kills_min": m["kills"] / duration * 60 if duration else 0.0}
    return out


def ratio(a, b):
    return b / a if a > 1e-6 else (math.inf if b > 1e-6 else 1.0)


def main():
    low, high = metrics(load(sys.argv[1])), metrics(load(sys.argv[2]))
    print(f"{'class':10} {'samples':>13} {'speed med':>17} {'speed p90':>17} {'air':>11} {'dist med':>13} {'kills/min':>11}  flag")
    flagged = 0
    for cls in sorted(set(low) | set(high)):
        a, b = low.get(cls), high.get(cls)
        if not a or not b:
            print(f"{cls:#010x} only in {'high' if b else 'low'} run ({(b or a)['samples']} samples)")
            continue
        flags = []
        if min(a["samples"], b["samples"]) >= 60:
            for key in ("speed_med", "speed_p90"):
                r = ratio(a[key], b[key])
                if not 0.75 <= r <= 1.33 and max(a[key], b[key]) > 0.5:
                    flags.append(f"{key} x{r:.2f}")
            if max(a["air"], b["air"]) > 0.02 and not 0.5 <= ratio(a["air"], b["air"]) <= 2:
                flags.append(f"air x{ratio(a['air'], b['air']):.2f}")
            if max(a["kills_min"], b["kills_min"]) > 2 and not 0.5 <= ratio(a["kills_min"], b["kills_min"]) <= 2:
                flags.append(f"kills x{ratio(a['kills_min'], b['kills_min']):.2f}")
        flagged += bool(flags)
        print(f"{cls:#010x} {a['samples']:>6}/{b['samples']:<6} {a['speed_med']:>7.2f}/{b['speed_med']:<8.2f} "
              f"{a['speed_p90']:>7.2f}/{b['speed_p90']:<8.2f} {a['air']:>5.2f}/{b['air']:<5.2f} "
              f"{a['dist_med']:>6.1f}/{b['dist_med']:<6.1f} {a['kills_min']:>5.1f}/{b['kills_min']:<5.1f}  {'; '.join(flags)}")
    print(f"flagged classes: {flagged}")


if __name__ == "__main__":
    main()

"""The measurement protocol for performance changes on the Steam Deck (docs/testing/performance-measurement.md).

Quick check, the default: one run before and one run after (the run before is remembered per binary and not repeated):
  python scripts/linux/measure.py BEFORE AFTER           # two uc binaries, or labels of build/layouts (LABEL = build/layouts/LABEL_s1)
It judges the work: guest instructions over the deterministic demo window, exact to 0.0013%, the same in every code layout. LESS-WORK keeps a change, MORE-WORK rejects
it, SAME-WORK says nothing about speed. The cycles and wall time of the pair are printed for information; the time is judged only if it is worse by more than a code
layout can explain (CHECK-TIME), then the full protocol decides.

Full protocol, only when a time claim is needed (K layouts, both builds in each, the change of every layout is paired):
  python scripts/linux/measure.py BEFORE AFTER --full [--seeds 1,2,3,4,5,6] [--runs 1]
with build/layouts/BEFORE_s1.. and AFTER_s1.. made by scripts/linux/relink_layouts.py. The report has the change in instructions, cycles and wall time with 95% intervals over the
layouts and the verdict. The raw numbers go to build/measure/<name>.json.
"""
import argparse
import hashlib
import json
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import deck_bench   # noqa: E402
import perf_stats   # noqa: E402

ROOT = deck_bench.ROOT
OUT = ROOT / "build" / "measure"


def resolve(side, seed):
    """A file path, or a label of build/layouts."""
    if Path(side).is_file():
        return Path(side)
    path = ROOT / "build" / "layouts" / f"{side}_s{seed}"
    if not path.exists():
        sys.exit(f"missing {path} (scripts/linux/relink_layouts.py {side} {seed}), and {side} is no file")
    return path


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()[:20]


def measure_runs(entries, demo, window, game_args, repeat, standalone=True):
    """entries: [(label, path[, ["env:NAME=value", ...]])] in the order to run. -> {label: record}; one run each (the pairs alternate order by the caller)."""
    extras = {entry[0]: list(entry[2]) if len(entry) > 2 else [] for entry in entries}
    builds = {entry[0]: str(entry[1]) for entry in entries}
    if standalone:
        deck_bench.prepare(demo, builds)   # pushes the binaries; the full protocol does it once for all layouts
    expected = (window["to"] - window["from"]) // 60
    records, reference = {}, None
    for entry in entries:
        label = entry[0]
        rows, log = deck_bench.one_run(label, demo, window, game_args + extras[label], repeat)
        if len(rows) < expected - 1:
            sys.exit(f"{label}: only {len(rows)} of {expected} game seconds measured\n" + "\n".join(log.splitlines()[-8:]))
        work = [(r[0], r[1]) for r in rows]
        reference = reference or work
        if work != reference:
            sys.exit(f"INVALID: the guest work of {label} differs from the first run (retrace or draw counts)")
        record = {"instructions": sum(r[4] for r in rows) * 1e6, "cycles": sum(r[5] for r in rows) * 1e6, "serial_ms": sum(r[6] for r in rows), "wall_ms": sum(r[3] for r in rows), "work": [list(w) for w in work]}   # lists: what a cached record gives back from JSON
        if not record["instructions"]:
            sys.exit("no hardware counters in the log (UC_PERF_COUNTERS; /proc/sys/kernel/perf_event_paranoid on the Deck must be 2 or lower)")
        records[label] = record
        print(f"{label}: {record['instructions'] / 1e6:.1f} Minstr, {record['cycles'] / 1e6:.1f} Mcycles, {record['wall_ms'] / 1000:.3f} s wall", flush=True)
    if standalone:
        clean(builds)
    return records


def clean(builds):
    deck_bench._ssh(f"cd {deck_bench.REMOTE} && rm -f " + " ".join(f"uc_{label}" for label in builds) + f"; rm -rf {deck_bench.BENCH}/run {deck_bench.BENCH}/run.log {deck_bench.REMOTE}/next-run.sh")


def quick(args, window):
    cache = OUT / "cache"
    cache.mkdir(parents=True, exist_ok=True)
    sides = {"before": resolve(args.a, 1), "after": resolve(args.b, 1)}
    env = {"before": [f"env:{e}" for e in args.env_before], "after": [f"env:{e}" for e in args.env_after]}   # the same binary with a different setting is a different run
    tag = {name: hashlib.sha256("|".join(env[name]).encode()).hexdigest()[:6] if env[name] else "" for name in sides}
    key = {name: f"{digest(path)}{tag[name]}-{args.demo}-{args.seconds}" for name, path in sides.items()}
    have = {name: json.loads((cache / f"{key[name]}.json").read_text()) for name in sides if (cache / f"{key[name]}.json").exists()}
    todo = [(name, sides[name], env[name]) for name in ("before", "after") if name not in have]
    for name in have:
        print(f"{name}: reused from the earlier run of this binary ({key[name]})")
    if todo:
        fresh = measure_runs(todo, args.demo, window, args.game_arg, 0)
        for name, record in fresh.items():
            (cache / f"{key[name]}.json").write_text(json.dumps(record))
            have[name] = record
    if have["before"]["work"] != have["after"]["work"]:
        print("INVALID: the guest work of the two runs differs (retrace or draw counts): not the same demo window")
        return 1
    code, text = perf_stats.work_verdict(have["before"]["instructions"], have["after"]["instructions"])
    cycles = have["after"]["cycles"] / have["before"]["cycles"] - 1
    wall = have["after"]["wall_ms"] / have["before"]["wall_ms"] - 1
    serial = have["after"].get("serial_ms", 0) / have["before"]["serial_ms"] - 1 if have["before"].get("serial_ms") else 0.0
    print(f"information only (one pair, one code layout: layout alone moves it by about +-1%): cycles {100 * cycles:+.2f}%, guest serial time {100 * serial:+.2f}%, wall {100 * wall:+.2f}%")
    flag = perf_stats.time_flag(cycles)
    if flag:
        code, text = "CHECK-TIME", f"{text}; {flag}"
    print(f"VERDICT {code}: {text}")
    return 0


def full(args, window):
    seeds = [int(s) for s in args.seeds.split(",")]
    paths = {(side, seed): resolve(side, seed) for seed in seeds for side in (args.a, args.b)}
    builds = {f"{side}s{seed}": str(path) for (side, seed), path in paths.items()}
    deck_bench.prepare(args.demo, builds)
    raw = []
    for index, seed in enumerate(seeds):
        for run in range(args.runs):
            order = [args.a, args.b] if (index + run) % 2 == 0 else [args.b, args.a]
            for side in order:
                label = f"{side}s{seed}"
                record = measure_runs([(label, paths[(side, seed)])], args.demo, window, args.game_arg, run, standalone=False)[label]
                raw.append({"side": side, "seed": seed, "run": run, **{k: v for k, v in record.items() if k != "work"}})

    clean(builds)

    def by_seed(side, key):
        return {seed: [r[key] for r in raw if r["side"] == side and r["seed"] == seed] for seed in seeds}
    report = {"a": args.a, "b": args.b, "seeds": seeds, "runs": args.runs, "demo": args.demo, "seconds": args.seconds, "raw": raw}
    bad = [(side, s) for side in (args.a, args.b) for s in perf_stats.invalid_runs(by_seed(side, "instructions"))]
    changes = {key: perf_stats.paired_change(by_seed(args.a, key), by_seed(args.b, key)) for key in ("instructions", "cycles", "serial_ms", "wall_ms")}
    for key, change in changes.items():
        print(f"{key:13s} {args.b} over {args.a}: {100 * change['change']:+.3f}% +-{100 * change['half']:.3f}% (95%, {change['seeds']} layouts; per layout "
              + ", ".join(f"{100 * v:+.2f}" for v in change["per_seed"].values()) + ")")
    code, text = ("INVALID", f"instruction counts not deterministic for {bad}") if bad else perf_stats.verdict(changes["instructions"], changes[args.time])
    print(f"VERDICT {code}: {text}")
    report["changes"] = {k: {"change": v["change"], "half": v["half"], "per_seed": v["per_seed"]} for k, v in changes.items()}
    report["verdict"] = {"code": code, "text": text}
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / f"{time.strftime('%Y%m%d-%H%M%S')}-{args.a}-vs-{args.b}{('-' + args.name) if args.name else ''}.json"
    path.write_text(json.dumps(report, indent=1))
    print(f"raw numbers: {path}")
    return 0 if code != "INVALID" else 1


def toggle(args, window):
    """The same binary with a setting changed (--env-before/--env-after): for changes that move work to helper threads, which the instruction counters of the guest threads do not
    see. Both arms are one file, so there is no code layout to average out; the arms alternate --repeat times and each repeat gives one paired ratio of the guest serial time (the wall
    time the baton was held), cycles and wall time."""
    path = resolve(args.a, 1)
    arms = {"A": [f"env:{e}" for e in args.env_before], "B": [f"env:{e}" for e in args.env_after]}
    builds = {"A": str(path), "B": str(path)}
    deck_bench.prepare(args.demo, builds)
    raw = []
    for repeat in range(args.repeat):
        for arm in (["A", "B"] if repeat % 2 == 0 else ["B", "A"]):
            record = measure_runs([(arm, path, arms[arm])], args.demo, window, args.game_arg, repeat, standalone=False)[arm]
            raw.append({"side": arm, "seed": repeat, "run": 0, **{k: v for k, v in record.items() if k != "work"}})
    clean(builds)

    def by_repeat(arm, key):
        return {r["seed"]: [r[key]] for r in raw if r["side"] == arm}
    changes = {key: perf_stats.paired_change(by_repeat("A", key), by_repeat("B", key)) for key in ("instructions", "cycles", "serial_ms", "wall_ms")}
    for key, change in changes.items():
        print(f"{key:13s} after over before: {100 * change['change']:+.3f}% +-{100 * change['half']:.3f}% (95%, {change['seeds']} repeats; per repeat " + ", ".join(f"{100 * v:+.2f}" for v in change["per_seed"].values()) + ")")
    if args.repeat >= perf_stats.MIN_SEEDS:
        code, text = perf_stats.verdict(changes["instructions"], changes[args.time])
    else:   # one pair of the same binary: only an effect beyond the run-to-run noise (below 1%, here 2.5%) is called
        change = changes[args.time]["change"]
        code = "FASTER" if change < -perf_stats.TIME_FLAG else "WORSE" if change > perf_stats.TIME_FLAG else "INCONCLUSIVE"
        text = f"{args.time} {100 * change:+.2f}% from {args.repeat} pair(s) of the same binary (the run-to-run noise is below 1%; a verdict from fewer than {perf_stats.MIN_SEEDS} repeats needs more than {100 * perf_stats.TIME_FLAG:.1f}%)"
    print(f"VERDICT {code}: {text}")
    OUT.mkdir(parents=True, exist_ok=True)
    path_out = OUT / f"{time.strftime('%Y%m%d-%H%M%S')}-toggle{('-' + args.name) if args.name else ''}.json"
    path_out.write_text(json.dumps({"env_before": args.env_before, "env_after": args.env_after, "raw": raw, "changes": {k: {"change": v["change"], "half": v["half"]} for k, v in changes.items()}, "verdict": [code, text]}, indent=1))
    print(f"raw numbers: {path_out}")
    return 0 if code != "INVALID" else 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("a", help="before: a uc binary, or a label of build/layouts"), parser.add_argument("b", help="after")
    parser.add_argument("--full", action="store_true", help="the full protocol over K code layouts (a time claim)")
    parser.add_argument("--env-before", action="append", default=[], metavar="NAME=VALUE", help="an environment variable of the game for the before run (quick check: the same binary with a setting changed)")
    parser.add_argument("--env-after", action="append", default=[], metavar="NAME=VALUE")
    parser.add_argument("--toggle", action="store_true", help="the same binary (a) with --env-before against --env-after, --repeat times: work moved to helper threads (time metric serial_ms)")
    parser.add_argument("--repeat", type=int, default=1, help="--toggle: pairs of runs (one is enough for an effect of several percent; the noise of one pair of the same binary is below 1%)")
    parser.add_argument("--seeds", default="1,2,3,4,5,6")
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--demo", default="fight4")
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--time", default="cycles", choices=["cycles", "serial_ms"], help="the time metric of the --full verdict: cycles (CPU work of the guest threads), or serial_ms (the wall time the baton was held: for changes that move work to helper threads, which the counters do not see)")
    parser.add_argument("--name", default="")
    parser.add_argument("--game-arg", action="append", default=[])
    args = parser.parse_args()
    window = json.loads((ROOT / "build" / "demos" / f"{args.demo}.json").read_text())
    window["to"] = min(window["to"], window["from"] + 60 * args.seconds)
    if args.toggle:
        args.time = "serial_ms"
        return toggle(args, window)
    return full(args, window) if args.full else quick(args, window)


sys.exit(main())

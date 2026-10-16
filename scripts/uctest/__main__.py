"""python -m uctest list | run <scenario> [--speed k] [--measure s] [--repeat n]"""
import argparse
import json
import shutil
import sys
import time

from .client import CHECKPOINTS, Game, StepFailed, resolve_saves
from .scenarios import SCENARIOS


def run_once(name, args):
    g = Game(**({"exe": args.exe} if args.exe else {}), speed=args.speed, god=args.god, skip_tutorials=args.skip_tutorials or name == "fight", saves=resolve_saves(args.saves_from), game_args=args.game_arg)
    start = time.monotonic()
    report = {"scenario": name, "ok": False, "speed": args.speed, "god": args.god}
    try:
        with g:
            report["result"] = SCENARIOS[name](g, args)
        report["ok"] = True
    except StepFailed as error:
        report["error"] = str(error)
        report["artifacts"] = error.artifacts
    if args.save_as and (g.run_dir / "saves").is_dir():   # the game is closed: its saves (progress so far) become a reusable starting point
        shutil.copytree(g.run_dir / "saves", CHECKPOINTS / args.save_as)
        report["checkpoint"] = str(CHECKPOINTS / args.save_as)
    report["seconds"] = round(time.monotonic() - start, 1)
    report["steps"] = g.steps
    report["counters"] = dict(g.counters)
    (g.run_dir / "report.json").write_text(json.dumps(report, indent=1, default=str))
    return report, g.run_dir


def main(argv=None):
    parser = argparse.ArgumentParser(prog="uctest")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("list")
    run = sub.add_parser("run")
    run.add_argument("scenario", choices=sorted(SCENARIOS))
    run.add_argument("--speed", type=int, default=1, choices=range(1, 33), metavar="1..32")
    run.add_argument("--measure", type=float, default=0, help="seconds of FPS measurement (perf)")
    run.add_argument("--exe", help="game executable to launch instead of dist/uc.exe (e.g. an instrumented build staged next to dist/data)")
    run.add_argument("--repeat", type=int, default=1)
    run.add_argument("--god", action="store_true", help="cheat: health and ammo stay at their maxima (marked in the report)")
    run.add_argument("--skip-tutorials", action="store_true", help="tutorial pages never open (the game's own tutorial opener is turned into a no-op)")
    run.add_argument("--from", dest="saves_from", help="start from a checkpoint name or a saves directory instead of the real saves (copied, never modified)")
    run.add_argument("--game-arg", action="append", default=[], help="extra uc.exe argument, repeatable (e.g. --game-arg=--validation)")
    run.add_argument("--chapter", type=int, default=1, choices=range(1, 5), help="chapter to start (2..4 must be unlocked in the saves)")
    run.add_argument("--save-as", help="after the run, keep its saves as checkpoint NAME (build/autotest/checkpoints/NAME)")
    run.add_argument("--mark", action="store_true", help="fight: ignore the saved slowdown point and wait for F5 to set a new one")
    run.add_argument("--max-minutes", type=float, default=10, help="autoplay wall-time limit")
    bench = sub.add_parser("bench", help="compare game builds on the heavy fight: cycles per frame at one scene weight, alternating runs, 95%% CI (see uctest/bench.py)")
    bench.add_argument("--demo", help="replay this recorded demo (python -m uctest demo record NAME) instead of playing the fight live: identical guest work in every run")
    bench.add_argument("exes", nargs="+", help="uc.exe builds to compare, each staged next to the data folders; the first is the baseline")
    bench.add_argument("--runs", type=int, default=0, help="rounds; every round runs each build once (default 1 with --demo: one pair screens differences above ~5%%, 3 rounds in alternating order resolve ~2%%; 3 without --demo)")
    bench.add_argument("--measure", type=int, default=20, help="seconds of the fight that count")
    bench.add_argument("--max-minutes", type=float, default=10)
    bench.add_argument("--guest-cpu", type=int, default=4, help="logical CPU the guest threads are pinned to (its SMT sibling cpu^1 is kept free); -1 leaves placement to Windows")
    bench.add_argument("--game-arg", action="append", default=[])
    demo = sub.add_parser("demo", help="record / check a deterministic demo of the heavy fight (see uctest/demo.py)")
    demo.add_argument("action", choices=["record", "check", "profile", "replay"])
    demo.add_argument("name")
    demo.add_argument("--exe", help="build to use (default dist/uc.exe)")
    demo.add_argument("--measure", type=int, default=20, help="seconds of idle fight after the slowdown point that the demo measures")
    demo.add_argument("--max-minutes", type=float, default=10)
    demo.add_argument("--trace", action="store_true", help="record: also hash guest memory at every retrace so `demo check` can compare the replay with the recording (slows the recording run about 5x)")
    inspect = sub.add_parser("inspect", help="read a crash bundle (see uctest/dumpview.py)")
    inspect.add_argument("bundle", help="crash-*.txt report or its .dump folder")
    inspect.add_argument("what", choices=["threads", "stack", "mem", "f32", "nan", "posmtx", "gx"])
    inspect.add_argument("arguments", nargs="*")
    args = parser.parse_args(argv)
    if args.command == "inspect":
        from . import dumpview
        return dumpview.command(dumpview.Bundle(args.bundle), args.what, args.arguments)
    if args.command == "demo":
        from . import demo as demo_module
        args.exe = args.exe or str(demo_module.ROOT / "dist" / "uc.exe")
        return {"record": lambda: demo_module.record(args, run_once), "check": lambda: demo_module.check(args), "profile": lambda: demo_module.profile(args), "replay": lambda: demo_module.replay(args)}[args.action]()
    if args.command == "bench" and not args.runs:
        args.runs = 1 if args.demo else 3
    if args.command == "bench" and args.demo:
        from . import demo as demo_module
        return demo_module.bench(args)
    if args.command == "bench":
        from . import bench as bench_module
        return bench_module.command(args, run_once)
    if args.command == "list":
        print("\n".join(sorted(SCENARIOS)))
        return 0
    failures = 0
    for index in range(args.repeat):
        report, run_dir = run_once(args.scenario, args)
        failures += not report["ok"]
        detail = "" if report["ok"] else f" - {report['error']}"
        series = (report.get("result") or {}).get("series", [])
        for second in sorted({int(row["t"]) for row in series}):   # one line per second from the 50 ms windows; full data in <run>/fps50.csv
            rows = [row for row in series if int(row["t"]) == second]
            rates = [row["fps"] for row in rows]
            gaps = [row["gap_ms"] for row in rows if row["gap_ms"] is not None]
            print(f"  t={second:3d}s fps avg {sum(rates) / len(rates):6.1f} min {min(rates):6.1f} max {max(rates):6.1f}  worst gap {max(gaps) if gaps else 0:5.1f} ms  {'/'.join(sorted({row['state'] for row in rows}))}", flush=True)
        result = report.get("result") or {}
        dips = sorted((row for row in series if row["state"] == "active" and row["gap_ms"]), key=lambda row: -row["gap_ms"])[:6]
        if dips:
            print("  worst frames: " + "  ".join(f"t={row['t']:.2f}s {row['gap_ms']:.1f}ms" for row in dips), flush=True)
        if "fps_p5" in result:
            print(f"  ACTIVE gameplay ({result['samples']} windows of 50 ms): mean {result['fps_mean']:.1f}  p50 {result['fps_p50']:.1f}  p5 {result['fps_p5']:.1f}  min {result['fps_min']:.1f}  frame gap p99 {result['gap_p99_ms']:.1f} ms  max {result['gap_max_ms']:.1f} ms  (non-active windows: {result['non_active_windows']})", flush=True)
        print(f"{args.scenario} {index + 1}/{args.repeat} {'PASS' if report['ok'] else 'FAIL'} {report['seconds']}s {run_dir}{detail}", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

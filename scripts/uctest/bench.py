"""python -m uctest bench <exe> <exe> [...] [--runs 3] [--measure 20]

Compares game builds on the heavy train fight with the noise taken out as far as it can be without a deterministic replay:
 - the runs alternate A B A B ..., so slow drift of the machine (clocks, background load) hits every build alike;
 - the cost is the CPU cycles the guest threads ran per presented frame (the game's `[bench]` line, QueryThreadCycleTime): no waits, no FPS cap;
 - the fight changes weight from second to second, so every run fits cost = a + b * draws_per_frame over its seconds and the builds are compared
   at one common scene weight (the median over all runs);
 - the verdict is a paired difference over the rounds with a 95% confidence interval; "no difference" means the interval contains 0.
"""
import math
import re
import statistics
import types

BENCH_LINE = re.compile(r"^\[bench\] presents (\d+) draws (\d+) guest_mcycles ([0-9.]+)")
# two-sided 95% Student t by degrees of freedom
T95 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365, 8: 2.306, 9: 2.262, 10: 2.228, 15: 2.131, 20: 2.086, 30: 2.042}


def t95(dof):
    keys = sorted(T95)
    return T95[next((k for k in keys if k >= dof), keys[-1])] if dof < keys[-1] else 1.96


def read_seconds(run_dir, measure):
    """The last `measure` complete seconds of the run: (frames, draws, guest Mcycles) per second."""
    rows = []
    for line in (run_dir / "run.log").read_text(errors="replace").splitlines():
        m = BENCH_LINE.match(line)
        if m and int(m[1]) > 0:
            rows.append((int(m[1]), int(m[2]), float(m[3])))
    if len(rows) > measure + 1:
        rows = rows[-measure - 1:-1]   # the game closes within a second of the window ending: the last record is partial
    return rows


def fit(rows):
    """cost per frame as a line in draws per frame: (intercept, slope, mean load); the slope is 0 when the scene weight hardly varied."""
    x = [c / f for f, d, c in rows]
    y = [d / f for f, d, c in rows]
    mean_y, mean_x = statistics.fmean(y), statistics.fmean(x)
    spread = statistics.pstdev(y)
    if len(rows) < 8 or spread < 0.03 * mean_y:
        return mean_x, 0.0, mean_y
    slope = sum((a - mean_y) * (b - mean_x) for a, b in zip(y, x)) / sum((a - mean_y) ** 2 for a in y)
    return mean_x - slope * mean_y, slope, mean_y


def analyse(per_exe):
    """per_exe: {name: [rows of run 1, rows of run 2, ...]} -> text report."""
    loads = [d / f for runs in per_exe.values() for rows in runs for f, d, c in rows]
    reference = statistics.median(loads)
    names = list(per_exe)
    cost = {name: [] for name in names}
    fps, weight = {name: [] for name in names}, {name: [] for name in names}
    for name, runs in per_exe.items():
        for rows in runs:
            a, b, mean_load = fit(rows)
            cost[name].append(a + b * reference)
            fps[name].append(sum(f for f, d, c in rows) / len(rows))
            weight[name].append(mean_load)
    lines = [f"scene weight for the comparison: {reference:.0f} draws per frame (median of {len(loads)} seconds)"]
    for name in names:
        c = cost[name]
        sd = statistics.stdev(c) if len(c) > 1 else 0.0
        lines.append(f"  {name}: {statistics.fmean(c):.3f} Mcycles/frame (sd between runs {100 * sd / statistics.fmean(c):.1f}%)"
                     f"  FPS {' '.join(f'{v:.0f}' for v in fps[name])}  draws/frame {' '.join(f'{v:.0f}' for v in weight[name])}")
    everything = [w for ws in weight.values() for w in ws]
    if max(everything) > 1.12 * min(everything):
        lines.append(f"  WARNING: the scene weight differs by {100 * (max(everything) / min(everything) - 1):.0f}% between runs; the regression corrects for it but a smaller spread is safer")
    base = names[0]
    for name in names[1:]:
        diffs = [100 * (b / a - 1) for a, b in zip(cost[base], cost[name])]   # negative: `name` needs fewer cycles per frame
        mean = statistics.fmean(diffs)
        half = t95(len(diffs) - 1) * statistics.stdev(diffs) / math.sqrt(len(diffs)) if len(diffs) > 1 else float("inf")
        verdict = "no difference" if abs(mean) <= half else ("FASTER" if mean < 0 else "SLOWER")
        lines.append(f"{name} vs {base}: {mean:+.1f}% cycles per frame, 95% CI [{mean - half:+.1f}%, {mean + half:+.1f}%] over {len(diffs)} paired runs -> {verdict}")
    return "\n".join(lines)


def command(args, run_once):
    from pathlib import Path
    exes = list(args.exes)
    per_exe = {exe: [] for exe in exes}
    for round_ in range(args.runs):
        for exe in exes:
            run = types.SimpleNamespace(exe=str(Path(exe).resolve()), speed=1, god=True, skip_tutorials=True, saves_from=None, save_as=None, chapter=1, mark=False,
                                        measure=args.measure, max_minutes=args.max_minutes, repeat=1,
                                        game_arg=["--game-hz", "480", "--profile"] + (["--guest-cpu", str(args.guest_cpu)] if args.guest_cpu >= 0 else []) + list(args.game_arg or []))
            report, run_dir = run_once("fight", run)
            if not report["ok"]:
                print(f"bench: run failed ({exe}): {report.get('error')}")
                return 1
            rows = read_seconds(run_dir, args.measure)
            if len(rows) < max(5, args.measure // 2):
                print(f"bench: only {len(rows)} [bench] records in {run_dir / 'run.log'} (does the exe print them? built before the bench line existed?)")
                return 1
            per_exe[exe].append(rows)
            print(f"bench: round {round_ + 1}/{args.runs} {Path(exe).name} {run_dir.name}", flush=True)
    print(analyse(per_exe))
    return 0

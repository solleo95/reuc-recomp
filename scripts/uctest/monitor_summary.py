"""Summarises a Deck monitor.csv (scripts/linux/deck_monitor.py): what the game used while it ran.

  python -m uctest.monitor_summary <monitor.csv> [from_s to_s]
"""
import csv
import statistics
import sys
from collections import Counter


def summarise(path, start=None, stop=None):
    rows = list(csv.DictReader(open(path)))
    rows = [r for r in rows if (start is None or float(r["t"]) >= start) and (stop is None or float(r["t"]) <= stop)]
    if not rows:
        return "no samples"
    num = lambda key: [float(r[key]) for r in rows if r[key] not in ("", None)]
    lines = [f"samples {len(rows)} over {float(rows[-1]['t']) - float(rows[0]['t']):.0f} s"]
    lines.append(f"game CPU {statistics.fmean(num('cpu_pct')):.0f}% of one core (8 threads = 800%), peak {max(num('cpu_pct')):.0f}%")
    names = Counter(r["top1"] for r in rows)
    lines.append("busiest thread: " + ", ".join(f"{name} {count * 100 // len(rows)}% of samples" for name, count in names.most_common(3))
                 + f"; its load mean {statistics.fmean(num('top1_pct')):.0f}% (a thread tops out at 100%), p95 {sorted(num('top1_pct'))[int(.95 * len(rows))]:.0f}%")
    lines.append(f"second thread mean {statistics.fmean(num('top2_pct')):.0f}%, third {statistics.fmean(num('top3_pct')):.0f}%")
    lines.append(f"CPU clock mean {statistics.fmean(num('cpu_mean_mhz')):.0f} MHz, highest core {statistics.fmean(num('cpu_max_mhz')):.0f} MHz (Deck APU: up to 3500)")
    if num("gpu_busy_pct"):
        lines.append(f"GPU busy mean {statistics.fmean(num('gpu_busy_pct')):.0f}%, p95 {sorted(num('gpu_busy_pct'))[int(.95 * len(rows))]:.0f}%")
    if num("gpu_sclk_mhz"):
        lines.append(f"GPU clock mean {statistics.fmean(num('gpu_sclk_mhz')):.0f} MHz (up to 1600)")
    if num("power_w"):
        lines.append(f"APU power mean {statistics.fmean(num('power_w')):.1f} W, peak {max(num('power_w')):.1f} W")
    return "\n".join(lines)


if __name__ == "__main__":
    arguments = sys.argv[1:]
    print(summarise(arguments[0], *(float(a) for a in arguments[1:3])))

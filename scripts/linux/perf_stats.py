"""The statistics of the measurement protocol (docs/testing/performance-measurement.md): pure functions, tested by tests/test_perf_stats.py.

A measurement is a set of runs per (layout seed, build): instructions and cycles of the guest threads over the same deterministic window. Two builds A and B are
compared seed by seed (the same code layout order for both), so what is left in the difference is the change, not where the linker put the code.
"""
import math
import statistics

# two-sided 95% Student t quantiles for 1..30 degrees of freedom
T95 = [12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, 2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
       2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042]

# Limits of the protocol, set by the validation runs recorded in the document.
DETERMINISM_LIMIT = 0.0002   # the instruction counts of the same binary on the same input differ by less than this share (measured: 0.0013%), or the run is invalid
WORK_EPS = 0.0002            # a change in instructions smaller than this is not counted as a change in the work
MIN_SEEDS = 4                # fewer code layouts than this cannot average the layout out of a time comparison


def t95(degrees):
    return T95[degrees - 1] if 1 <= degrees <= len(T95) else 1.96


def mean_ci(values):
    """-> (mean, half width of the 95% confidence interval of the mean); a single value has no interval (inf)."""
    n = len(values)
    mean = statistics.fmean(values)
    if n < 2:
        return mean, math.inf
    return mean, t95(n - 1) * statistics.stdev(values) / math.sqrt(n)


def spread(values):
    """Relative range (max - min) / mean."""
    mean = statistics.fmean(values)
    return (max(values) - min(values)) / mean if mean else math.inf


def paired_change(a_by_seed, b_by_seed):
    """a_by_seed, b_by_seed: {seed: [measurements of the runs]}. -> {'seeds', 'change' (B over A, as a fraction), 'half' (95% interval half width), 'per_seed'}.
    Each seed contributes one ratio of the mean of B's runs to the mean of A's runs; the interval comes from the ratios of the different layouts."""
    seeds = sorted(set(a_by_seed) & set(b_by_seed))
    ratios = [statistics.fmean(b_by_seed[s]) / statistics.fmean(a_by_seed[s]) - 1 for s in seeds]
    mean, half = mean_ci(ratios) if ratios else (math.nan, math.inf)
    return {"seeds": len(seeds), "change": mean, "half": half, "per_seed": dict(zip(seeds, ratios))}


def invalid_runs(by_seed):
    """Seeds whose repeated runs of one binary disagree by more than DETERMINISM_LIMIT: the work was not the same, the measurement cannot be used."""
    return [s for s, runs in by_seed.items() if len(runs) > 1 and spread(runs) > DETERMINISM_LIMIT]


def verdict(work, time, expected_work=None):
    """work, time: paired_change() results for instructions and cycles. -> (code, text). The decision rule of the protocol:
      INVALID      fewer layouts than MIN_SEEDS for a time claim, or work that is not deterministic
      WORSE        the time interval is entirely above zero (slower on average over the layouts) or the work grew: reject
      FASTER       the time interval is entirely below zero: confirmed gain
      WORK-ONLY    fewer instructions (interval below zero), time inconclusive: the change removes work; the time effect is below what this many layouts resolve
      INCONCLUSIVE neither: nothing is known, nothing is claimed"""
    if time["seeds"] < MIN_SEEDS:
        return "INVALID", f"only {time['seeds']} code layouts (the protocol needs {MIN_SEEDS})"
    lo_t, hi_t = time["change"] - time["half"], time["change"] + time["half"]
    lo_w, hi_w = work["change"] - work["half"], work["change"] + work["half"]
    if lo_w > WORK_EPS:
        return "WORSE", f"more instructions: {100 * work['change']:+.3f}% (95% interval {100 * lo_w:+.3f}..{100 * hi_w:+.3f}%)"
    if lo_t > 0:
        return "WORSE", f"slower over {time['seeds']} layouts: {100 * time['change']:+.2f}% (95% interval {100 * lo_t:+.2f}..{100 * hi_t:+.2f}%)"
    if hi_t < 0:
        return "FASTER", f"faster over {time['seeds']} layouts: {100 * time['change']:+.2f}% (95% interval {100 * lo_t:+.2f}..{100 * hi_t:+.2f}%)"
    if hi_w < -WORK_EPS:
        return "WORK-ONLY", (f"{100 * work['change']:+.3f}% instructions (interval {100 * lo_w:+.3f}..{100 * hi_w:+.3f}%), time {100 * time['change']:+.2f}% "
                             f"(interval {100 * lo_t:+.2f}..{100 * hi_t:+.2f}%): less work, the time effect is below what {time['seeds']} layouts resolve")
    return "INCONCLUSIVE", f"instructions {100 * work['change']:+.3f}%, time {100 * time['change']:+.2f}% (interval {100 * lo_t:+.2f}..{100 * hi_t:+.2f}%)"


def work_verdict(a_instructions, b_instructions):
    """The quick check (one run per build, one layout): only the work is judged, because the instruction count is exact (0.0013% between runs, the same in every code layout).
    -> (code, text): LESS-WORK, MORE-WORK or SAME-WORK. The time is not judged, and nothing is said about it."""
    change = b_instructions / a_instructions - 1
    if change > WORK_EPS:
        return "MORE-WORK", f"{100 * change:+.3f}% instructions: the change adds work"
    if change < -WORK_EPS:
        return "LESS-WORK", f"{100 * change:+.3f}% instructions: the change removes work (the time effect is not judged by the quick check)"
    return "SAME-WORK", f"{100 * change:+.4f}% instructions: no change in the work"


TIME_FLAG = 0.025   # a single pair of runs worse than this in cycles is more than a code layout explains (the layouts of the validation runs span -1.1..+2.0%)


def time_flag(cycles_change):
    """The quick check does not judge the time, but it must not wave through a pair that is clearly slower: text for CHECK-TIME, or ''."""
    if cycles_change > TIME_FLAG:
        return f"the cycles are {100 * cycles_change:+.2f}% (more than a code layout explains): run the full protocol (--full) before keeping the change"
    return ""

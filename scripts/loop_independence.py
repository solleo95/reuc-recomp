"""Which calls of a function can run side by side? Reads the result of the guest memory tracer (runtime/mem_trace.h, UC_TRACE_MEM build).

  python scripts/loop_independence.py <mem-trace.txt> [--siblings]

Each recorded call lists the byte ranges it read and wrote (its callees included, its own stack frames left out) and the HLE functions it used. For every function the calls
that share one caller are compared pairwise: a call that writes what another reads or writes is a conflict. Verdicts:
  independent   no conflict between any two calls of the same caller: the calls could run side by side
  reduction     every conflict is a read-modify-write of the same few bytes in both calls (a counter or a sum: needs per-worker copies and a merge)
  dependent     some call reads or overwrites what another wrote
  impure        an HLE function (GX, OS, scheduler...) ran inside: its effects are not modelled
  needs-irq     the function disables interrupts around shared state: a critical section that would have to become a lock
  single-call   never called twice by one caller in the window: nothing to split
--siblings also compares the different functions a caller called (A against B: could they overlap in time).
"""
import collections
import sys
from dataclasses import dataclass
from pathlib import Path

import re

# HLE functions that only read and write the memory their pointer arguments name (the tracer records r3..r6 as possibly read and written for them).
MEMORY_ONLY = re.compile(r"hle_(Leaf_|SaveGpr|RestGpr|SaveFpr|RestFpr|ModelHistoryIdentity|PSMTX|PSVEC|PSQUAT|Nop\b|MTXPerspective|WorldRaycast|HitBufferConstruct|SegmentTriangleHit|"
                         r"VectorAddStep|BoundsRecordInit|GameActionMask|CrossfadeStep|GameMotionCount|GameWeaponUpdate|GameActionOwnerUpdate)")
# Shared machine state the translated code guards with interrupt masks: the interrupt mask itself, mutexes, message queues, thread switches, the locked cache.
IRQ = re.compile(r"hle_(OSDisableInterrupts|OSRestoreInterrupts|OSLockMutex|OSUnlockMutex|OSSendMessage|OSResumeThread|OSWakeupThread|LCStoreBlocks|LCLoadBlocks)")


@dataclass
class Call:
    id: int
    parent: int
    fn: str
    ticks: int
    reads: list
    writes: list
    hles: list


def _ranges(text):
    out = []
    for item in text.split():
        lo, hi = item.split(":")
        out.append((int(lo, 16), int(hi, 16)))
    return out


def parse(text):
    calls, current = [], None
    for line in text.splitlines():
        try:
            if line.startswith("C "):
                current = None   # a malformed record (a trace cut off at a crash) is skipped together with its R/W/H lines
                _, id_, parent, fn, ticks = line.split()
                current = Call(int(id_), int(parent), fn, int(ticks), [], [], [])
                calls.append(current)
            elif current is not None and line.startswith("R"):
                current.reads = _ranges(line[1:])
            elif current is not None and line.startswith("W"):
                current.writes = _ranges(line[1:])
            elif current is not None and line.startswith("H"):
                current.hles = [n for n in line[1:].strip().split("\t") if n]
        except ValueError:
            if current is not None and current in calls:
                calls.remove(current)
            current = None
    return calls


def _overlaps(a, b):
    """Intersections of two sorted, merged range lists."""
    out, i, j = [], 0, 0
    while i < len(a) and j < len(b):
        lo, hi = max(a[i][0], b[j][0]), min(a[i][1], b[j][1])
        if lo < hi:
            out.append((lo, hi))
        if a[i][1] < b[j][1]:
            i += 1
        else:
            j += 1
    return out


def conflicts(a, b):
    """The byte ranges on which two calls cannot run side by side: written by one and read or written by the other."""
    found = set(_overlaps(a.writes, b.reads)) | set(_overlaps(a.writes, b.writes)) | set(_overlaps(b.writes, a.reads))
    return sorted(found)


def _covered(ranges, r):
    return any(lo <= r[0] and hi >= r[1] for lo, hi in ranges)


def _kind(hles):
    """'impure' when an HLE function with effects other than on memory (graphics, video, anything unknown) ran, 'needs-irq' for interrupt masking, mutexes and the locked cache."""
    if any(not (MEMORY_ONLY.search(n) or IRQ.search(n)) for n in hles):
        return "impure"
    if any(IRQ.search(n) for n in hles):
        return "needs-irq"
    return ""


def analyse(calls):
    by_fn = collections.defaultdict(list)
    for c in calls:
        by_fn[c.fn].append(c)
    result = {}
    for fn, mine in by_fn.items():
        groups = collections.defaultdict(list)
        for c in mine:
            groups[c.parent].append(c)
        summary = {"calls": len(mine), "groups": len(groups), "ticks": sum(c.ticks for c in mine), "example": None, "verdict": "single-call"}
        result[fn] = summary
        many = [g for parent, g in groups.items() if parent and len(g) >= 2]   # parent 0: no tracked caller, the calls may come from different frames or threads
        if not many:
            continue
        kind = _kind([h for c in mine for h in c.hles])
        if kind:
            summary["verdict"] = kind
            continue
        worst = "independent"
        for group in many:
            for i in range(len(group)):
                for j in range(i + 1, len(group)):
                    for r in conflicts(group[i], group[j]):
                        accumulator = all(_covered(c.reads, r) and _covered(c.writes, r) for c in (group[i], group[j]))
                        if not accumulator:
                            worst = "dependent"
                            summary["example"] = summary["example"] or r
                        elif worst == "independent":
                            worst = "reduction"
                            summary["example"] = summary["example"] or r
        summary["verdict"] = worst
    return result


def sibling_conflicts(calls):
    """{(function A, function B): the first conflicting range} for different functions called by the same caller."""
    by_parent = collections.defaultdict(list)
    for c in calls:
        if c.parent:
            by_parent[c.parent].append(c)
    pairs = {}
    for children in by_parent.values():
        for i in range(len(children)):
            for j in range(i + 1, len(children)):
                a, b = children[i], children[j]
                if a.fn == b.fn:
                    continue
                key = tuple(sorted((a.fn, b.fn)))
                if key not in pairs:
                    found = conflicts(a, b)
                    if found:
                        pairs[key] = found[0]
    return pairs


def main():
    path = sys.argv[1]
    calls = parse(Path(path).read_text())
    roots = sum(c.ticks for c in calls if c.parent == 0) or 1
    result = analyse(calls)
    print(f"{len(calls)} calls, {len(result)} functions; time is the share of the outermost tracked calls")
    print(f"{'function':10} {'verdict':12} {'calls':>7} {'callers':>8} {'time':>7}  example conflict")
    for fn, s in sorted(result.items(), key=lambda kv: -kv[1]["ticks"]):
        example = "" if s["example"] is None else f"{s['example'][0]:08X}-{s['example'][1]:08X}"
        print(f"{fn:10} {s['verdict']:12} {s['calls']:7d} {s['groups']:8d} {100 * s['ticks'] / roots:6.1f}%  {example}")
    if "--siblings" in sys.argv:
        print("\nconflicts between different functions of one caller (the others did not conflict):")
        for (a, b), r in sorted(sibling_conflicts(calls).items()):
            print(f"  {a} / {b}: {r[0]:08X}-{r[1]:08X}")


if __name__ == "__main__":
    main()

"""Field step exponents measured from paired RAM dumps of the same scenario at 30 Hz and at a higher update rate.

A float field (class, offset) gets exponent k when every instance with a nonzero, finite value at both rates
has ratio high/low within TOLERANCE of s^k, k in {-1, 0, 1, 2}, and at least MIN_SAMPLES instance pairs agree.
"""
import math
import re
import struct
from pathlib import Path

TOLERANCE = 0.02
MIN_SAMPLES = 2
MAX_OBJECT = 0x2800          # bytes scanned per object
EXPONENTS = (0, 1, 2, -1)


def _floats(buf, base, address, size):
    o = address - base
    size = min(size, len(buf) - o) // 4 * 4
    if o < 0 or size <= 0:
        return None
    return struct.unpack_from(">%df" % (size // 4), buf, o)


def type_fields(pairs, base, index, vptr_offsets, s):
    """pairs: [(low_rate_bytes, high_rate_bytes)] sharing `base`; vptr_offsets: vtable -> offset of the vptr in the object."""
    votes = {}
    for low, high in pairs:
        for vt, holders in index.objects.items():
            off = vptr_offsets.get(vt, 0)
            for loc in holders:
                obj = loc - off
                a = _floats(low, base, obj, MAX_OBJECT)
                b = _floats(high, base, obj, MAX_OBJECT)
                if a is None or b is None:
                    continue
                if struct.unpack_from(">I", high, loc - base)[0] != vt:
                    continue          # different object at this address in the other run
                for i, (x, y) in enumerate(zip(a, b)):
                    key = (("class", vt), 4 * i)
                    if not (math.isfinite(x) and math.isfinite(y)) or x == 0 or y == 0 or abs(x) < 1e-6 or abs(x) > 1e7:
                        continue
                    ratio = y / x
                    k = next((e for e in EXPONENTS if abs(ratio - s ** e) <= TOLERANCE * s ** e), "none")
                    votes.setdefault(key, []).append(k)
    return {key: ks[0] for key, ks in votes.items() if len(ks) >= MIN_SAMPLES and len(set(ks)) == 1 and ks[0] != "none"}


def pointer_types(raws, base, index, vptr_offsets, max_object=MAX_OBJECT):
    """(class V, offset) -> class W when, in every dump and every V instance, the word at that offset points at a W object."""
    starts = {}
    for vt, holders in index.objects.items():
        for loc in holders:
            starts[loc - vptr_offsets.get(vt, 0)] = vt
    votes = {}
    for raw in raws:
        end = base + len(raw)
        for vt, holders in index.objects.items():
            off = vptr_offsets.get(vt, 0)
            for loc in holders:
                obj = loc - off
                if not (base <= obj and loc + 4 <= end) or struct.unpack_from(">I", raw, loc - base)[0] != vt:
                    continue
                size = min(max_object, end - obj) // 4 * 4
                words = struct.unpack_from(">%dI" % (size // 4), raw, obj - base)
                for i, w in enumerate(words):
                    if w and w in starts or (w and 0x80000000 <= w < 0x94000000 and i):
                        votes.setdefault((("class", vt), 4 * i), []).append(starts.get(w))
    return {key: ws[0] for key, ws in votes.items() if ws[0] is not None and len(set(ws)) == 1}


def vptr_offsets(functions, classes):
    """Most common X in `lwz r12, X(r3|r31)` inside each class's methods; default 0."""
    counts = {}
    for f, vt in classes.items():
        fn = functions.get(f)
        if not fn:
            continue
        for insn in fn.insns:
            if insn.op == "lwz" and len(insn.args) == 3 and insn.args[0] == 12 and insn.args[1] in (3, 31) and insn.args[2] < 0x100:
                counts.setdefault(vt, {}).setdefault(insn.args[2], 0)
                counts[vt][insn.args[2]] += 1
    return {vt: max(c, key=c.get) for vt, c in counts.items()}


def dump_pairs(autotest: Path):
    """Same scenario and dump time at 30 Hz and 480 Hz: episode<ch>-30-<stop>-* vs episode<ch>-480-<stop>-*."""
    runs = {}
    for d in Path(autotest).glob("episode*-*-*-*"):
        m = re.match(r"episode(\d+)-(\d+)-(-?\d+)-\d+$", d.name)
        if m:
            runs.setdefault((m[1], m[3]), {}).setdefault(int(m[2]), []).append(d)
    pairs = []
    for (_, _), by_rate in runs.items():
        if 30 not in by_rate or 480 not in by_rate:
            continue
        for low_run in by_rate[30]:
            for high_run in by_rate[480]:
                for low in sorted(low_run.glob("dump_*.bin")):
                    high = high_run / low.name
                    if high.exists():
                        pairs.append((low, high))
    return pairs

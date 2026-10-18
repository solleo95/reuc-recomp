"""Compare guest RAM snapshots of the same scripted scene taken at the same game times with two step rates (UC_STATE_DUMP, game/state_dump.h).

python tools/spikes/state_diff.py <prefixA> <prefixB> <seconds> [<seconds> ...]

Words that hold a finite float of a plausible size and differ by more than 5% in both of the last two snapshots are listed per 4 KB page (a page is mostly one object):
objects that follow the real step agree at equal game time, objects that advance per call do not.
"""
import struct, sys

MEM1, TOTAL = 24 << 20, 88 << 20
REL = 0.05


def address(offset):
    return 0x80000000 + offset if offset < MEM1 else 0x90000000 + offset - MEM1


def load(prefix, seconds):
    with open("%s_%.2f.bin" % (prefix, seconds), "rb") as f:
        return f.read()


def differing(a, b):
    """offset -> (floatA, floatB) of plausible floats that differ by more than REL."""
    out = {}
    for page in range(0, TOTAL, 4096):
        if a[page:page + 4096] == b[page:page + 4096]:
            continue
        wa = struct.unpack_from(">1024f", a, page)
        wb = struct.unpack_from(">1024f", b, page)
        for i in range(1024):
            x, y = wa[i], wb[i]
            if x == y or not (x == x and y == y):
                continue
            m = max(abs(x), abs(y))
            if 1e-3 < m < 1e6 and abs(x - y) > REL * m:
                out[page + 4 * i] = (x, y)
    return out


def main():
    prefix_a, prefix_b, times = sys.argv[1], sys.argv[2], [float(t) for t in sys.argv[3:]]
    sets = []
    for t in times[-2:]:
        sets.append(differing(load(prefix_a, t), load(prefix_b, t)))
    common = set(sets[0]) & set(sets[1])
    pages = {}
    for off in common:
        pages.setdefault(off >> 12, []).append(off)
    print("words differing at the last two times:", len(common), "pages:", len(pages))
    for page, offs in sorted(pages.items(), key=lambda kv: -len(kv[1]))[:40]:
        offs.sort()
        sample = ", ".join("%08X %.4g/%.4g" % (address(o), sets[1][o][0], sets[1][o][1]) for o in offs[:4])
        print("page %08X words %d: %s" % (address(page << 12), len(offs), sample))


main()

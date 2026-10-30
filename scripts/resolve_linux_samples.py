"""Names the functions of a Linux flat profile (game/cpu_sampler.cpp, UC_CPU_SAMPLE=1 on the Deck) and ranks them.

  python scripts/resolve_linux_samples.py <cpu-samples.txt> <uc binary with symbols> [top N] [--threads]

The sampler records the interrupted instruction of whatever thread was running; the report is per thread (the guest thread is the one with the most samples),
then per function with its share of that thread, grouped as translated guest code (fn_8xxxxxxx), the runtime and the libraries.
"""
import collections
import re
import subprocess
import sys
from pathlib import Path

SYMBOLIZER = r"C:\Program Files\LLVM\bin\llvm-symbolizer.exe"


def load(path):
    base = 0
    samples, waits = [], []
    for line in Path(path).read_text().splitlines():
        if line.startswith("# base"):
            base = int(line.split()[2], 16)
        elif line.startswith("w "):   # a wall-clock sample of the guest thread (UC_CPU_SAMPLE=wait): the top frame's name, the first frame in the executable
            _, top, caller, ms = line.split()
            waits.append((top, int(caller, 16), int(ms)))
        else:
            tid, rip, ms = line.split()
            samples.append((int(tid), int(rip, 16), int(ms)))
    return base, samples, waits


def report_waits(binary, base, waits, top):
    callers = symbolize(binary, [c - base for _, c, _ in waits if c])
    total = len(waits)
    print(f"{total} wall-clock samples of the guest thread over {(waits[-1][2] - waits[0][2]) / 1000:.1f} s")
    tops = collections.Counter(t for t, _, _ in waits)
    print("where the top frame is (exe = running our code):")
    for name, n in tops.most_common(12):
        print(f"  {100 * n / total:5.1f}%  {name}")
    blocked = [c for t, c, _ in waits if t != "exe" and c]
    print(f"\nsamples outside the executable (libc, the driver, the kernel wait): {100 * sum(1 for t, _, _ in waits if t != 'exe') / total:.1f}%; by calling function:")
    for name, n in collections.Counter(callers[c - base] for c in blocked).most_common(top):
        print(f"  {100 * n / total:5.1f}%  {name[:110]}")


def symbolize(binary, addresses):
    """address (file offset in the image) -> function name, in one llvm-symbolizer run."""
    unique = sorted(set(addresses))
    result = subprocess.run([SYMBOLIZER, f"--obj={binary}", "--functions=linkage", "--no-inlines"], input="\n".join(hex(a) for a in unique), capture_output=True, text=True)
    lines = result.stdout.replace("\r\n", "\n").strip("\n").split("\n\n")   # one block per address: the name, then file:line
    out = {}
    for address, block in zip(unique, lines):
        out[address] = block.split("\n")[0] if block else "?"
    return out


def main():
    path, binary = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else 25
    base, samples, waits = load(path)
    if waits:
        if "--last" in sys.argv:   # only the final N seconds (the measured window of a replay)
            seconds = int(sys.argv[sys.argv.index("--last") + 1])
            waits = [w for w in waits if w[2] >= waits[-1][2] - 1000 * seconds]
        report_waits(binary, base, waits, top)
        return
    by_thread = collections.Counter(tid for tid, _, _ in samples)
    names = symbolize(binary, [rip - base for _, rip, _ in samples])
    total = len(samples)
    print(f"{total} samples, {samples[-1][2] - samples[0][2]} ms; threads: " + ", ".join(f"{tid} {100 * n / total:.0f}%" for tid, n in by_thread.most_common(4)))
    for tid, count in by_thread.most_common(2):
        mine = [names[rip - base] for t, rip, _ in samples if t == tid]
        groups = collections.Counter("guest code" if re.match(r"fn_[0-9A-F]{8}", n) else "other" for n in mine)
        print(f"\n=== thread {tid}: {count} samples ({100 * count / total:.0f}% of all); guest code {100 * groups['guest code'] / count:.0f}%, runtime/libraries {100 * groups['other'] / count:.0f}%")
        functions = collections.Counter(mine)
        for name, n in functions.most_common(top):
            print(f"  {100 * n / count:5.1f}%  {name[:110]}")
        runtime = collections.Counter(n for n in mine if not re.match(r"fn_[0-9A-F]{8}", n))
        if runtime:
            print("  -- runtime and libraries only:")
            for name, n in runtime.most_common(top):
                print(f"  {100 * n / count:5.1f}%  {name[:110]}")


main()

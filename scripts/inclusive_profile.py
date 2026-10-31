"""Inclusive time per function of a Linux stack profile (game/cpu_sampler.cpp, UC_CPU_SAMPLE=1 UC_CPU_SAMPLE_STACKS=1 on the Deck).

  python scripts/inclusive_profile.py <cpu-samples.txt> <uc binary with symbols> [top N] [--min PERCENT]

Each `s <tid> <ms> <rip> <return addresses...>` line is one sample of a thread with its call stack. The report is for the busiest thread (the guest thread): per function the
share of its samples that had the function anywhere on the stack (`incl`, each function once per sample, recursion included) and the share where its code was running (`self`).
Translated guest functions are named fn_/body_ + the guest address, so the table reads as a guest call profile.
"""
import collections
import os
import shutil
import subprocess
import sys
from pathlib import Path

DEFAULT_SYMBOLIZER = r"C:\Program Files\LLVM\bin\llvm-symbolizer.exe"


def parse(text):
    """-> (image base, [(tid, [relative addresses: the instruction pointer, then the return addresses])])."""
    base, samples = 0, []
    for line in text.splitlines():
        if line.startswith("# base"):
            base = int(line.split()[2], 16)
        elif line.startswith("s "):
            fields = line.split()
            samples.append((int(fields[1]), [int(a, 16) for a in fields[3:]]))
    return base, [(tid, [a - base for a in frames]) for tid, frames in samples]


def inclusive(samples, name_of):
    """samples: lists of addresses (instruction pointer first). -> (inclusive Counter, self Counter, sample count); name_of(address) is None for code outside the image."""
    incl, own = collections.Counter(), collections.Counter()
    for frames in samples:
        names = [name_of(a) for a in frames]
        for name in set(n for n in names if n):
            incl[name] += 1
        if names and names[0]:
            own[names[0]] += 1
    return incl, own, len(samples)


def find_symbolizer(environment, which):
    """UC_LLVM_SYMBOLIZER, else llvm-symbolizer on the PATH, else the usual Windows install."""
    return environment.get("UC_LLVM_SYMBOLIZER") or which("llvm-symbolizer") or DEFAULT_SYMBOLIZER


def parse_symbolizer(output):
    """llvm-symbolizer --print-address output -> {address: function name or None}. Blocks are told apart by their `0x...` address line, not by position, so an odd block shifts nothing."""
    names, current = {}, None
    for line in output.replace("\r\n", "\n").split("\n"):
        if line.startswith("0x") and " " not in line:
            current = int(line, 16)
            names[current] = False   # the next line is the function name
        elif current is not None and names.get(current) is False:
            names[current] = None if line in ("", "??") else line
    return {a: (n or None) for a, n in names.items()}


def unwound_fraction(frames):
    """Share of the samples that carry at least one return address; a sample taken inside a library has none (the sampler does not unwind there)."""
    return sum(1 for f in frames if len(f) > 1) / len(frames) if frames else 0.0


def symbolize(binary, addresses):
    unique = sorted(set(addresses))
    result = subprocess.run([find_symbolizer(os.environ, shutil.which), f"--obj={binary}", "--print-address", "--functions=linkage", "--no-inlines"], input="\n".join(hex(a) for a in unique), capture_output=True, text=True)
    names = parse_symbolizer(result.stdout)
    return {a: names.get(a) for a in unique}


def main():
    path, binary = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else 40
    floor = float(sys.argv[sys.argv.index("--min") + 1]) if "--min" in sys.argv else 0.0
    base, samples = parse(Path(path).read_text())
    if not samples:
        sys.exit("no stack samples in the file (run with UC_CPU_SAMPLE=1 UC_CPU_SAMPLE_STACKS=1)")
    guest = collections.Counter(tid for tid, _ in samples).most_common(1)[0][0]
    frames = [f for tid, f in samples if tid == guest]
    # a return address points after the call: look one byte back so the call site is inside the caller
    addresses = {a if i == 0 else a - 1 for f in frames for i, a in enumerate(f)}
    names = symbolize(binary, addresses)
    resolved = [[names.get(a if i == 0 else a - 1) for i, a in enumerate(f)] for f in frames]
    incl, own, total = inclusive(resolved, lambda n: n)
    print(f"thread {guest}: {total} samples ({100 * unwound_fraction(frames):.0f}% with a call stack; the rest were taken inside libraries and count only in the total); "
          "incl% = stacks containing the function, self% = its own code running")
    for name, n in incl.most_common():
        if 100 * n / total < floor or top <= 0:
            break
        top -= 1
        print(f"{100 * n / total:6.1f}% {100 * own[name] / total:6.1f}%  {name[:110]}")


if __name__ == "__main__":
    main()

"""python scripts/resolve_cpu_samples.py <cpu-samples.txt> <uc.exe built with /Z7 + pdb> [top]
Self and inclusive time per function, plus the hottest source lines (llvm-symbolizer). Sample file: "rva rva ... = count" per distinct stack."""
import collections, shutil, subprocess, sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
SYMBOLIZER = shutil.which("llvm-symbolizer") or "C:/Program Files/LLVM/bin/llvm-symbolizer.exe"
BASE = 0x140000000
exe, top = sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 40
stacks = []
for line in open(sys.argv[1]):
    frames, count = line.split("=")
    stacks.append(([int(f, 16) for f in frames.split()], int(count)))
addresses = sorted({f for s, _ in stacks for f in s if f < 0x10000000})
out = subprocess.run([SYMBOLIZER, "--obj=" + exe, "--no-inlines", "--demangle"], input="\n".join(hex(BASE + a) for a in addresses) + "\n",
                     capture_output=True, text=True, encoding="utf-8", errors="replace").stdout.strip().split("\n\n")
name, where = {}, {}
for a, block in zip(addresses, out):
    lines = block.split("\n")
    name[a] = lines[0][:110]
    where[a] = lines[1].rsplit("\\", 1)[-1][:60] if len(lines) > 1 else "?"
total = sum(n for _, n in stacks)
self_time, inclusive, lines_hit, edges = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
for frames, n in stacks:
    frames = [f for f in frames if f in name]
    if not frames: continue
    self_time[name[frames[0]]] += n
    lines_hit[where[frames[0]]] += n
    for fn in {name[f] for f in frames}: inclusive[fn] += n
    if len(frames) > 1: edges[(name[frames[0]][:60], name[frames[1]][:60])] += n
print("samples", total)
print("\n== self"); [print(f"{v / total * 100:5.1f}% {k}") for k, v in self_time.most_common(top)]
print("\n== inclusive"); [print(f"{v / total * 100:5.1f}% {k}") for k, v in inclusive.most_common(top)]
print("\n== lines"); [print(f"{v / total * 100:5.1f}% {k}") for k, v in lines_hit.most_common(top)]
print("\n== leaf <- caller"); [print(f"{v / total * 100:5.1f}% {a} <- {b}") for (a, b), v in edges.most_common(top)]

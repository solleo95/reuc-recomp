"""python scripts/guest_op_profile.py <cpu-samples.txt> <uc.exe built with /Z7 + pdb> [top]

Where the translated guest code spends its time: every sample whose leaf lies in generated code (game_NNN.cpp) is attributed to the PPC instruction of
its source line (`ppc::op_lwz`, a branch, a call, ...) and to the innermost inlined primitive (hp, note_write, is_mmio, byte swap, ...).
Samples in the runtime or in HLE functions are reported as one lump."""
import collections
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
SYMBOLIZER = shutil.which("llvm-symbolizer") or "C:/Program Files/LLVM/bin/llvm-symbolizer.exe"
BASE = 0x140000000
samples, exe = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25

stacks = []
for line in open(samples):
    frames, count = line.split("=")
    frames = [int(f, 16) for f in frames.split()]
    stacks.append((frames, int(count)))
leaves = sorted({frames[0] for frames, _ in stacks if frames and frames[0] < 0x10000000})
text = subprocess.run([SYMBOLIZER, "--obj=" + exe, "--inlines", "--demangle"], input="\n".join(hex(BASE + a) for a in leaves) + "\n",
                      capture_output=True, text=True, encoding="utf-8", errors="replace").stdout.strip().split("\n\n")
chains = {}
for address, block in zip(leaves, text):
    rows = block.split("\n")
    chain = [(rows[i], rows[i + 1]) for i in range(0, len(rows) - 1, 2)]   # innermost first: (function, file:line:col)
    chains[address] = chain

source_cache = {}


def source_line(path, number):
    if path not in source_cache:
        try:
            source_cache[path] = Path(path).read_text(errors="replace").split("\n")
        except OSError:
            source_cache[path] = []
    lines = source_cache[path]
    return lines[number - 1] if 0 < number <= len(lines) else ""


def classify(code):
    m = re.search(r"ppc::(\w+)\(", code)
    if m:
        return m.group(1)
    if "goto" in code or "crbit" in code:
        return "branch"
    if "return" in code:
        return "return"
    if re.search(r"\bfn_[0-9A-F]+\(c\)", code) or "call_indirect" in code:
        return "call"
    return "other"


by_op, by_primitive, by_pair = collections.Counter(), collections.Counter(), collections.Counter()
total = in_guest = 0
for frames, count in stacks:
    total += count
    chain = chains.get(frames[0]) if frames else None
    if not chain:
        continue
    outer = chain[-1][1]
    m = re.match(r"(.*?[\\/]generated[\\/].*?game_\d+\.cpp):(\d+)", outer)
    if not m:
        continue
    in_guest += count
    op = classify(source_line(m.group(1), int(m.group(2))))
    inner_function = chain[0][0]
    if len(chain) > 1 and "game_" not in chain[0][1]:
        primitive = re.sub(r"\(.*", "", inner_function.split("::")[-1])
    else:
        primitive = "(the instruction itself)"
    by_op[op] += count
    by_primitive[primitive] += count
    by_pair[(op, primitive)] += count

print(f"samples {total}; leaf in translated code {100 * in_guest / total:.1f}%")
print("\n== by PPC instruction (share of translated-code samples)")
for key, n in by_op.most_common(top):
    print(f"  {100 * n / in_guest:5.1f}%  {key}")
print("\n== by innermost inlined primitive")
for key, n in by_primitive.most_common(top):
    print(f"  {100 * n / in_guest:5.1f}%  {key}")
print("\n== instruction x primitive")
for (op, primitive), n in by_pair.most_common(top):
    print(f"  {100 * n / in_guest:5.1f}%  {op:12s} {primitive}")

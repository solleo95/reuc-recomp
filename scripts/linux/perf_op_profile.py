"""Instructions executed per kind of guest operation: python scripts/linux/perf_op_profile.py RAW BINARY [--top 40]

RAW is the file written by perf_instr_profile.py --raw (thread id and address in the binary of every sample), BINARY the build with -gline-tables-only that ran
(scripts/linux/build.ps1 -NoPgo -Folder dbgnp with CXXFLAGS=-gline-tables-only). The addresses of the guest thread (the busiest) are resolved to source lines with
llvm-symbolizer, inlined frames included; a sample inside generated code (generated/.../game_NNN.cpp) is attributed to the guest instruction on that line
(`/*8037AB90*/ { ppc::op_stwu(c, 1, 1, -48); }`) and from there to its operation (`stwu`, `lwz`, ...); the part of the sample spent in the runtime helpers that the operation
inlines is shown separately (which line of ppc_rt.h). The result is the share of the guest thread's instructions per guest operation, the guide to which helper of
runtime/ppc_rt.h is worth optimising.
"""
import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import localenv   # noqa: E402

OP = re.compile(r"ppc::op_(\w+)|uc::invoke_hle<&?(\w+)>|(fn_[0-9A-F]+)\(c\)|(call_indirect)|(c\.ctr)|(--c\.ctr)|(poll)|(cf\d[ab]|\(c\.f\[)")


def classify(text):
    m = re.search(r"ppc::op_(\w+)", text)
    if m:
        return "op_" + m[1]
    if "invoke_hle" in text:
        return "invoke_hle"
    if "call_indirect" in text:
        return "call_indirect"
    if re.search(r"\bfn_[0-9A-F]+\(c\)", text):
        return "direct call"
    if "poll(c)" in text:
        return "branch+poll"
    if "goto" in text:
        return "branch/fused compare"
    if "/*" in text and "folded" in text:
        return "folded compare"
    return "other: " + text.strip()[:50]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("raw"), parser.add_argument("binary")
    parser.add_argument("--top", type=int, default=40)
    args = parser.parse_args()
    samples = [(int(a), int(b, 16)) for a, b in (line.split() for line in Path(args.raw).read_text().splitlines() if line.strip())]
    guest = collections.Counter(t for t, _ in samples).most_common(1)[0][0]
    addresses = sorted({ip for t, ip in samples if t == guest})
    out = subprocess.run([localenv.llvm_tool("llvm-symbolizer"), f"--obj={args.binary}", "--print-address", "--inlines", "--functions=none"],
                         input="\n".join(hex(a) for a in addresses), capture_output=True, text=True).stdout
    frames, current = {}, None
    for line in out.replace("\r\n", "\n").split("\n"):
        if line.startswith("0x") and " " not in line:
            current = int(line, 16); frames[current] = []
        elif current is not None and line and line != "??:0:0":
            frames[current].append(line)
    sources = {}

    def source_line(path, number):
        if path not in sources:
            try:
                sources[path] = Path(path).read_text(errors="replace").splitlines()
            except OSError:
                sources[path] = []
        lines = sources[path]
        return lines[number - 1] if 0 < number <= len(lines) else ""

    total = collections.Counter(); inner = collections.defaultdict(collections.Counter); n = 0
    for t, ip in samples:
        if t != guest:
            continue
        n += 1
        chain = frames.get(ip) or []
        kind, helper = None, None
        for entry in chain:   # innermost first; the outermost generated line names the guest operation
            m = re.match(r"(.*):(\d+):\d+$", entry)
            if not m:
                continue
            path, number = m[1].replace("\\", "/"), int(m[2])
            if re.search(r"/game_\d+\.cpp$", path):
                kind = classify(source_line(path, number))
            elif helper is None and path.endswith("ppc_rt.h"):
                helper = f"ppc_rt.h:{number}"
        if kind is None:
            kind = "(not in generated code: " + (chain[0].rsplit("/", 1)[-1][:40] if chain else "no line info") + ")"
        total[kind] += 1
        if helper:
            inner[kind][helper] += 1
    print(f"{n} guest-thread samples")
    for kind, count in total.most_common(args.top):
        helpers = ", ".join(f"{h} {100 * c / count:.0f}%" for h, c in inner[kind].most_common(3))
        print(f"{100 * count / n:5.2f}%  {kind:34s} {helpers}")


main()

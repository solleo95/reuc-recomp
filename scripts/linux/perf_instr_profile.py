"""Instruction profile of the game on the Steam Deck: python scripts/linux/perf_instr_profile.py BINARY_WITH_SYMBOLS [--top 50] [--seconds 14] [--delay 12]

Runs scripts/linux/deck_bench.py on the deterministic demo with the binary and, in parallel, records `perf record -e instructions:u` of the game on the Deck for --seconds
seconds after --delay seconds (past the start-up), then resolves the instruction addresses with llvm-symbolizer on the local BINARY (the unstripped build, the same file that
ran). The result is the share of the executed instructions per function, and per thread: the guest threads are the ones in translated code, the renderer is the other big
one. Unlike the time sampler (UC_CPU_SAMPLE) it does not move with code layout or cache behaviour. The text of the game is remapped to anonymous memory at start-up, so
perf alone cannot name the translated functions; this does it from the addresses.
"""
import argparse
import collections
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import localenv   # noqa: E402

SSH = ["ssh", "-o", "BatchMode=yes", "deck"]


def text_segment(binary):
    """-> (page offset, page vaddr) of the executable PT_LOAD of the binary."""
    out = subprocess.run([localenv.llvm_tool("llvm-objdump"), "-p", str(binary)], capture_output=True, text=True).stdout.splitlines()
    for i, line in enumerate(out):
        if "LOAD" in line and i + 1 < len(out) and "flags r-x" in out[i + 1]:
            off = int(re.search(r"off\s+(0x[0-9a-f]+)", line)[1], 16)
            vaddr = int(re.search(r"vaddr\s+(0x[0-9a-f]+)", line)[1], 16)
            return off & ~0xFFF, vaddr & ~0xFFF
    sys.exit("no executable segment")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("--top", type=int, default=50)
    parser.add_argument("--seconds", type=int, default=14)
    parser.add_argument("--delay", type=int, default=12)
    parser.add_argument("--demo", default="fight4", help="the deterministic demo to replay (build/demos)")
    parser.add_argument("--stat", help="instead of recording: counts of these perf events (comma separated) for the whole process, printed from /tmp/perf.log")
    parser.add_argument("--event", default="instructions:u", help="perf event: instructions:u (the work) or cycles:u (where the time goes; the sample lands a few instructions after the one that waited)")
    parser.add_argument("--period", type=int, default=400000, help="events per sample (use 800000 for cycles:u)")
    parser.add_argument("--raw", help="also write 'tid address-in-the-binary' lines of every sample to this file (for line-level analysis of a build with -gline-tables-only)")
    args = parser.parse_args()
    binary = Path(args.binary).resolve()
    label = "IP"
    wait_for_game = f'for i in $(seq 1 600); do pid=$(pgrep -x "uc_{label}" | head -1); [ -n "$pid" ] && break; sleep 0.5; done; sleep {args.delay}; '
    if args.stat:
        measure = f"perf stat -e {args.stat} -p $pid -- sleep {args.seconds} >/tmp/perf.log 2>&1"
    else:
        measure = f"perf record -e {args.event} -c {args.period} -p $pid -o /tmp/perf.data -- sleep {args.seconds} >/tmp/perf.log 2>&1"
    record = subprocess.Popen(SSH + [wait_for_game + measure])
    env = dict(os.environ, UC_DECK_DIR=os.environ.get("UC_DECK_DIR", "~/Games/UmbrellaChronicles"), UC_DECK_SHORTCUT=os.environ.get("UC_DECK_SHORTCUT", "11761780335405170688"))
    subprocess.run([sys.executable, str(ROOT / "scripts" / "linux" / "deck_bench.py"), "--seconds", "10", "--demo", args.demo, f"{label}={binary}"], env=env, stdout=subprocess.DEVNULL)
    record.wait()
    if args.stat:
        print(subprocess.run(SSH + ["cat /tmp/perf.log"], capture_output=True, text=True).stdout)
        return
    mmaps = subprocess.run(SSH + [f"perf script -i /tmp/perf.data --show-mmap-events 2>/dev/null | grep 'PERF_RECORD_MMAP2' | grep 'uc_{label}' | head -4"], capture_output=True, text=True).stdout
    page_offset, page_vaddr = text_segment(binary)
    base = None
    for line in mmaps.splitlines():
        m = re.search(r"\[0x([0-9a-f]+)\(0x[0-9a-f]+\) @ (?:0x)?([0-9a-f]+) .*r-xp", line)
        if m and int(m[2], 16) == page_offset:
            base = int(m[1], 16) - page_vaddr
    if base is None:
        sys.exit("cannot find the mapping of the executable in the perf data:\n" + mmaps)
    ips = subprocess.run(SSH + ["perf script -i /tmp/perf.data -F tid,ip 2>/dev/null"], capture_output=True, text=True).stdout.split("\n")
    samples = [(int(a), int(b, 16)) for a, b in (line.split() for line in ips if line.strip())]
    if args.raw:
        Path(args.raw).write_text("\n".join(f"{tid} {ip - base:#x}" for tid, ip in samples if 0 <= ip - base < 0x8000000))
    addresses = sorted({ip - base for _, ip in samples if 0 <= ip - base < 0x8000000})
    out = subprocess.run([localenv.llvm_tool("llvm-symbolizer"), f"--obj={binary}", "--print-address", "--functions=linkage", "--no-inlines"], input="\n".join(hex(a) for a in addresses), capture_output=True, text=True).stdout
    names, current = {}, None
    for line in out.replace("\r\n", "\n").split("\n"):
        if line.startswith("0x") and " " not in line:
            current = int(line, 16); names[current] = False
        elif current is not None and names.get(current) is False:
            names[current] = None if line in ("", "??") else line
    by, by_tid = collections.Counter(), collections.defaultdict(collections.Counter)
    for tid, ip in samples:
        name = names.get(ip - base) or "(outside the executable: libc, drivers, kernel entry)"
        by[name] += 1; by_tid[name][tid] += 1
    total = len(samples)
    print(f"{total} samples of 400000 instructions; threads: " + ", ".join(f"{t}:{n}" for t, n in collections.Counter(t for t, _ in samples).most_common(5)))
    for name, count in by.most_common(args.top):
        print(f"{100 * count / total:5.2f}%  {name[:100]}  {dict(by_tid[name].most_common(2))}")


main()

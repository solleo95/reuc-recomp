"""Which generated functions take the compile time: python scripts/compile_hotspots.py [build folder, default build-linux-nopgo] [parallel jobs, default 16]

Compiles every generated game file once more with clang's -ftime-trace (without ccache, to a scratch object) and lists the functions whose optimisation takes the most time.
A function of several seconds belongs in project/uc.cfg as `isolate <hex>`: the translator then emits it in a file of its own, so the build compiles it next to the other files instead of
after them (the compile time of one function cannot be split). Run it again after the translation changes a lot; functions below about 3 seconds are not worth a line.
"""
import json
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / (sys.argv[1] if len(sys.argv) > 1 else "build-linux-nopgo")
JOBS = int(sys.argv[2]) if len(sys.argv) > 2 else 16
GENERATION = sorted((ROOT / "generated").glob("generation_*"), key=lambda p: p.stat().st_mtime)[-1].name
NAMES = sorted(p.stem for p in (ROOT / "generated" / GENERATION).glob("game_[0-9][0-9][0-9].cpp"))


def trace(name):
    target = f"game/CMakeFiles/uc_game.dir/__/generated/{GENERATION}/{name}.cpp.o"
    command = subprocess.run(["ninja", "-t", "commands", target], cwd=BUILD, capture_output=True, text=True).stdout.strip().splitlines()[-1]
    command = re.sub(r"^.*?ccache(\.exe)?\s+", "", command)   # the compiler itself, not through the cache
    path, out = BUILD / f"{name}.trace.json", BUILD / f"{name}.tmp.o"
    command = command.replace(" -o ", f' -ftime-trace="{path}" -ftime-trace-granularity=200000 -o ', 1)
    command = re.sub(r"-MD -MT \S+ -MF \S+", "", command)
    command = re.sub(r"-o \S+", lambda m: f'-o "{out}"', command, count=1)
    subprocess.run(command, cwd=BUILD, shell=True, capture_output=True, text=True)
    events = json.loads(path.read_text())["traceEvents"]
    functions = {}
    for e in events:
        if e.get("ph") == "X" and e["name"] == "OptFunction":
            detail = e.get("args", {}).get("detail", "")
            functions[detail] = functions.get(detail, 0) + e["dur"] / 1e6
    total = next((e["dur"] / 1e6 for e in events if e.get("name") == "Total ExecuteCompiler"), 0)
    path.unlink(missing_ok=True)
    out.unlink(missing_ok=True)
    return name, total, functions


def main():
    with ThreadPoolExecutor(max_workers=JOBS) as pool:
        results = sorted(pool.map(trace, NAMES), key=lambda r: -r[1])
    print("slowest files:", ", ".join(f"{n[-3:]}={t:.0f}s" for n, t, _ in results[:12]))
    rows = sorted(((d, n, f) for n, _, fs in results for f, d in fs.items()), reverse=True)
    for d, n, f in rows[:12]:
        m = re.match(r"_Z\d+(fn|body)_([0-9A-F]+)", f) or re.match(r"_ZL\d+(fn|body)_([0-9A-F]+)", f)
        print(f"{d:6.1f}s  {n}  {m[2] if m else f[:50]}")
    print("longest file:", f"{results[0][1]:.0f}s", "(the build cannot finish before its longest file)")


main()

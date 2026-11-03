"""Replays one demo with two exes (hash trace of guest memory per retrace) and reports which 64 KiB chunks ever differ and from which retrace.
usage: py -3.13 cross_exe_trace.py <demo name> <exeA> <exeB> [frames]"""
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
import det_check  # noqa: E402
from uctest.client import SAVE_SOURCE  # noqa: E402
from uctest.demo import DEMOS, paths, replay_args  # noqa: E402

import os  # noqa: E402
GAME = Path(os.environ.get("UC_GAME_DIR") or Path(os.environ["UC_INSTALL_DIR"]) / "game")   # the folder with DATA\sys\main.dol (UC_INSTALL_DIR comes from local.env)
name, exe_a, exe_b = sys.argv[1], Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve()
frames = int(sys.argv[4]) if len(sys.argv) > 4 else 1000000
demo, meta = paths(name)
window = json.loads(meta.read_text())
folder = ROOT / "build" / "det"
folder.mkdir(parents=True, exist_ok=True)
traces = []
for index, exe in enumerate((exe_a, exe_b)):
    saves = folder / f"cross-saves{index}"
    if saves.exists():
        shutil.rmtree(saves)
    shutil.copytree(SAVE_SOURCE, saves)
    out = folder / f"cross{index}.bin"
    with open(out.with_suffix(".log"), "wb") as log:
        subprocess.run(replay_args(exe, demo, window["to"], saves, ["--det-out", str(out), "--det-frames", str(frames), "--game", str(GAME)]),
                       stdout=log, stderr=log, cwd=exe.parent, timeout=3600, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    traces.append(det_check.read_trace(out))
    print(f"{exe.name}: {len(traces[-1])} retraces", flush=True)
a, b = traces
common = min(len(a), len(b))
first = {}
for i in range(common):
    for chunk in range(det_check.CHUNKS):
        if a[i][1][chunk] != b[i][1][chunk] and chunk not in first:
            first[chunk] = i
print(f"records compared {common}; chunks that ever differ: {len(first)}")
ranges = []
for chunk in sorted(first):
    if ranges and ranges[-1][1] == chunk - 1:
        ranges[-1][1] = chunk
    else:
        ranges.append([chunk, chunk])
for lo, hi in ranges[:40]:
    print(f"  {det_check.chunk_address(lo):08X}..{det_check.chunk_address(hi) + 0xFFFF:08X}  first at record {min(first[c] for c in range(lo, hi + 1))}")

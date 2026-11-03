"""Relinks the Linux uc with shuffled section order: python scripts/linux/relink_layouts.py LABEL SEED [SEED ...]

The objects of the current build folder (build-linux-znver2, after scripts/linux/build.ps1 -Target uc) are linked once per seed with lld's
--shuffle-sections=*=SEED, so the same compiled code lands at a different place in the binary. The results are build/layouts/LABEL_sSEED: one source state, K
code layouts. scripts/linux/measure.py compares two such sets seed by seed, which averages out the speed difference that only comes from where the linker happened
to put the code (docs/testing/performance-measurement.md). Seed 0 is the linker's own order (no shuffle).
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build-linux-znver2"
OUT = ROOT / "build" / "layouts"


def link_command():
    text = subprocess.run(["ninja", "-t", "commands", "game/uc"], cwd=BUILD, capture_output=True, text=True, check=True).stdout.strip().splitlines()[-1]
    match = re.search(r'cd \. && (.*?) -o game\\uc\b', text)
    if not match:
        sys.exit("cannot find the link command in: " + text[:300])
    return match.group(1)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    label, seeds = sys.argv[1], [int(s) for s in sys.argv[2:]]
    OUT.mkdir(parents=True, exist_ok=True)
    if not (BUILD / "CMakeFiles" / "uc.rsp").exists():   # ninja deletes the response file of a link after it; -d keeprsp keeps it
        (BUILD / "game" / "uc").unlink(missing_ok=True)
        subprocess.run(["ninja", "-d", "keeprsp", "game/uc"], cwd=BUILD, check=True)
    command = link_command()
    for seed in seeds:
        target = OUT / f"{label}_s{seed}"
        shuffle = f" -Wl,--shuffle-sections=*={seed}" if seed else ""
        subprocess.run(f'{command}{shuffle} -o "{target}"', shell=True, cwd=BUILD, check=True)
        print(f"{target.name}: {target.stat().st_size // 1024} KiB", flush=True)
    # the data files next to the game (shaders, seed) are the build folder's own; the bench copies the binary only


main()

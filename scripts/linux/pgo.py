"""Records the optimizer profile for the Steam Deck build: python scripts/linux/pgo.py [--minutes 0.7] [--skip-build]

Builds the instrumented Linux uc (scripts/linux/build.ps1 -PgoGenerate: build-linux-pgogen), copies it to the Deck as ~/uc-run/uc_pgogen, plays the autoplay (8x), controls and fight
scenarios on the Deck in Game Mode (the owner can watch) with LLVM_PROFILE_FILE set, fetches the raw profiles and merges them into project/pgo/uc-linux.profdata. The normal Linux build
(scripts/linux/build.ps1) then uses that file (CMakeLists.txt). The Windows profile (project/pgo/uc.profdata) cannot be used: its function names are MSVC-mangled.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import localenv   # noqa: E402

BUILD = ROOT / "build-linux-pgogen"
PROFILES = BUILD / "profiles"
OUT = ROOT / "project" / "pgo" / "uc-linux.profdata"
REMOTE = "/home/deck/uc-run"


def ssh(command, **kwargs):
    return subprocess.run(["ssh", "-o", "BatchMode=yes", "deck", command], **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--minutes", type=float, default=0.7, help="wall time of the autoplay workload at 8x")
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()
    if not args.skip_build:
        subprocess.run(["pwsh", str(ROOT / "scripts" / "linux" / "build.ps1"), "-Target", "uc", "-PgoGenerate"], check=True, cwd=ROOT)
    with open(BUILD / "game" / "uc", "rb") as source:
        subprocess.run(["ssh", "deck", f"cat > {REMOTE}/uc_pgogen && chmod +x {REMOTE}/uc_pgogen"], stdin=source, check=True)
    ssh(f"rm -rf {REMOTE}/pgo; mkdir -p {REMOTE}/pgo", check=True)
    env = dict(os.environ, UC_DECK_GAMEMODE="1", UC_EXE="./uc_pgogen", LLVM_PROFILE_FILE=f"{REMOTE}/pgo/uc-%p.profraw")
    harness = [sys.executable, "-m", "uctest", "run"]
    for scenario in (["autoplay", "--god", "--speed", "8", "--max-minutes", str(args.minutes)], ["controls", "--speed", "8"],
                     ["fight", "--god", "--wave-offset", "15", "--measure", "8", "--max-minutes", "4"], ["perf", "--measure", "10"]):
        # a scenario that fails still ran the game for a while and its profile is written when the game quits: the training goes on
        subprocess.run(harness + scenario + ["--deck"], cwd=ROOT / "scripts", env=env, check=False)
    shutil.rmtree(PROFILES, ignore_errors=True)
    PROFILES.mkdir(parents=True)
    subprocess.run(["scp", "-q", "-r", f"deck:{REMOTE}/pgo/.", str(PROFILES)], check=True)
    ssh(f"rm -rf {REMOTE}/pgo {REMOTE}/uc_pgogen")
    raw = glob.glob(str(PROFILES / "*.profraw"))
    if not raw:
        sys.exit("no raw profiles were written")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([localenv.llvm_tool("llvm-profdata"), "merge", "--sparse", "-o", str(OUT), *raw], check=True)
    print(f"profile written: {OUT} ({OUT.stat().st_size // 1024} KiB) from {len(raw)} runs. Rebuild with scripts/linux/build.ps1 to use it.")


main()

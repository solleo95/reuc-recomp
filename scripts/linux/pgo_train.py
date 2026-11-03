"""Records the optimizer profile of the Steam Deck (Linux) build from scratch: python scripts/linux/pgo_train.py [--seconds 20] [--no-final-build]

1. builds the instrumented uc (scripts/linux/build.ps1 -Target uc -PgoGenerate, folder build-linux-pgogen);
2. replays the deterministic demos (fight4, zombie-firstscene, opening: build/demos) on the Deck through scripts/linux/deck_bench.py with the owner's settings
   (per-pixel lighting, ultra motion blur, bloom 150) and LLVM_PROFILE_FILE set, so every run writes a raw profile;
3. fetches the raw profiles and merges them into project/pgo/uc-linux.profdata (the old file is kept as build/uc-linux.profdata.previous);
   the profile is made from these runs alone: nothing is carried over from an older profile;
4. builds the normal uc with the new profile (scripts/linux/build.ps1 -Target uc) and leaves it next to the others as build/uc_pgo_symbols.

Needs the Steam Deck on the network (ssh deck), no game running on it, and the environment of scripts/linux/deck_bench.py: UC_DECK_DIR (default ~/Games/UmbrellaChronicles) and
UC_DECK_SHORTCUT (the Steam shortcut id of the game on the Deck). The profile file is local only (project/pgo is not in git). Run it on the final tree, before the release build:
the profile belongs to the source state (docs/testing/performance-measurement.md says why changes are compared without it).
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

DEMOS = ("fight4", "zombie-firstscene", "opening")
PROFILE = ROOT / "project" / "pgo" / "uc-linux.profdata"
RAW = ROOT / "build" / "pgo-raw"
GAME_SETTINGS = ["--per-pixel-lighting", "--motion-blur", "ultra", "--bloom-strength", "150"]


def run(command, **kwargs):
    print(">", " ".join(str(part) for part in command), flush=True)
    subprocess.run(command, check=True, **kwargs)


def ssh(command, **kwargs):
    return subprocess.run(["ssh", "-o", "BatchMode=yes", "deck", command], **kwargs)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=int, default=20, help="game seconds of each demo that are played")
    parser.add_argument("--no-final-build", action="store_true", help="stop after writing the profile")
    args = parser.parse_args()
    env = dict(os.environ)
    env.setdefault("UC_DECK_DIR", "~/Games/UmbrellaChronicles")
    remote = env["UC_DECK_DIR"].replace("~", "$HOME")
    missing = [d for d in DEMOS if not (ROOT / "build" / "demos" / f"{d}.demo").exists()]
    if missing:
        sys.exit(f"missing demos in build/demos: {missing} (python -m uctest demo record <name>)")
    if ssh("pgrep -x 'uc|uc_.*' >/dev/null").returncode == 0:
        sys.exit("a game is running on the Deck: close it first")
    run(["pwsh", "-NoProfile", "-File", str(ROOT / "scripts" / "linux" / "build.ps1"), "-Target", "uc", "-PgoGenerate"], cwd=ROOT)
    instrumented = ROOT / "build" / "uc_pgogen_binary"
    shutil.copyfile(ROOT / "build-linux-pgogen" / "game" / "uc", instrumented)
    ssh(f"rm -rf {remote}/pgo; mkdir -p {remote}/pgo", check=True)
    env["UC_DECK_GAMEMODE"] = "1"
    for demo in DEMOS:
        run([sys.executable, str(ROOT / "scripts" / "linux" / "deck_bench.py"), "--demo", demo, "--runs", "1", "--seconds", str(args.seconds),
             *[f"--game-arg={a}" for a in GAME_SETTINGS], f"pg|env:LLVM_PROFILE_FILE={remote}/pgo/uc-%p.profraw={instrumented}"], cwd=ROOT, env=env)
    shutil.rmtree(RAW, ignore_errors=True)
    RAW.mkdir(parents=True)
    run(["scp", "-q", f"deck:{env['UC_DECK_DIR'].replace('~/', '')}/pgo/*.profraw", str(RAW)])
    ssh(f"rm -rf {remote}/pgo {remote}/uc_pg {remote}/bench/run {remote}/next-run.sh")
    raw = glob.glob(str(RAW / "*.profraw"))
    if not raw:
        sys.exit("no raw profiles were written")
    if PROFILE.exists():
        shutil.copyfile(PROFILE, ROOT / "build" / "uc-linux.profdata.previous")
    PROFILE.parent.mkdir(parents=True, exist_ok=True)
    run([localenv.llvm_tool("llvm-profdata"), "merge", "--sparse", "-o", str(PROFILE), *raw])
    print(f"profile written: {PROFILE} ({PROFILE.stat().st_size // 1024} KiB) from {len(raw)} raw profiles")
    if args.no_final_build:
        return
    run(["pwsh", "-NoProfile", "-File", str(ROOT / "scripts" / "linux" / "build.ps1"), "-Target", "uc"], cwd=ROOT)
    shutil.copyfile(ROOT / "build-linux-znver2" / "game" / "uc", ROOT / "build" / "uc_pgo_symbols")
    print("built with the new profile: build/uc_pgo_symbols (build-linux-znver2/game/uc)")


main()

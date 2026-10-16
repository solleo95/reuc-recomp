"""Records the optimizer profile: python scripts/pgo.py [--minutes 0.7]

Builds an instrumented uc.exe in build-pgo-gen (UC_PGO_GENERATE), runs the autoplay (8x), controls and fight scenarios through the test harness with LLVM_PROFILE_FILE
set, and merges the raw profiles into project/pgo/uc.profdata. Normal builds then use that file automatically (see CMakeLists.txt). Takes one full build."""
import argparse, glob, os, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build-pgo-gen"
PROFILES = BUILD / "profiles"
OUT = ROOT / "project" / "pgo" / "uc.profdata"


def sh(command, **kwargs):
    print(">", command, flush=True)
    subprocess.run('cmd /s /c "' + command + '"', check=True, cwd=ROOT, **kwargs)   # /s keeps the quotes of a path with spaces intact


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--minutes", type=float, default=0.7, help="wall-time of the autoplay workload at 8x")
    args = parser.parse_args()
    env_bat = str(ROOT / "scripts" / "env.bat")
    sh(f'call "{env_bat}" && cmake -S . -B build-pgo-gen -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DUC_PGO_GENERATE=ON && cmake --build build-pgo-gen --target uc')
    staged = ROOT / "dist" / "uc_pgo_gen.exe"   # next to dist/data and dist/assets, which the game needs
    shutil.copyfile(BUILD / "game" / "uc.exe", staged)
    shutil.rmtree(PROFILES, ignore_errors=True)
    PROFILES.mkdir(parents=True)
    env = dict(os.environ, LLVM_PROFILE_FILE=str(PROFILES / "uc-%p.profraw"))
    harness = [sys.executable, "-m", "uctest", "run"]
    # The fight scenario trains the heavy 30-enemy scene (2D HUD layout code, animation) that the plain autoplay passes in a few seconds; it needs
    # the slowdown point saved by an F5 press (scripts/uctest/marks/fight.json).
    for scenario in (["autoplay", "--god", "--speed", "8", "--max-minutes", str(args.minutes)], ["controls", "--speed", "8"],
                     ["fight", "--god", "--measure", "8", "--max-minutes", "4"]):
        subprocess.run(harness + scenario + ["--exe", str(staged)], cwd=ROOT / "scripts", env=env, check=True)
    demo = ROOT / "build" / "demos" / "fight2.demo"
    if demo.exists():   # the deterministic fight (python -m uctest demo record fight2) trains the same heavy scene in every refresh
        subprocess.run([sys.executable, "-m", "uctest", "demo", "replay", "fight2", "--exe", str(staged)], cwd=ROOT / "scripts", env=env, check=True)
    raw = glob.glob(str(PROFILES / "*.profraw"))
    if not raw:
        sys.exit("no raw profiles were written")
    llvm_profdata = shutil.which("llvm-profdata") or r"C:\Program Files\LLVM\bin\llvm-profdata.exe"
    OUT.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([llvm_profdata, "merge", "--sparse", "-o", str(OUT), *raw], check=True)
    staged.unlink(missing_ok=True)
    print(f"profile written: {OUT} ({OUT.stat().st_size // 1024} KiB). Rebuild to use it.")


if __name__ == "__main__":
    main()

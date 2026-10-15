"""Records the optimizer profile: python scripts/pgo.py [--minutes 0.7]

Builds an instrumented uc.exe in build-pgo-gen (UC_PGO_GENERATE), runs the autoplay scenario at 8x through the test harness with LLVM_PROFILE_FILE
set, and merges the raw profiles into project/pgo/uc.profdata. Normal builds then use that file automatically (see CMakeLists.txt). Takes one full build."""
import argparse, glob, os, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build-pgo-gen"
PROFILES = BUILD / "profiles"
OUT = ROOT / "project" / "pgo" / "uc.profdata"


def sh(command, **kwargs):
    print(">", command, flush=True)
    subprocess.run(["cmd", "/c", command], check=True, cwd=ROOT, **kwargs)


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
    for scenario in (["autoplay", "--god", "--speed", "8", "--max-minutes", str(args.minutes)], ["controls", "--speed", "8"]):
        subprocess.run(harness + scenario + ["--exe", str(staged)], cwd=ROOT / "scripts", env=env, check=True)
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

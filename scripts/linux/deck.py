"""Run Linux builds on the Steam Deck (`ssh deck`) and leave nothing behind.

  python scripts/linux/deck.py push <file...>          copy files into ~/uc-test on the Deck
  python scripts/linux/deck.py run <exe> [args...]      push <exe>, run it there, print its output, then clean
  python scripts/linux/deck.py clean [--all]            delete ~/uc-test (--all also ~/uc-game, the extracted game folder)
  python scripts/linux/deck.py selftest                 cross-compile and run a hello program (checks the toolchain and the link)

The Deck's /home has little free space: every command that runs something ends with clean and prints the free space.
"""
import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HOST = "deck"
WORK = "~/uc-test"
GAME = "~/uc-run"   # the staged game: uc, libSDL3, data/ and the extracted game/ folder (copied once, 3.8 GB)
ROOT = Path(__file__).resolve().parents[2]


def ssh(command, check=True, **kwargs):
    return subprocess.run(["ssh", "-o", "BatchMode=yes", HOST, command], check=check, **kwargs)


def push(files):
    """Copy files (by name, flat) into WORK; tar over ssh keeps the executable bit."""
    files = [Path(f) for f in files]
    ssh(f"mkdir -p {WORK}")
    for f in files:
        with subprocess.Popen(["tar", "-cf", "-", "-C", str(f.parent), f.name], stdout=subprocess.PIPE) as tar:
            subprocess.run(["ssh", "-o", "BatchMode=yes", HOST, f"tar -xf - -C {WORK}"], stdin=tar.stdout, check=True)


def clean(everything=False):
    ssh(f"rm -rf {WORK}" + (f" {GAME}" if everything else "") + "; df -h /home | tail -1")


def run(exe, args, env=""):
    exe = Path(exe)
    push([exe])
    try:
        quoted = " ".join("'" + a.replace("'", "'\\''") + "'" for a in args)
        result = ssh(f"cd {WORK} && chmod +x {exe.name} && {env} ./{exe.name} {quoted}", check=False)
        return result.returncode
    finally:
        clean()


def stage(build_dir):
    """Copy uc, SDL3 and data/ from the Linux build into GAME (the game/ folder there stays)."""
    build = Path(build_dir)
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp)
        shutil.copy2(build / "game" / "uc", out / "uc")
        # No libSDL3 here: the Deck's own SDL3 (3.4, with Wayland) is used; the one we build has no Wayland backend.
        shutil.copytree(build / "game" / "data", out / "data")
        launcher = Path(__file__).with_name("uc-deck.sh").read_bytes().replace(b"\r\n", b"\n")   # git on Windows may check it out with CRLF; sh needs LF
        (out / "uc-deck.sh").write_bytes(launcher)   # the Steam / terminal launcher
        ssh(f"mkdir -p {GAME}")
        with subprocess.Popen(["tar", "-cf", "-", "-C", str(out), "."], stdout=subprocess.PIPE) as tar:
            subprocess.run(["ssh", "-o", "BatchMode=yes", HOST, f"tar -xf - -C {GAME} && chmod +x {GAME}/uc {GAME}/uc-deck.sh"], stdin=tar.stdout, check=True)
    ssh(f"du -sh {GAME}; df -h /home | tail -1")


def selftest():
    llvm = Path("C:/Program Files/LLVM/bin/clang++.exe")
    sysroot = "D:/Toolchains/sniper-sysroot"
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "hello.cpp"
        source.write_text('#include <cstdio>\n#include <thread>\nint main() { int n = 0; std::thread t([&] { n = 4; }); t.join(); std::printf("hello linux %d\\n", n); }\n')
        exe = Path(tmp) / "hello"
        subprocess.run([str(llvm), "--target=x86_64-linux-gnu", f"--sysroot={sysroot}", "-fuse-ld=lld", "-std=c++20", "-O2", "-march=x86-64-v3", "-pthread",
                        str(source), "-o", str(exe)], check=True)
        push([exe])
        try:
            r = ssh(f"chmod +x {WORK}/hello && {WORK}/hello", check=False, capture_output=True, text=True)
        finally:
            clean()
        print(r.stdout.strip())
        return 0 if r.returncode == 0 and "hello linux 4" in r.stdout else 1


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("push").add_argument("files", nargs="+")
    r = sub.add_parser("run")
    r.add_argument("exe")
    r.add_argument("args", nargs=argparse.REMAINDER)
    c = sub.add_parser("clean")
    c.add_argument("--all", action="store_true")
    sub.add_parser("selftest")
    sub.add_parser("stage").add_argument("build_dir", nargs="?", default=str(ROOT / "build-linux-znver2"))   # scripts/linux/build.ps1's default folder
    a = p.parse_args()
    if a.cmd == "push":
        push(a.files)
        return 0
    if a.cmd == "run":
        return run(a.exe, a.args)
    if a.cmd == "stage":
        stage(a.build_dir)
        return 0
    if a.cmd == "clean":
        clean(a.all)
        return 0
    return selftest()


if __name__ == "__main__":
    sys.exit(main())

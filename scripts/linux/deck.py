"""Run Linux builds on the Steam Deck (`ssh deck`) and leave nothing behind.

  python scripts/linux/deck.py push <file...>          copy files into ~/uc-test on the Deck
  python scripts/linux/deck.py run <exe> [args...]      push <exe>, run it there, print its output, then clean
  python scripts/linux/deck.py clean [--all]            delete ~/uc-test (--all also ~/uc-game, the extracted game folder)
  python scripts/linux/deck.py package <out>            assemble the Steam Deck release folder from the Linux build (see scripts/build_release_deck.py)
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
        if (build / "launcher" / "uc_launcher").is_file():   # the launcher the owner starts from Steam: a stale one hides new settings
            shutil.copy2(build / "launcher" / "uc_launcher", out / "uc_launcher")
        # No libSDL3 here: the Deck's own SDL3 (3.4, with Wayland) is used; the one we build has no Wayland backend.
        shutil.copytree(build / "game" / "data", out / "data")
        for script in ("uc-deck.sh", "uc-game.sh"):   # git on Windows may check them out with CRLF; sh needs LF
            (out / script).write_bytes(Path(__file__).with_name(script).read_bytes().replace(b"\r\n", b"\n"))   # the Steam / terminal entry; the game's start script
        ssh(f"mkdir -p {GAME}")
        with subprocess.Popen(["tar", "-cf", "-", "-C", str(out), "."], stdout=subprocess.PIPE) as tar:
            subprocess.run(["ssh", "-o", "BatchMode=yes", HOST, f"tar -xf - -C {GAME} && chmod +x {GAME}/uc {GAME}/uc_launcher {GAME}/uc-deck.sh {GAME}/uc-game.sh"], stdin=tar.stdout, check=True)
    ssh(f"du -sh {GAME}; df -h /home | tail -1")


def package_tree(build_dir, out_dir):
    """The folder a Steam Deck release is made of (scripts/build_release_deck.py packs it): uc, the launcher, data/, the launcher's pictures, Wit and the licences.

    Assembled from the Linux build folder rather than by `cmake --install`, which would also want the test tools that are not built. No libSDL3: the Deck's own is used.
    """
    build, out = Path(build_dir), Path(out_dir)
    wit = build / "_deps" / "wit_linux-src"
    parts = {  # target (relative to out) -> source
        "uc": build / "game" / "uc",
        "uc_launcher": build / "launcher" / "uc_launcher",
        "assets/launcher/graphite-train.png": ROOT / "assets" / "launcher" / "graphite-train.png",
        "assets/launcher/graphite-brand.png": ROOT / "assets" / "launcher" / "graphite-brand.png",
        "tools/wit/wit": wit / "bin" / "wit",
        "licenses/wit/gpl-2.0.txt": wit / "gpl-2.0.txt",
        "licenses/wit/wit-source.txt": wit / "doc" / "wit-source.txt",
        "licenses/wit/NOTICE.txt": ROOT / "third_party" / "wit" / "NOTICE.txt",
        "licenses/stb/NOTICE.txt": ROOT / "third_party" / "stb" / "NOTICE.txt",
        # the licences of what is linked into uc and uc_launcher: what the Windows package installs (CMake install(... licenses/...)), minus the Windows-only parts
        "THIRD_PARTY_NOTICES.md": ROOT / "THIRD_PARTY_NOTICES.md",
        "licenses/nlohmann_json/LICENSE.MIT": build / "_deps" / "nlohmann_json-src" / "LICENSE.MIT",
        "licenses/mimalloc/LICENSE": build / "_deps" / "mimalloc-src" / "LICENSE",
        "licenses/imgui/LICENSE.txt": build / "_deps" / "imgui-src" / "LICENSE.txt",
        "licenses/miniz/LICENSE": build / "_deps" / "miniz-src" / "LICENSE",
        "licenses/lua/license-and-readme.html": build / "_deps" / "lua-src" / "doc" / "readme.html",
        "licenses/quickjs-ng/LICENSE": build / "_deps" / "quickjs-src" / "LICENSE",
        "licenses/SMAA/LICENSE.txt": ROOT / "third_party" / "smaa" / "LICENSE.txt",
    }
    missing = [str(source) for source in parts.values() if not source.is_file()]
    if missing or not (build / "game" / "data").is_dir():
        raise FileNotFoundError("build the Linux launcher and game first (scripts/linux/build.ps1); missing: " + ", ".join(missing or [str(build / "game" / "data")]))
    if out.exists():
        shutil.rmtree(out)
    for target, source in parts.items():
        (out / target).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, out / target)
    for script in ("uc-deck.sh", "uc-game.sh"):   # the Steam entry (starts the launcher) and the game's start script (the launcher runs it); sh needs LF, git on Windows may check out CRLF
        (out / script).write_bytes((Path(__file__).with_name(script)).read_bytes().replace(b"\r\n", b"\n"))
    shutil.copytree(build / "game" / "data", out / "data")
    return out


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
    pk = sub.add_parser("package", help="assemble the release folder, then: python scripts/build_release_deck.py --version X.Y.Z --stage <out>")
    pk.add_argument("out")
    pk.add_argument("--build", default=str(ROOT / "build-linux-znver2"))
    a = p.parse_args()
    if a.cmd == "package":
        print(package_tree(a.build, a.out))
        return 0
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

"""Draw the Steam Deck launcher on the Deck and bring its screens back as PNG.

  python scripts/linux/deck_snapshot.py [--build build-linux-znver2] [--out <folder>] [--cwd /]

Pushes uc_launcher and its artwork to ~/uc-test/launcher, runs `uc_launcher --snapshot` there (a 1280x720 window, never fullscreen) from
the given working directory (the default `/` proves the launcher finds its own files), fetches the BMPs, converts them to PNG in <folder>
(default build/deck-snapshots), checks that every screen shows a drawn picture, and cleans the Deck.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
import deck  # noqa: E402  (ssh, WORK, clean)

ROOT = Path(__file__).resolve().parents[2]
DISPLAY_ENV = "XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=gamescope-0 SDL_VIDEODRIVER=wayland"   # Game Mode's gamescope; the harness uses wayland-0 on the desktop


def read_bmp(data: bytes):
    """(width, height, pixels) of an uncompressed 24- or 32-bit BMP; pixels are (r, g, b), row-major, top row first."""
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError("not a BMP")
    offset, header = struct.unpack_from("<II", data, 10)
    width, height, _planes, bits, compression = struct.unpack_from("<iiHHI", data, 18)
    if header < 40 or bits not in (24, 32) or compression not in (0, 3):
        raise ValueError(f"unsupported BMP (header {header}, {bits} bit, compression {compression})")
    top_down = height < 0
    height = abs(height)
    row_bytes = (width * bits // 8 + 3) // 4 * 4
    if offset + row_bytes * height > len(data):
        raise ValueError("truncated BMP")
    step = bits // 8
    rows = []
    for y in range(height):
        start = offset + y * row_bytes
        rows.append([(data[start + x * step + 2], data[start + x * step + 1], data[start + x * step]) for x in range(width)])
    if not top_down:
        rows.reverse()
    return width, height, [pixel for row in rows for pixel in row]


def looks_drawn(width, height, pixels) -> bool:
    """A screen that is one flat colour (a failed first frame) is not a launcher."""
    if not pixels:
        return False
    luminance = [(r * 299 + g * 587 + b * 114) / 1000 for r, g, b in pixels]
    mean = sum(luminance) / len(luminance)
    deviation = (sum((v - mean) ** 2 for v in luminance) / len(luminance)) ** .5
    return len(set(pixels)) >= 16 and deviation > 6


def to_png(width, height, pixels) -> bytes:
    raw = b"".join(b"\0" + b"".join(bytes(pixels[y * width + x]) for x in range(width)) for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default="build-linux-znver2")
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "deck-snapshots")
    parser.add_argument("--cwd", default="/")
    args = parser.parse_args()
    build = ROOT / args.build
    launcher = build / "launcher" / "uc_launcher"
    if not launcher.is_file():
        print(f"missing {launcher}: build it with scripts/linux/build.ps1 -Target uc_launcher", file=sys.stderr)
        return 2
    remote = f"{deck.WORK}/launcher"
    args.out.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            (stage / "assets" / "launcher").mkdir(parents=True)
            (stage / "uc_launcher").write_bytes(launcher.read_bytes())
            for name in ("god_mode", "infinite_ammo", "inspector", "randomizer"):   # what the Mods tab lists (its folder is the snapshot's own, never the install's)
                shutil.copytree(ROOT / "mods-examples" / name, stage / "snap" / "mods" / name)
            (stage / "game" / "DATA" / "sys").mkdir(parents=True)   # a game folder (only its main.dol is looked for), so that the Play button is enabled
            (stage / "game" / "DATA" / "sys" / "main.dol").write_bytes(b"\0")
            demo = stage / "snap" / "mods" / "zz_settings_demo"   # a mod with a choice, a text and a number setting, listed last: the windows that open over a mod's settings
            demo.mkdir()
            (demo / "mod.json").write_text(json.dumps({
                "id": "settings-demo", "name": "Settings Demo", "version": "1.0", "api": "1.0", "script": "main.lua", "description": "Snapshot only.",
                "settings": [
                    {"key": "mode", "type": "choice", "default": "slow", "choices": ["slow", "fast"], "label": {"en": "Mode"}},
                    {"key": "name", "type": "text", "default": "Leon", "label": {"en": "Name"}},
                    {"key": "count", "type": "int", "default": 5, "min": 0, "max": 10, "label": {"en": "Count"}}]}))
            for name in ("graphite-train.png", "graphite-brand.png"):
                (stage / "assets" / "launcher" / name).write_bytes((ROOT / "assets" / "launcher" / name).read_bytes())
            deck.ssh(f"rm -rf {remote} && mkdir -p {remote}")
            with subprocess.Popen(["tar", "-cf", "-", "-C", str(stage), "."], stdout=subprocess.PIPE) as tar:
                subprocess.run(["ssh", "-o", "BatchMode=yes", deck.HOST, f"tar -xf - -C {remote} && chmod +x {remote}/uc_launcher"], stdin=tar.stdout, check=True)
        run = deck.ssh(f"cd {args.cwd} && {DISPLAY_ENV} {remote}/uc_launcher --snapshot {remote}/snap --window 1280x720 2>&1 | tail -n 20", check=False)
        if run.returncode:
            print("launcher failed on the Deck", file=sys.stderr)
            return 1
        with tempfile.TemporaryDirectory() as tmp:
            with subprocess.Popen(["ssh", "-o", "BatchMode=yes", deck.HOST, f"tar -cf - -C {remote}/snap ."], stdout=subprocess.PIPE) as fetch:
                subprocess.run(["tar", "-xf", "-", "-C", tmp], stdin=fetch.stdout, check=True)
            bmps = sorted(Path(tmp).glob("*.bmp"))
            if not bmps:
                print("no screens were written", file=sys.stderr)
                return 1
            bad = 0
            for path in bmps:
                width, height, pixels = read_bmp(path.read_bytes())
                png = args.out / (path.stem + ".png")
                png.write_bytes(to_png(width, height, pixels))
                drawn = looks_drawn(width, height, pixels)
                print(f"{png} {width}x{height} {'ok' if drawn else 'FLAT'}")
                bad += not drawn
            return 1 if bad else 0
    finally:
        deck.ssh(f"rm -rf {remote}; df -h /home | tail -1", check=False)


if __name__ == "__main__":
    sys.exit(main())

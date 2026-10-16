"""python scripts/det_check.py [--exe dist/uc.exe] [--frames 600] [--runs 2]

Runs the game N times from launch in deterministic mode (--det-out: virtual clock, events from other threads delivered only at idle) and compares
the per-retrace hashes of guest memory. Prints the first retrace where two runs differ and the guest address ranges (64 KiB chunks) that differ there.
No input is sent, so the game plays its intro and title screen; the real saves are never touched (each run gets a copy)."""
import argparse
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))
from uctest.client import SAVE_SOURCE  # noqa: E402

CHUNKS = (24 + 64) * 1024 * 1024 // (64 * 1024)
RECORD = 4 + 8 * CHUNKS


def chunk_address(index):
    return 0x80000000 + index * 0x10000 if index < 384 else 0x90000000 + (index - 384) * 0x10000


def read_trace(path):
    data = Path(path).read_bytes()
    frames = []
    for at in range(0, len(data) - RECORD + 1, RECORD):
        number, = struct.unpack_from("<I", data, at)
        frames.append((number, struct.unpack_from(f"<{CHUNKS}Q", data, at + 4)))
    return frames


def compare(a, b):
    lines = [f"frames: run A {len(a)}, run B {len(b)}"]
    common = min(len(a), len(b))
    different = [i for i in range(common) if a[i][1] != b[i][1] or a[i][0] != b[i][0]]
    if not different:
        lines.append(f"IDENTICAL over {common} retraces" + ("" if len(a) == len(b) else " (different length)"))
        return "\n".join(lines), not different and len(a) == len(b)
    first = different[0]
    lines.append(f"first difference at record {first} (retrace A {a[first][0]}, B {b[first][0]}); {len(different)} of {common} records differ")
    chunks = [i for i in range(CHUNKS) if a[first][1][i] != b[first][1][i]]
    ranges = []
    for index in chunks:
        if ranges and ranges[-1][1] == index - 1:
            ranges[-1][1] = index
        else:
            ranges.append([index, index])
    lines.append(f"{len(chunks)} chunks differ at that record:")
    for lo, hi in ranges[:40]:
        lines.append(f"  {chunk_address(lo):08X}..{chunk_address(hi) + 0xFFFF:08X}")
    return "\n".join(lines), False


def run(exe, out, frames, saves, timeout):
    if saves.exists():
        shutil.rmtree(saves)
    shutil.copytree(SAVE_SOURCE, saves)
    args = [str(exe), "--det-out", str(out), "--det-frames", str(frames), "--save-root", str(saves), "--seconds", str(timeout),
            "--mute", "--no-vulkan-layers", "--width", "1280", "--height", "720"]
    start = time.monotonic()
    log = open(out.with_suffix(".log"), "wb")
    process = subprocess.run(args, stdout=log, stderr=log, cwd=Path(exe).parent, timeout=timeout + 60)
    log.close()
    return process.returncode, time.monotonic() - start


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", default=str(ROOT / "dist" / "uc.exe"))
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    folder = ROOT / "build" / "det"
    folder.mkdir(parents=True, exist_ok=True)
    traces = []
    for index in range(args.runs):
        out = folder / f"run{index}.bin"
        code, seconds = run(args.exe, out, args.frames, folder / f"saves{index}", args.timeout)
        traces.append(read_trace(out))
        print(f"run {index}: exit {code}, {len(traces[-1])} retraces in {seconds:.1f} s (log {out.with_suffix('.log')})", flush=True)
    status = 0
    for index in range(1, args.runs):
        text, same = compare(traces[0], traces[index])
        print(f"run 0 vs run {index}:\n{text}")
        status |= 0 if same else 1
    return status


if __name__ == "__main__":
    sys.exit(main())

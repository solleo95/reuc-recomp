"""Makes and inspects the game's movie files (Sofdec .sfd) for mods (docs/modding/video.md).

    python sfd_tool.py make <input video> <output.sfd> [--ffmpeg PATH] [--bitrate KBPS] [--fit pad|stretch|crop] [--force]
    python sfd_tool.py info <file.sfd>

`make` needs ffmpeg (on PATH or given with --ffmpeg) with the mpeg1video and adpcm_adx encoders: it turns any video into the picture the game plays
(MPEG-1, 640x480, 29.97 fps) and sound (ADX, stereo, 32 kHz) and writes them into the game's file layout. Put the result at
mods/<id>/files/Data/movie/<name of the movie to replace>.sfd.
"""
import argparse
import base64
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from dataclasses import dataclass, field
from pathlib import Path

SECTOR = 2048
FRAME_TICKS = 3003        # 90 kHz ticks per picture at 29.97 fps
AUDIO_SECTOR_TICKS = 5040 # 2016 bytes of 32 kHz stereo ADX last 0.056 s
SCR_PER_SECTOR = 14       # the mux-rate ramp of the game's files
VIDEO_LEAD = 45000        # the picture data is sent half a second ahead of the sound
SYSTEM_HEADER = bytes.fromhex("000001bb00098824290620ffc0c004")   # as the game's own movies start
PACK_TAIL = bytes.fromhex("882429")   # the mux rate field
END_CODE = b"\x00\x00\x01\xb9"        # ISO end code: the last sector of the game's movies


class SfdError(ValueError):
    """The file is not a Sofdec pack stream this tool understands."""


def stamp(value, prefix=0x21):
    """A 33-bit time stamp as the 5 bytes MPEG-1 packs and packets use."""
    return bytes([prefix | ((value >> 29) & 0x0E), (value >> 22) & 0xFF, 0x01 | ((value >> 14) & 0xFE), (value >> 7) & 0xFF, 0x01 | ((value << 1) & 0xFE)])


def read_stamp(b):
    return ((b[0] >> 1 & 7) << 30) | (b[1] << 22) | ((b[2] >> 1) << 15) | (b[3] << 7) | (b[4] >> 1)


def pack_header(sector_number):
    return b"\x00\x00\x01\xba" + stamp(sector_number * SCR_PER_SECTOR) + PACK_TAIL


def packet(code, body):
    return b"\x00\x00\x01" + bytes([code]) + struct.pack(">H", len(body)) + body


def header_sector(n):
    padding = b"\x0f" + b"\xff" * (SECTOR - 12 - len(SYSTEM_HEADER) - 6 - 1)
    return pack_header(n) + SYSTEM_HEADER + packet(0xBE, padding)


# Sector 2 of the game's movies: a private packet that starts "SofdecStream" and holds the file name, the encoder's name, the length in milliseconds and the
# picture count, largest and average picture size (little-endian words at offsets 184 to 200). It is copied from MV_002_0.sfd with those fields blanked;
# players such as ffmpeg need it to see that the 0xC0 stream is ADX. (A second packet, CRITAGS, repeats the numbers as text and is left out.)
SOFDEC_SECTOR = zlib.decompress(base64.b64decode(
    "eNpjYEAGjPvZ36EIMATnp6WkJgeXFKUm5iogASZZBqIARL+Cc5CjW4i+e6inQlhqkZ6hnoURTAEHhGJCYuMDzIyMjOfbTzMMF2AT"
    "7OYbH2BkpxfiG2BkYGBuYGFobGhqaLSfSP2+YfEGBkbxBnrFaWXI+h8w/P+vwfiAhZGBQYCBn5kY/YkI/QYmB0BRUsswCkbBKBgF"
    "o2CYAgBBFSk8"))


def sofdec_sector(number, name, pictures, largest, average):
    sector = bytearray(SOFDEC_SECTOR)
    sector[0:12] = pack_header(number)
    sector[64:76] = name.encode("ascii", "replace")[:12].ljust(12, b"\x00")
    sector[76:88] = time.strftime("%Y%m%d%H%M").encode("ascii")
    milliseconds = round(pictures * 1001 / 30)
    sector[184:204] = struct.pack("<5I", milliseconds, milliseconds, pictures, largest, average)
    return bytes(sector)


def picture_starts(es):
    starts, i = [], 0
    while True:
        i = es.find(b"\x00\x00\x01\x00", i)
        if i < 0:
            return starts
        starts.append(i)
        i += 4


def mux(es, adx, name="movie.sfd"):
    """The .sfd for an MPEG-1 elementary stream (the sequence end code is added) and an ADX file, as 2048-byte sectors in time order."""
    es = es + b"\x00\x00\x01\xb7"
    starts = picture_starts(es)
    sizes = [b - a for a, b in zip(starts, starts[1:] + [len(es)])]
    sectors = [header_sector(0), header_sector(1), sofdec_sector(2, name, len(starts), max(sizes, default=0), sum(sizes) // max(len(sizes), 1))]
    vpos = apos = vpic = n_audio = 0
    while vpos < len(es) or apos < len(adx):
        video_time = float("inf")
        if vpos < len(es):
            video_time = min(vpic, len(starts)) * FRAME_TICKS - VIDEO_LEAD   # the next picture's time (the rest of the last one counts as the end)
        audio_time = n_audio * AUDIO_SECTOR_TICKS if apos < len(adx) else float("inf")
        number = len(sectors)
        if video_time <= audio_time:
            has_picture = any(vpos <= p < vpos + 2025 for p in starts[vpic:vpic + 2])
            if has_picture:
                index = vpic + next(k for k, p in enumerate(starts[vpic:vpic + 2]) if vpos <= p < vpos + 2025)
                header, take = stamp(index * FRAME_TICKS), 2025
            else:
                header, take = b"\x0f", 2029
            body = es[vpos:vpos + take]
            body += bytes(take - len(body))
            sectors.append(pack_header(number) + packet(0xE0, header + body))
            vpos += take
            while vpic < len(starts) and starts[vpic] < vpos:
                vpic += 1
        else:
            body = adx[apos:apos + 2016]
            body += bytes(2016 - len(body))
            sectors.append(pack_header(number) + packet(0xC0, b"\x40\x04" + stamp(n_audio * AUDIO_SECTOR_TICKS) + body) + packet(0xBE, b"\x0f"))
            apos += 2016
            n_audio += 1
    sectors.append(END_CODE + bytes(SECTOR - 4))
    assert all(len(s) == SECTOR for s in sectors)
    return b"".join(sectors)


@dataclass
class SfdInfo:
    sectors: int = 0
    video_bytes: bytes = b""
    audio_bytes: bytes = b""
    pictures: int = 0
    picture_pts: list = field(default_factory=list)
    audio_pts: list = field(default_factory=list)
    scr: list = field(default_factory=list)
    kinds: list = field(default_factory=list)   # per sector: header, video, audio or other
    problems: list = field(default_factory=list)

    @property
    def video_sectors(self):
        return self.kinds.count("video")

    @property
    def seconds(self):
        return self.pictures * 1001 / 30000


def parse(data):
    """Reads the sector structure: the streams' bytes, time stamps and the order of the sectors; raises SfdError for anything that is not a pack stream."""
    if not data or len(data) % SECTOR:
        raise SfdError("the file is not a whole number of 2048-byte sectors")
    info = SfdInfo(sectors=len(data) // SECTOR)
    video, audio = bytearray(), bytearray()
    for number in range(info.sectors):
        sector = data[number * SECTOR:(number + 1) * SECTOR]
        if sector[:4] == END_CODE and number == info.sectors - 1:   # the game's files end with a sector that holds only the end code
            info.kinds.append("end")
            continue
        if sector[:4] != b"\x00\x00\x01\xba":
            raise SfdError(f"sector {number} does not start with a pack header")
        if sector[4] >> 4 != 2:
            raise SfdError(f"sector {number} has an MPEG-2 pack header; the game's movies are MPEG-1")
        info.scr.append(read_stamp(sector[4:9]))
        kind, at = None, 12
        while at + 6 <= SECTOR and sector[at:at + 3] == b"\x00\x00\x01":
            code, length = sector[at + 3], struct.unpack(">H", sector[at + 4:at + 6])[0]
            if at + 6 + length > SECTOR:
                raise SfdError(f"a packet in sector {number} is longer than the sector")
            body = sector[at + 6:at + 6 + length]
            at += 6 + length
            if code in (0xE0, 0xC0):
                k = 0
                while k < len(body) and body[k] == 0xFF:
                    k += 1
                if k < len(body) and body[k] >> 6 == 1:
                    k += 2
                pts = None
                if k < len(body) and body[k] >> 4 in (2, 3):
                    pts = read_stamp(body[k:k + 5])
                    k += 5 if body[k] >> 4 == 2 else 10
                elif k < len(body) and body[k] == 0x0F:
                    k += 1
                if code == 0xE0:
                    video += body[k:]
                    if pts is not None:
                        info.picture_pts.append(pts)
                else:
                    audio += body[k:]
                    if pts is not None:
                        info.audio_pts.append(pts)
                kind = kind or ("video" if code == 0xE0 else "audio")
            elif code == 0xBB:
                kind = kind or "header"
            elif code != 0xBE:
                kind = kind or "other"
        if kind is None:
            info.problems.append(f"sector {number} holds no packet")
        info.kinds.append(kind or "other")
    info.video_bytes, info.audio_bytes = bytes(video), bytes(audio)
    info.pictures = len(picture_starts(info.video_bytes))
    return info


# ---- the command line: ffmpeg does the encoding, mux() the container ----

MAX_SECONDS = 30 * 60
DEFAULT_BITRATE = 2500   # kb/s of picture; the game's own movies are around 1700


class ToolError(Exception):
    """An ordinary mistake (missing program, bad file): reported in one line, exit code 1."""


def find_ffmpeg(explicit):
    path = explicit or shutil.which("ffmpeg")
    if not path or (explicit and not Path(path).exists()):
        raise ToolError("ffmpeg was not found: install it and put it on PATH, or give its location with --ffmpeg")
    try:
        encoders = subprocess.run([str(path), "-hide_banner", "-encoders"], capture_output=True, text=True, errors="replace").stdout
    except OSError as error:
        raise ToolError(f"cannot run ffmpeg ({path}): {error}")
    for name in ("mpeg1video", "adpcm_adx"):
        if name not in encoders:
            raise ToolError(f"this ffmpeg has no {name} encoder (needed for the game's movies): use a full build of ffmpeg")
    return str(path)


def probe(ffmpeg, source):
    """(seconds or None, has_audio) of a file; raises ToolError when ffmpeg finds no video in it."""
    text = subprocess.run([ffmpeg, "-hide_banner", "-i", str(source)], capture_output=True, text=True, encoding="utf-8", errors="replace").stderr
    if "Video:" not in text:
        raise ToolError(f"{source}: ffmpeg finds no video in this file")
    match = re.search(r"Duration: (\d+):(\d+):(\d+(?:\.\d+)?)", text)
    seconds = int(match.group(1)) * 3600 + int(match.group(2)) * 60 + float(match.group(3)) if match else None
    return seconds, "Audio:" in text


def run_ffmpeg(ffmpeg, arguments, what):
    result = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", *map(str, arguments)], capture_output=True, text=True, encoding="utf-8", errors="replace")
    if result.returncode != 0:
        lines = [line for line in result.stderr.splitlines() if line.strip()]
        raise ToolError(f"ffmpeg failed to {what}: {lines[-1] if lines else 'no message'}")


FIT_FILTERS = {
    "pad": "scale=640:480:force_original_aspect_ratio=decrease,pad=640:480:(ow-iw)/2:(oh-ih)/2:black,setsar=1",
    "stretch": "scale=640:480,setsar=1",
    "crop": "scale=640:480:force_original_aspect_ratio=increase,crop=640:480,setsar=1",
}


def encode_video(ffmpeg, source, fit, bitrate, workdir):
    out = Path(workdir) / "video.m1v"
    run_ffmpeg(ffmpeg, ["-i", source, "-an", "-vf", FIT_FILTERS[fit], "-r", "30000/1001", "-c:v", "mpeg1video", "-b:v", f"{bitrate}k", "-maxrate", f"{bitrate}k",
                        "-bufsize", "655360", "-bf", "0", "-g", "15", "-pix_fmt", "yuv420p", "-f", "mpeg1video", out], "encode the picture")
    return out


def encode_audio(ffmpeg, source, seconds, has_audio, workdir):
    """An ADX file (stereo, 32 kHz) of exactly `seconds`: the source's sound padded with silence or cut, silence alone when it has none."""
    out = Path(workdir) / "audio.adx"
    if has_audio:
        arguments = ["-i", source, "-vn", "-af", "apad", "-t", f"{seconds:.3f}"]
    else:
        arguments = ["-f", "lavfi", "-i", "anullsrc=r=32000:cl=stereo", "-t", f"{seconds:.3f}"]
    run_ffmpeg(ffmpeg, arguments + ["-ar", "32000", "-ac", "2", "-c:a", "adpcm_adx", "-f", "adx", out], "encode the sound")
    return out


def make(source, output, ffmpeg_path, bitrate, fit, force):
    source, output = Path(source), Path(output)
    if not source.is_file():
        raise ToolError(f"{source}: no such file")
    if output.exists() and not force:
        raise ToolError(f"{output} exists: use --force to overwrite it")
    ffmpeg = find_ffmpeg(ffmpeg_path)
    seconds, has_audio = probe(ffmpeg, source)
    if seconds is not None and seconds > MAX_SECONDS:
        raise ToolError(f"{source} is {seconds / 60:.0f} minutes long: movies longer than 30 minutes are refused")
    with tempfile.TemporaryDirectory() as workdir:
        video = encode_video(ffmpeg, source, fit, bitrate, workdir)
        es = video.read_bytes()
        pictures = len(picture_starts(es))
        if pictures == 0:
            raise ToolError(f"{source}: ffmpeg produced no pictures")
        length = pictures * 1001 / 30000
        adx = encode_audio(ffmpeg, source, length, has_audio, workdir).read_bytes()
        data = mux(es, adx, output.name)
    output.write_bytes(data)
    print(f"wrote {output}: {len(data) / 2**20:.1f} MiB, {length:.1f} s, {pictures} pictures{'' if has_audio else ', silent'}")


def info(path):
    path = Path(path)
    try:
        data = path.read_bytes()
    except OSError as error:
        raise ToolError(f"{path}: {error.strerror or error}")
    try:
        found = parse(data)
    except SfdError as error:
        raise ToolError(f"{path}: {error}")
    print(f"{path}: {len(data) / 2**20:.1f} MiB, {found.sectors} sectors, {found.pictures} pictures, {found.seconds:.1f} s")
    print(f"  video: {len(found.video_bytes)} bytes in {found.video_sectors} sectors; audio: {len(found.audio_bytes)} bytes in {found.kinds.count('audio')} sectors")
    for problem in found.problems[:10]:
        print(f"  problem: {problem}")


def main(argv):
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8")   # file names are Unicode; the console's code page must not break a message
    parser = argparse.ArgumentParser(prog="sfd_tool", description="Makes and inspects the game's movie files (Sofdec .sfd) for mods.")
    commands = parser.add_subparsers(dest="command", required=True)
    make_parser = commands.add_parser("make", help="turn a video into a movie file the game plays (needs ffmpeg)")
    make_parser.add_argument("input")
    make_parser.add_argument("output")
    make_parser.add_argument("--ffmpeg", help="path of ffmpeg (default: the one on PATH)")
    make_parser.add_argument("--bitrate", type=int, default=DEFAULT_BITRATE, help=f"picture bitrate in kb/s (default {DEFAULT_BITRATE})")
    make_parser.add_argument("--fit", choices=sorted(FIT_FILTERS), default="pad", help="how a picture that is not 4:3 fills 640x480: pad with black bars (default), stretch, or crop")
    make_parser.add_argument("--force", action="store_true", help="overwrite an existing output file")
    info_parser = commands.add_parser("info", help="show what a movie file holds (works on the game's own movies)")
    info_parser.add_argument("file")
    args = parser.parse_args(argv[1:])
    try:
        if args.command == "make":
            if not 50 <= args.bitrate <= 20000:
                raise ToolError("--bitrate must be between 50 and 20000 kb/s")
            make(args.input, args.output, args.ffmpeg, args.bitrate, args.fit, args.force)
        else:
            info(args.file)
    except ToolError as error:
        print(f"sfd_tool: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

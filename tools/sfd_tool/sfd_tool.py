"""Makes and inspects the game's movie files (Sofdec .sfd) for mods (docs/modding/video.md).

    python sfd_tool.py make <input video> <output.sfd> [--ffmpeg PATH] [--bitrate KBPS] [--fit pad|stretch|crop] [--force]
    python sfd_tool.py info <file.sfd>

`make` needs ffmpeg (on PATH or given with --ffmpeg) with the mpeg1video and adpcm_adx encoders: it turns any video into the picture the game plays
(MPEG-1, 640x480, 29.97 fps) and sound (ADX, stereo, 32 kHz) and writes them into the game's file layout. Put the result at
mods/<id>/files/Data/movie/<name of the movie to replace>.sfd.
"""
import struct
import sys
from dataclasses import dataclass, field

SECTOR = 2048
FRAME_TICKS = 3003        # 90 kHz ticks per picture at 29.97 fps
AUDIO_SECTOR_TICKS = 5040 # 2016 bytes of 32 kHz stereo ADX last 0.056 s
SCR_PER_SECTOR = 14       # the mux-rate ramp of the game's files
VIDEO_LEAD = 45000        # the picture data is sent half a second ahead of the sound
SYSTEM_HEADER = bytes.fromhex("000001bb00098824290620ffc0c004")   # as the game's own movies start
PACK_TAIL = bytes.fromhex("882429")   # the mux rate field


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


def picture_starts(es):
    starts, i = [], 0
    while True:
        i = es.find(b"\x00\x00\x01\x00", i)
        if i < 0:
            return starts
        starts.append(i)
        i += 4


def mux(es, adx):
    """The .sfd for an MPEG-1 elementary stream (the sequence end code is added) and an ADX file, as 2048-byte sectors in time order."""
    es = es + b"\x00\x00\x01\xb7"
    starts = picture_starts(es)
    sectors = [header_sector(0), header_sector(1)]
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

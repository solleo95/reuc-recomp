"""Spike M7: put a new MPEG-1 video into a Sofdec .sfd, keeping everything else of a template movie (its sectors, audio, headers).

    python tools/spikes/sfd_mux.py info <template.sfd>                      how many video sectors the template has and the byte budget of a new video
    python tools/spikes/sfd_mux.py mux  <template.sfd> <video.m1v> <out.sfd> [audio.adx]
        new video (MPEG-1 elementary stream, no B-frames) in the template's video sectors; with an ADX file (stereo, 32 kHz like the template) its bytes go
        into the template's audio sectors (2016 bytes each, in order; the template's timestamps stay)

The .sfd files of the game are MPEG-1 system streams in 2048-byte sectors (a pack header and one packet each): a few header sectors (system header, the
CRI "SofdecStream" and CRITAGS packets), then video (stream 0xE0) and audio (0xC0, ADX) sectors in the order of play. The new video goes into the template's
video sectors in order, with a PTS on each sector in which a picture starts; the sector order, audio, headers and pack timing stay the template's.
"""
import struct
import sys

SECTOR = 2048
FRAME_TICKS = 3003   # 90 kHz ticks per frame at 29.97 fps


def sectors_of(data):
    return [data[i:i + SECTOR] for i in range(0, len(data) - len(data) % SECTOR, SECTOR)]


def stream_of(sector):
    """The stream id of the first packet after the 12-byte pack header (0xE0 video, 0xC0 audio, ...)."""
    return sector[15] if sector[:4] == b"\x00\x00\x01\xba" and sector[12:15] == b"\x00\x00\x01" else None


def pts_bytes(pts):
    return bytes([0x21 | ((pts >> 29) & 0x0E), (pts >> 22) & 0xFF, 0x01 | ((pts >> 14) & 0xFE), (pts >> 7) & 0xFF, 0x01 | ((pts << 1) & 0xFE)])


def picture_starts(es):
    starts, i = [], 0
    while True:
        i = es.find(b"\x00\x00\x01\x00", i)
        if i < 0:
            return starts
        starts.append(i)
        i += 4


def main(argv):
    template = open(argv[2], "rb").read()
    sectors = sectors_of(template)
    video = [i for i, s in enumerate(sectors) if stream_of(s) == 0xE0]
    if argv[1] == "info":
        print(f"{len(sectors)} sectors, {len(video)} video sectors, at most {len(video) * 2025} bytes of new video")
        return 0
    es = open(argv[3], "rb").read() + b"\x00\x00\x01\xb7"
    if len(es) > len(video) * 2025:
        print(f"the new video is {len(es)} bytes but the template has room for {len(video) * 2025}: encode it at a lower bitrate", file=sys.stderr)
        return 1
    starts = picture_starts(es)
    pos, picture, last = 0, 0, 0   # last: the last sector that carries new data; the file ends there (the player plays until the file runs out)
    out_sectors = list(sectors)
    for i in video:
        sector = bytearray(sectors[i])
        first = next((p for p in starts[picture:] if pos <= p < pos + 2025), None)
        if first is not None:
            index = starts.index(first, picture)
            header = pts_bytes(index * FRAME_TICKS)
            take = 2025
            picture = index + 1
            while picture < len(starts) and starts[picture] < pos + take:   # more pictures start in this sector: they get no PTS of their own
                picture += 1
        else:
            header = b"\x0f"
            take = 2029
            while picture < len(starts) and starts[picture] < pos + take:
                picture += 1
        body = es[pos:pos + take]
        body += b"\x00" * (take - len(body))   # zero stuffing after the sequence end code
        if pos < len(es):
            last = max(last, i)
        pos += take
        sector[12:] = b"\x00\x00\x01\xe0" + struct.pack(">H", 2030) + header + body
        assert len(sector) == SECTOR
        out_sectors[i] = bytes(sector)
    if len(argv) > 5:
        adx = open(argv[5], "rb").read()
        audio = [i for i, sector in enumerate(sectors) if stream_of(sector) == 0xC0]
        if len(adx) > len(audio) * 2016:
            print(f"the audio is {len(adx)} bytes but the template has room for {len(audio) * 2016}", file=sys.stderr)
            return 1
        for n, i in enumerate(audio):
            sector = bytearray(out_sectors[i])
            chunk = adx[n * 2016:(n + 1) * 2016]
            sector[18 + 7:18 + 7 + 2016] = chunk + bytes(2016 - len(chunk))   # after the packet's STD and PTS bytes; zero bytes are silent ADX frames
            out_sectors[i] = bytes(sector)
            if n * 2016 < len(adx):
                last = max(last, i)
    open(argv[4], "wb").write(b"".join(out_sectors[:last + 1]))
    print(f"{len(starts)} pictures in {pos} bytes written into {len(video)} video sectors")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

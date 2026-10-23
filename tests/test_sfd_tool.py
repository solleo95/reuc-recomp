"""sfd_tool.py: the writer and reader on synthetic streams (no ffmpeg), and make/info with ffmpeg when one is installed."""
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "sfd_tool"))
import sfd_tool  # noqa: E402

TOOL = ROOT / "tools" / "sfd_tool" / "sfd_tool.py"
FFMPEG = shutil.which("ffmpeg")


def synthetic_es(sizes, seed=1):
    """A sequence header and one picture per size: the start code and `size` bytes that never form a start code."""
    rng = random.Random(seed)
    es = bytearray(b"\x00\x00\x01\xb3" + bytes([0x28, 0x01, 0xe0, 0x14, 0xff, 0xff, 0xe0, 0x00]))
    for size in sizes:
        es += b"\x00\x00\x01\x00" + bytes(rng.randrange(0x10, 0x100) for _ in range(size))
    return bytes(es)


def synthetic_adx(frames):
    return b"\x80\x00\x00\x20\x03\x12\x04\x02\x00\x00\x7d\x00\x00\x01\x00\x00" + bytes(16) + bytes((i * 7) % 251 + 1 for i in range(36 * frames))


def sizes_mixed():
    return [3000 if i % 9 == 0 else 40 + (i * 13) % 200 for i in range(40)]


class WriterReader(unittest.TestCase):
    def setUp(self):
        self.es = synthetic_es(sizes_mixed())
        self.adx = synthetic_adx(4000)
        self.sfd = sfd_tool.mux(self.es, self.adx)
        self.info = sfd_tool.parse(self.sfd)

    def test_mux_output_is_whole_sectors(self):
        self.assertEqual(len(self.sfd) % 2048, 0)
        for i in range(0, len(self.sfd) - 2048, 2048):
            self.assertEqual(self.sfd[i:i + 4], b"\x00\x00\x01\xba", i)
        self.assertEqual(self.sfd[-2048:], b"\x00\x00\x01\xb9" + bytes(2044))   # the last sector holds the end code, as in the game's movies
        self.assertEqual(self.info.sectors, len(self.sfd) // 2048)
        self.assertTrue(all(b > a for a, b in zip(self.info.scr, self.info.scr[1:])))

    def test_video_payload_round_trips(self):
        video = self.info.video_bytes
        self.assertTrue(video.startswith(self.es))
        tail = video[len(self.es):]
        self.assertEqual(tail[:4], b"\x00\x00\x01\xb7")
        self.assertFalse(any(tail[4:]))
        self.assertEqual(self.info.pictures, 40)

    def test_audio_payload_round_trips(self):
        audio = self.info.audio_bytes
        self.assertTrue(audio.startswith(self.adx))
        self.assertFalse(any(audio[len(self.adx):]))
        self.assertEqual(len(audio) % 2016, 0)

    def test_pts_rules(self):
        self.assertEqual(self.info.picture_pts[0], 0)
        starts = [m for m in range(len(self.es)) if self.es.startswith(b"\x00\x00\x01\x00", m)]
        self.assertEqual(len(starts), 40)
        for pts in self.info.picture_pts:
            self.assertEqual(pts % 3003, 0)
            self.assertLess(pts // 3003, 40)
        self.assertEqual(self.info.picture_pts, sorted(self.info.picture_pts))   # no B-frames: increasing
        self.assertEqual(self.info.audio_pts, [5040 * n for n in range(len(self.info.audio_pts))])
        # a sector in which no picture starts has no PTS: there are more video sectors than PTS values
        self.assertLess(len(self.info.picture_pts), self.info.video_sectors)

    def test_streams_are_interleaved(self):
        longest, run, last = 0, 0, None
        last_video = len(self.info.kinds) - 1 - self.info.kinds[::-1].index("video")
        self.assertEqual(self.info.kinds[-1], "end")
        for kind in self.info.kinds[:last_video + 1]:   # while both streams have data
            run = run + 1 if kind == last else 1
            last = kind
            longest = max(longest, run if kind in ("video", "audio") else 0)
        self.assertLessEqual(longest, 40)

    def test_header_sectors_are_first(self):
        self.assertEqual(self.sfd[12:27], b"\x00\x00\x01\xbb\x00\x09\x88\x24\x29\x06\x20\xff\xc0\xc0\x04")
        self.assertEqual(self.info.kinds[:2], ["header", "header"])

    def test_long_audio_and_short_video_and_the_reverse(self):
        short_video = synthetic_es([60] * 5)
        long_audio = synthetic_adx(20000)
        a = sfd_tool.parse(sfd_tool.mux(short_video, long_audio))
        self.assertTrue(a.audio_bytes.startswith(long_audio) and a.video_bytes.startswith(short_video))
        self.assertEqual(a.kinds[-2:], ["audio", "end"])   # the file ends after the last sector that carries data, with the end code
        long_video = synthetic_es([2500] * 60)
        short_audio = synthetic_adx(100)
        b = sfd_tool.parse(sfd_tool.mux(long_video, short_audio))
        self.assertTrue(b.video_bytes.startswith(long_video) and b.audio_bytes.startswith(short_audio))
        self.assertEqual(b.kinds[-2:], ["video", "end"])
        for result in (a, b):
            self.assertNotIn("other", result.kinds)
            self.assertEqual(result.problems, [])

    def test_parse_rejects_foreign_files(self):
        pack = b"\x00\x00\x01\xba\x21\x00\x01\x00\x01\x88\x24\x29"
        bad_packet = bytearray(self.sfd[:2048])
        bad_packet[16:18] = struct.pack(">H", 5000)   # a packet longer than its sector (sector 0 holds the system header at offset 12, padding after)
        cases = {
            "empty": b"",
            "ten": bytes(10),
            "random": bytes(random.Random(3).randrange(256) for _ in range(4096)),
            "no pack header": bytes(2048),
            "odd length": self.sfd[:2047],
            "long packet": bytes(pack + b"\x00\x00\x01\xe0" + struct.pack(">H", 5000) + bytes(2048 - 18)),
        }
        for name, data in cases.items():
            with self.assertRaises(sfd_tool.SfdError, msg=name):
                sfd_tool.parse(data)


if __name__ == "__main__":
    unittest.main()

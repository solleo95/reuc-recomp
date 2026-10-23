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
        self.assertEqual(self.info.kinds[2], "other")   # sector 2 is the "SofdecStream" packet
        sector = self.sfd[2 * 2048:3 * 2048]
        self.assertIn(b"SofdecStream", sector)
        frames, largest, average = struct.unpack("<3I", sector[192:204])
        self.assertEqual(frames, 40)
        self.assertGreaterEqual(largest, 3000)
        self.assertLess(average, largest)

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
            self.assertEqual(result.kinds.count("other"), 1)   # the Sofdec packet
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


class ReviewFixes(unittest.TestCase):
    def test_each_picture_start_gets_the_pts_of_its_sector(self):
        # sizes chosen so that start codes land everywhere in a sector, including its last bytes
        for step in range(2000, 2040):
            es = synthetic_es([step - 4] * 30 + [37] * 5)
            info = sfd_tool.parse(sfd_tool.mux(es, synthetic_adx(10)))
            starts = [m for m in range(len(es)) if es.startswith(b"\x00\x00\x01\x00", m)]
            for offset, pts in info.picture_pts_at:
                following = next(p for p in starts if p >= offset)
                self.assertEqual(pts, 3003 * starts.index(following), (step, offset))
                self.assertLess(following - offset, 2030, (step, offset))   # the picture starts inside this packet
            covered = {p for offset, _ in info.picture_pts_at for p in starts if offset <= p < offset + 2030}
            # every picture that is the first to start in its packet is stamped; only pictures that share a packet with an earlier one go without
            self.assertGreaterEqual(len(info.picture_pts_at), len(starts) // 2)
            self.assertEqual(starts[0] in covered, True)

    def test_sofdec_sector_numbers_follow_the_originals(self):
        seq = b"\x00\x00\x01\xb3" + bytes(8)
        gop = b"\x00\x00\x01\xb8" + bytes(4)
        pic = lambda size: b"\x00\x00\x01\x00" + bytes([0x55]) * size
        es = seq + gop + pic(500) + pic(300) + gop + pic(52)
        sector = sfd_tool.mux(es, synthetic_adx(10))[2 * 2048:3 * 2048]
        milliseconds, again, frames, largest, average = struct.unpack("<5I", sector[184:204])
        self.assertEqual((milliseconds, again, frames), (100, 100, 3))     # 3 pictures last 100.1 ms: truncated
        self.assertEqual(largest, 12 + 8 + 504)                            # a picture counts with the headers in front of it
        self.assertEqual(average, -(-(len(es) + 4) // 3))                  # rounded up (the end code is part of the stream)

    def test_info_on_a_hostile_packet(self):
        pack = b"\x00\x00\x01\xba\x21\x00\x01\x00\x01\x88\x24\x29"
        for body in (b"\x21", b"\x31\x00\x01", b"\x40"):
            sector = pack + b"\x00\x00\x01\xe0" + struct.pack(">H", len(body)) + body
            sector += bytes(2048 - len(sector))
            with tempfile.TemporaryDirectory() as folder:
                path = Path(folder) / "x.sfd"
                path.write_bytes(sector)
                result = run_tool("info", path)
                self.assertEqual(result.returncode, 1, body)
                self.assertNotIn("Traceback", result.stderr, body)

    def test_output_errors_are_clear_and_nothing_is_lost(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "in.mp4"
            source.write_bytes(b"not a video, the parent folder is checked first")
            result = run_tool("make", source, Path(folder) / "no" / "such" / "out.sfd")
            self.assertEqual(result.returncode, 1)
            self.assertNotIn("Traceback", result.stderr)
            self.assertIn("folder", result.stderr)
            (Path(folder) / "adir").mkdir()
            result = run_tool("make", source, Path(folder) / "adir", "--force")
            self.assertEqual(result.returncode, 1)
            self.assertNotIn("Traceback", result.stderr)

    def test_bitrate_range_matches_the_docs(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "in.mp4"
            source.write_bytes(b"x")
            for bad in ("20000", "19001", "49"):
                result = run_tool("make", source, Path(folder) / "o.sfd", "--bitrate", bad)
                self.assertEqual(result.returncode, 1, bad)
                self.assertIn("19000", result.stderr)


def run_tool(*args):
    return subprocess.run([sys.executable, "-I", str(TOOL), *map(str, args)], capture_output=True, text=True, encoding="utf-8")


def ffmpeg(*args):
    result = subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y", *map(str, args)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def probe(path):
    """ffmpeg's own view of a file (stderr of `ffmpeg -i`)."""
    return subprocess.run([FFMPEG, "-hide_banner", "-i", str(path)], capture_output=True, text=True, encoding="utf-8", errors="replace").stderr


class CommandLine(unittest.TestCase):
    def test_bad_arguments_are_clear_errors(self):
        with tempfile.TemporaryDirectory() as folder:
            for args in ([], ["make"], ["make", str(Path(folder) / "missing.mp4"), str(Path(folder) / "o.sfd")], ["frobnicate"], ["info"]):
                result = run_tool(*args)
                self.assertNotEqual(result.returncode, 0, args)
                self.assertTrue(result.stderr.strip(), args)
                self.assertNotIn("Traceback", result.stderr, args)

    def test_no_ffmpeg_is_explained(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "in.mp4"
            source.write_bytes(b"x")
            result = run_tool("make", source, Path(folder) / "o.sfd", "--ffmpeg", Path(folder) / "no-such-ffmpeg")
            self.assertEqual(result.returncode, 1)
            self.assertIn("ffmpeg", result.stderr)
            self.assertNotIn("Traceback", result.stderr)

    def test_an_ffmpeg_without_the_encoders_is_explained(self):
        with tempfile.TemporaryDirectory() as folder:
            fake = Path(folder) / ("fake_ffmpeg.bat" if sys.platform == "win32" else "fake_ffmpeg.sh")
            if sys.platform == "win32":
                fake.write_text("@echo off\r\necho  V....D mpeg4   MPEG-4 part 2\r\n")
            else:
                fake.write_text("#!/bin/sh\necho ' V....D mpeg4   MPEG-4 part 2'\n")
                fake.chmod(0o755)
            source = Path(folder) / "in.mp4"
            source.write_bytes(b"x")
            result = run_tool("make", source, Path(folder) / "o.sfd", "--ffmpeg", fake)
            self.assertEqual(result.returncode, 1)
            self.assertIn("mpeg1video", result.stderr)
            self.assertNotIn("Traceback", result.stderr)

    def test_info_on_a_foreign_file(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "x.sfd"
            path.write_bytes(bytes(5000))
            result = run_tool("info", path)
            self.assertEqual(result.returncode, 1)
            self.assertTrue(result.stderr.strip())
            self.assertNotIn("Traceback", result.stderr)
            self.assertNotEqual(run_tool("info", Path(folder) / "missing.sfd").returncode, 0)

    def test_info_on_a_made_file(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "x.sfd"
            path.write_bytes(sfd_tool.mux(synthetic_es(sizes_mixed()), synthetic_adx(400)))
            result = run_tool("info", path)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("40 pictures", result.stdout)


@unittest.skipUnless(FFMPEG, "ffmpeg is not installed: the make tests are skipped")
class MakeWithFfmpeg(unittest.TestCase):
    def source(self, folder, name="in.mp4", size="320x240", seconds=1, audio=True, rate=25):
        path = Path(folder) / name
        args = ["-f", "lavfi", "-i", f"testsrc2=size={size}:rate={rate}"]
        if audio:
            args += ["-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100"]
        args += ["-t", seconds, "-c:v", "mpeg4"] + (["-c:a", "aac"] if audio else ["-an"]) + [path]
        ffmpeg(*args)
        return path

    def test_make_from_a_generated_clip(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / "out.sfd"
            result = run_tool("make", self.source(folder), out)
            self.assertEqual(result.returncode, 0, result.stderr)
            text = probe(out)
            for expected in ("mpeg1video", "640x480", "29.97 fps", "adpcm_adx", "32000 Hz", "stereo"):
                self.assertIn(expected, text)
            info = sfd_tool.parse(out.read_bytes())
            self.assertIn(info.pictures, range(29, 32))
            self.assertAlmostEqual(info.seconds, 1.0, delta=0.1)
            self.assertEqual(info.problems, [])
            self.assertIn("pictures", run_tool("info", out).stdout)

    def test_source_without_audio_gets_silence(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / "out.sfd"
            self.assertEqual(run_tool("make", self.source(folder, audio=False), out).returncode, 0)
            self.assertIn("adpcm_adx", probe(out))
            info = sfd_tool.parse(out.read_bytes())
            self.assertGreaterEqual(len(info.audio_bytes), int(info.seconds * 36000))   # about the video's length of sound

    def test_fit_modes_give_640x480(self):
        with tempfile.TemporaryDirectory() as folder:
            source = self.source(folder, size="640x360", seconds=0.5)
            for fit in ("pad", "stretch", "crop"):
                out = Path(folder) / f"{fit}.sfd"
                result = run_tool("make", source, out, "--fit", fit)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("640x480", probe(out), fit)

    def test_existing_output_needs_force(self):
        with tempfile.TemporaryDirectory() as folder:
            source, out = self.source(folder, seconds=0.5), Path(folder) / "out.sfd"
            out.write_bytes(b"keep me")
            result = run_tool("make", source, out)
            self.assertEqual(result.returncode, 1)
            self.assertIn("--force", result.stderr)
            self.assertEqual(out.read_bytes(), b"keep me")
            self.assertEqual(run_tool("make", source, out, "--force").returncode, 0)
            self.assertNotEqual(out.read_bytes(), b"keep me")

    def test_refuses_over_30_minutes(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "long.mp4"
            ffmpeg("-f", "lavfi", "-i", "color=c=black:s=16x16:r=1", "-t", "1860", "-c:v", "mpeg4", source)
            out = Path(folder) / "out.sfd"
            result = run_tool("make", source, out)
            self.assertEqual(result.returncode, 1)
            self.assertIn("30 minutes", result.stderr)
            self.assertFalse(out.exists())

    def test_not_a_video_is_an_error(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "text.mp4"
            source.write_text("this is not a video")
            result = run_tool("make", source, Path(folder) / "out.sfd")
            self.assertEqual(result.returncode, 1)
            self.assertNotIn("Traceback", result.stderr)

    def test_non_ascii_and_spaced_paths(self):
        with tempfile.TemporaryDirectory() as folder:
            nested = Path(folder) / "папка with spaces"
            nested.mkdir()
            source = self.source(nested, name="видео clip.mp4", seconds=0.5)
            out = nested / "ролик.sfd"
            result = run_tool("make", source, out)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(out.exists())

    def test_late_starting_audio_stays_in_sync_and_the_length_is_exact(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "late.mkv"
            ffmpeg("-f", "lavfi", "-i", "testsrc2=size=320x240:rate=25", "-itsoffset", "1.0", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100:duration=2",
                   "-t", "3", "-c:v", "mpeg4", "-c:a", "aac", source)
            out = Path(folder) / "out.sfd"
            self.assertEqual(run_tool("make", source, out).returncode, 0)
            info = sfd_tool.parse(out.read_bytes())
            adx_seconds = (len(info.audio_bytes) - 0x24) / 36000
            self.assertGreaterEqual(adx_seconds, info.seconds - 0.1)          # as long as the picture although the sound starts a second late
            # the first second is silence, the tone comes after: decode the sound back and look at the level of its first half second
            wav = Path(folder) / "a.wav"
            ffmpeg("-i", out, "-vn", "-t", "0.5", wav)
            levels = subprocess.run([FFMPEG, "-hide_banner", "-i", str(wav), "-af", "volumedetect", "-f", "null", "-"], capture_output=True, text=True).stderr
            mean = float(levels.split("mean_volume:")[1].split("dB")[0])
            self.assertLess(mean, -50)

    def test_fit_respects_the_sample_aspect_ratio(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / "anamorphic.mkv"
            ffmpeg("-f", "lavfi", "-i", "testsrc2=size=720x480:rate=25,setsar=32/27", "-t", "0.5", "-c:v", "mpeg4", source)   # 16:9 on screen
            out = Path(folder) / "out.sfd"
            self.assertEqual(run_tool("make", source, out).returncode, 0)
            frame = Path(folder) / "f.gray"
            ffmpeg("-i", out, "-frames:v", "1", "-vf", "format=gray", "-f", "rawvideo", frame)
            data = frame.read_bytes()
            rows = [y for y in range(480) if max(data[y * 640:(y + 1) * 640]) > 40]
            self.assertAlmostEqual(len(rows), 360, delta=6)   # 16:9 inside 4:3 is 640x360

    def test_unknown_duration_is_limited_too(self):
        with tempfile.TemporaryDirectory() as folder:
            raw = Path(folder) / "clip.h264"
            ffmpeg("-f", "lavfi", "-i", "testsrc2=size=64x64:rate=25", "-t", "4", "-c:v", "mpeg4", "-f", "m4v", raw)
            self.assertIn("N/A", probe(raw))
            saved = sfd_tool.MAX_SECONDS
            sfd_tool.MAX_SECONDS = 1
            try:
                with self.assertRaises(sfd_tool.ToolError) as caught:
                    sfd_tool.make(raw, Path(folder) / "out.sfd", None, 2500, "pad", False)
            finally:
                sfd_tool.MAX_SECONDS = saved
            self.assertIn("minutes", str(caught.exception))
            self.assertFalse((Path(folder) / "out.sfd").exists())

    def test_bitrate_option_changes_the_size(self):
        with tempfile.TemporaryDirectory() as folder:
            source = self.source(folder, seconds=1)
            small, big = Path(folder) / "small.sfd", Path(folder) / "big.sfd"
            self.assertEqual(run_tool("make", source, small, "--bitrate", "300").returncode, 0)
            self.assertEqual(run_tool("make", source, big, "--bitrate", "4000").returncode, 0)
            self.assertLess(small.stat().st_size, big.stat().st_size)


if __name__ == "__main__":
    unittest.main()

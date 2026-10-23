"""arc_tool.py on synthetic archives; no game data required."""
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "arc_tool" / "arc_tool.py"


def run(*args):
    return subprocess.run([sys.executable, "-I", str(TOOL), *map(str, args)], capture_output=True, text=True, encoding="utf-8")


def tree(folder):
    return {p.relative_to(folder).as_posix(): p.read_bytes() for p in Path(folder).rglob("*") if p.is_file()}


class ArcToolTests(unittest.TestCase):
    def test_pack_then_unpack_gives_the_same_files(self):
        with tempfile.TemporaryDirectory() as folder:
            src = Path(folder) / "src"
            files = {"a.txt": b"alpha", "Dir/x.bin": b"x" * 40, "Dir/sub/y.bin": b"", "z/ün.bin": b"umlaut"}
            for name, content in files.items():
                (src / name).parent.mkdir(parents=True, exist_ok=True)
                (src / name).write_bytes(content)
            arc = Path(folder) / "out.arc"
            self.assertEqual(run("pack", src, arc).returncode, 0)
            listing = run("list", arc)
            self.assertEqual(listing.returncode, 0)
            self.assertEqual(sorted(line.split("\t")[1] for line in listing.stdout.splitlines()), sorted(files))
            out = Path(folder) / "unpacked"
            self.assertEqual(run("unpack", arc, out).returncode, 0)
            self.assertEqual(tree(out), files)

    def test_packed_layout_follows_the_game_format(self):
        with tempfile.TemporaryDirectory() as folder:
            src = Path(folder) / "src"
            (src / "d").mkdir(parents=True)
            (src / "d" / "one.bin").write_bytes(b"1" * 33)
            (src / "two.bin").write_bytes(b"2")
            arc = Path(folder) / "out.arc"
            self.assertEqual(run("pack", src, arc).returncode, 0)
            data = arc.read_bytes()
            self.assertEqual(data[:4], b"\x55\xAA\x38\x2D")
            root_off, tables, data_off = struct.unpack_from(">III", data, 4)
            self.assertEqual(root_off, 0x20)
            self.assertEqual(data_off % 0x20, 0)
            self.assertGreaterEqual(data_off, root_off + tables)
            count = struct.unpack_from(">I", data, root_off + 8)[0]
            self.assertEqual(count, 4)   # root, d, d/one.bin, two.bin
            for i in range(1, count):
                flag, a, b = struct.unpack_from(">B3xII", data, root_off + i * 12)
                if flag == 0:
                    self.assertEqual(a % 0x20, 0)
                    self.assertLessEqual(a + b, len(data))
            self.assertEqual(len(data) % 0x20, 0)

    def test_unpack_refuses_a_filled_folder_and_bad_archives(self):
        with tempfile.TemporaryDirectory() as folder:
            src = Path(folder) / "src"
            src.mkdir()
            (src / "a.bin").write_bytes(b"a")
            arc = Path(folder) / "out.arc"
            self.assertEqual(run("pack", src, arc).returncode, 0)
            taken = Path(folder) / "taken"
            taken.mkdir()
            (taken / "keep.txt").write_bytes(b"k")
            self.assertNotEqual(run("unpack", arc, taken).returncode, 0)
            self.assertEqual((taken / "keep.txt").read_bytes(), b"k")
            good = arc.read_bytes()
            cases = {"magic": b"\x00" + good[1:], "short": good[:10], "cut": good[:-40], "empty": b""}
            for name, content in cases.items():
                path = Path(folder) / f"{name}.arc"
                path.write_bytes(content)
                result = run("list", path)
                self.assertNotEqual(result.returncode, 0, name)
                self.assertTrue(result.stderr.strip(), name)
            self.assertNotEqual(run("list", Path(folder) / "missing.arc").returncode, 0)

    def test_unpack_refuses_names_that_leave_the_folder(self):
        with tempfile.TemporaryDirectory() as folder:
            src = Path(folder) / "src"
            src.mkdir()
            (src / "abcd.bin").write_bytes(b"x")
            arc = Path(folder) / "out.arc"
            self.assertEqual(run("pack", src, arc).returncode, 0)
            data = arc.read_bytes().replace(b"abcd.bin", b"..\\x.bin")   # a backslash name, same length
            evil = Path(folder) / "evil.arc"
            evil.write_bytes(data)
            out = Path(folder) / "out"
            self.assertNotEqual(run("unpack", evil, out).returncode, 0)
            self.assertFalse(out.exists())

    def test_pack_of_an_empty_folder_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "empty").mkdir()
            self.assertNotEqual(run("pack", Path(folder) / "empty", Path(folder) / "x.arc").returncode, 0)


if __name__ == "__main__":
    unittest.main()

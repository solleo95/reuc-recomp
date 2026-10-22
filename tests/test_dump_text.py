"""dump_text.py on a synthetic text file; no game data required."""
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "dump_text" / "dump_text.py"


def txtres(records):
    """records: (id, voice, [lines]); header, table, records, pool with a leftover block (like the game's files)."""
    count = len(records)
    data = bytearray(b"TRES" + struct.pack(">III", 0x00010000, count, 0x10) + bytes(4 * count + 20 * count))
    data += b"leftover\0"
    for i, (rid, voice, lines) in enumerate(records):
        rec = 0x10 + 4 * count + 20 * i
        struct.pack_into(">I", data, 0x10 + 4 * i, rec)
        voice_at = 0
        if voice:
            voice_at = len(data)
            data += voice.encode() + b"\0"
        text_at = len(data)
        for line in lines:
            data += line.encode() + b"\0"
        struct.pack_into(">5I", data, rec, rid, voice_at, len(lines), text_at, 0)
    return bytes(data)


def run(path, *extra):
    return subprocess.run([sys.executable, "-I", str(TOOL), str(path), *extra], capture_output=True)


class DumpTextTests(unittest.TestCase):
    def test_dump_is_json_with_ids_and_line_breaks(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "mes_xx.txtres"
            path.write_bytes(txtres([(1, "", ["Enter"]), (5, "VO_5", ["first", "second %s"]), (9, "", ["日本語 ü"])]))
            result = run(path)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout.decode("utf-8")), {"1": "Enter", "5": "first\nsecond %s", "9": "日本語 ü"})
            self.assertTrue(result.stdout.decode("utf-8").index('"1"') < result.stdout.decode("utf-8").index('"9"'))   # ascending ids

    def test_output_file(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "mes_xx.txtres"
            path.write_bytes(txtres([(2, "", ["Two"])]))
            out = Path(folder) / "us.json"
            result = run(path, "--output", str(out))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(out.read_text(encoding="utf-8")), {"2": "Two"})

    def test_damaged_files_are_refused_with_a_message(self):
        with tempfile.TemporaryDirectory() as folder:
            good = txtres([(1, "", ["a"]), (2, "", ["b"])])
            cases = {"empty": b"", "magic": b"XXXX" + good[4:], "truncated": good[:30], "pointer": good[:0x10 + 8] + struct.pack(">I", 0) + good[0x10 + 12:]}
            bad_pointer = bytearray(good)
            struct.pack_into(">I", bad_pointer, 0x10 + 4 * 2 + 12, len(good) + 100)   # text pointer of record 0 past the end
            cases["pointer"] = bytes(bad_pointer)
            for name, content in cases.items():
                path = Path(folder) / f"{name}.txtres"
                path.write_bytes(content)
                result = run(path)
                self.assertNotEqual(result.returncode, 0, name)
                self.assertTrue(result.stderr.strip(), name)
            self.assertNotEqual(run(Path(folder) / "missing.txtres").returncode, 0)


if __name__ == "__main__":
    unittest.main()

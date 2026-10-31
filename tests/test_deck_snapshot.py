"""Unit tests for the BMP helpers of scripts/linux/deck_snapshot.py (the live run on the Deck is manual: python scripts/linux/deck_snapshot.py)."""
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

REPO = Path(sys.argv.pop(1)).resolve()
spec = importlib.util.spec_from_file_location("deck_snapshot", REPO / "scripts/linux/deck_snapshot.py")
snap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(snap)


def bmp(width, height, pixel, bits=32, top_down=False):
    """A BMP with a BITMAPINFOHEADER; pixel(x, y) -> (r, g, b)."""
    row_bytes = (width * bits // 8 + 3) // 4 * 4
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            r, g, b = pixel(x, y)
            row += bytes((b, g, r)) + (b"\xff" if bits == 32 else b"")
        row += b"\0" * (row_bytes - len(row))
        rows.append(bytes(row))
    data = b"".join(rows if top_down else rows[::-1])
    header = struct.pack("<2sIHHI", b"BM", 14 + 40 + len(data), 0, 0, 14 + 40)
    info = struct.pack("<IiiHHIIiiII", 40, width, -height if top_down else height, 1, bits, 0, len(data), 2835, 2835, 0, 0)
    return header + info + data


class BmpTests(unittest.TestCase):
    def test_reads_size_and_pixels_both_orientations_and_depths(self):
        for bits in (24, 32):
            for top_down in (False, True):
                data = bmp(5, 3, lambda x, y: (x * 10, y * 20, 7), bits, top_down)
                width, height, pixels = snap.read_bmp(data)
                self.assertEqual((width, height), (5, 3))
                self.assertEqual(pixels[2 * 5 + 4], (40, 40, 7), (bits, top_down))   # row-major, top row first

    def test_rejects_other_formats(self):
        with self.assertRaises(ValueError):
            snap.read_bmp(b"PNG" + b"\0" * 100)
        with self.assertRaises(ValueError):
            snap.read_bmp(bmp(2, 2, lambda x, y: (0, 0, 0), 16 if False else 24)[:30])

    def test_uniform_picture_is_flagged_and_varied_one_is_not(self):
        flat = snap.read_bmp(bmp(16, 16, lambda x, y: (18, 19, 22)))
        busy = snap.read_bmp(bmp(16, 16, lambda x, y: ((x * 16) % 256, (y * 16) % 256, 99)))
        self.assertFalse(snap.looks_drawn(*flat))
        self.assertTrue(snap.looks_drawn(*busy))

    def test_png_conversion_roundtrips_through_zlib(self):
        width, height, pixels = snap.read_bmp(bmp(4, 2, lambda x, y: (x * 60, y * 100, 5)))
        png = snap.to_png(width, height, pixels)
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        position, idat = 8, b""
        while position < len(png):
            length, kind = struct.unpack(">I4s", png[position:position + 8])
            body = png[position + 8:position + 8 + length]
            if kind == b"IHDR":
                self.assertEqual(struct.unpack(">IIBB", body[:10]), (4, 2, 8, 2))
            if kind == b"IDAT":
                idat += body
            position += 12 + length
        raw = zlib.decompress(idat)
        self.assertEqual(len(raw), 2 * (1 + 4 * 3))
        self.assertEqual(raw[1 + 3 * 3:1 + 3 * 3 + 3], bytes((180, 0, 5)))       # first row, x = 3


if __name__ == "__main__":
    unittest.main()

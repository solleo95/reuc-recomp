"""Draw marks on a capture PNG (stdlib only): `python -m uctest.overlay <capture.png> <enemies.json> [out.png]`.

enemies.json: list of {"head":[x,y],"body":[x,y],...} in aim coordinates (screen x = w/2 + 0.625*w*x ... see aim_to_pixel).
"""
import json
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, width, height, depth, color = 8, b"", 0, 0, 0, 0
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or color not in (2, 6) or interlace:
                raise ValueError("only 8-bit RGB/RGBA non-interlaced PNG")
        elif kind == b"IDAT":
            idat += body
    channels = 3 if color == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return width, height, channels, rows


def write_png(path, width, height, channels, rows):
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    color = 2 if channels == 3 else 6
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def aim_to_pixel(x, y, width, height):
    """Aim coordinates -> pixels: the 640x480 game area is centred; aim 1.0 = 0.625 of the width / 0.8333 of the height of a 1280x720 window."""
    return width / 2 + 0.625 * width * x, height / 2 + 0.8333333 * height * y


def mark(rows, channels, width, height, px, py, color, size=8, box=None):
    def put(x, y):
        if 0 <= x < width and 0 <= y < height:
            rows[y][x * channels:x * channels + 3] = bytes(color)
    px, py = int(round(px)), int(round(py))
    for d in range(-size, size + 1):
        put(px + d, py)
        put(px, py + d)
    if box:
        x0, y0, x1, y1 = (int(round(v)) for v in box)
        for x in range(x0, x1 + 1):
            put(x, y0); put(x, y1)
        for y in range(y0, y1 + 1):
            put(x0, y); put(x1, y)


def main(argv):
    capture, enemies = argv[1], json.load(open(argv[2]))
    out = argv[3] if len(argv) > 3 else capture.replace(".png", "-overlay.png")
    width, height, channels, rows = read_png(capture)
    for e in enemies:
        if "head" in e:
            mark(rows, channels, width, height, *aim_to_pixel(*e["head"], width, height), (255, 0, 0))
        if "body" in e:
            mark(rows, channels, width, height, *aim_to_pixel(*e["body"], width, height), (0, 255, 0), size=5)
    write_png(out, width, height, channels, rows)
    print(out)


if __name__ == "__main__":
    main(sys.argv)

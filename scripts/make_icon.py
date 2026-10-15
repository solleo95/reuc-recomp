"""Renders assets/icon.svg into assets/icon.ico (16..256 px, PNG frames) for the exe resource of uc.exe and uc_launcher.exe.

Needs `pip install resvg-py pillow`. The .ico is committed, so a build does not need either package.
"""
import io
import sys
from pathlib import Path

import resvg_py
from PIL import Image

ASSETS = Path(__file__).resolve().parent.parent / "assets"
SIZES = (16, 24, 32, 48, 64, 128, 256)


def render(svg: Path, size: int) -> Image.Image:
    data = bytes(resvg_py.svg_to_bytes(svg_path=str(svg), width=size, height=size))
    return Image.open(io.BytesIO(data)).convert("RGBA")


def main() -> int:
    svg = ASSETS / "icon.svg"
    frames = [render(svg, size) for size in SIZES]
    frames[-1].save(ASSETS / "icon.ico", format="ICO", sizes=[(s, s) for s in SIZES], append_images=frames[:-1])
    check = Image.open(ASSETS / "icon.ico")
    print("icon.ico sizes:", sorted(check.info.get("sizes", {(0, 0)})))
    return 0


if __name__ == "__main__":
    sys.exit(main())

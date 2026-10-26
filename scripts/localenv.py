"""Loads local.env (project root, git-ignored, template local.env.example) into os.environ; variables already set win. Imported for its effect."""
import os
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NOT_SET = "UC_INSTALL_DIR is not set: copy local.env.example to local.env and fill it in (docs/building.md)"


def _load():
    path = ROOT / "local.env"
    if not path.is_file():
        return
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        key, sep, value = line.strip().partition("=")
        if sep and not key.startswith("#"):
            os.environ.setdefault(key.strip(), value.strip())


_load()


def install_dir():
    """The game's install folder (UC_INSTALL_DIR) or None."""
    value = os.environ.get("UC_INSTALL_DIR")
    return Path(value) if value else None


def game_dir():
    """The extracted disc (UC_GAME_DIR, else <install>\\game) or None."""
    value = os.environ.get("UC_GAME_DIR")
    install = install_dir()
    return Path(value) if value else install / "game" if install else None


def saves_dir():
    """The player's saves (<install>\\saves) or None."""
    install = install_dir()
    return install / "saves" if install else None


def llvm_tool(name):
    """Path of an LLVM tool (llvm-profdata, llvm-symbolizer): on PATH, else in UC_LLVM_DIR."""
    found = shutil.which(name)
    if found:
        return found
    folder = os.environ.get("UC_LLVM_DIR")
    if not folder:
        raise SystemExit(f"{name} not found: put LLVM on PATH or set UC_LLVM_DIR in local.env")
    return str(Path(folder) / (name + ".exe"))

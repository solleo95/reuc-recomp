"""The translated bodies of the functions that tools/leaf_hle.py replaces, independent of generated/.

Functions mapped to a Leaf_ HLE entry are absent from the normal generation, so this translates the game once more with those entries left out
into build/leaf_ref/ (git-ignored, rebuilt when the DOL, the translator or the configs change) and returns the generation folder to read.
"""
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def reference_generation():
    out = ROOT / "build" / "leaf_ref"
    install = Path(os.environ.get("UC_INSTALL_DIR", r"S:\Games\Umbrella Chronicles"))
    dol = install / "game" / "DATA" / "sys" / "main.dol"
    translator = ROOT / "build" / "tools" / "translator" / "translator.exe"
    cfg = ROOT / "project" / "uc.cfg"
    hle = ROOT / "project" / "hle.cfg"
    for needed in (dol, translator, hle):
        if not needed.exists():
            sys.exit("leaf_ref: %s is missing (run build.bat first; UC_INSTALL_DIR points at the game files)" % needed)
    out.mkdir(parents=True, exist_ok=True)
    reference_hle = out / "hle_ref.cfg"
    reference_hle.write_text("".join(line for line in hle.read_text(encoding="utf-8").splitlines(keepends=True) if " Leaf_" not in line), encoding="utf-8")
    key = "".join(hashlib.sha256(p.read_bytes()).hexdigest() for p in (dol, translator, cfg, reference_hle))   # the translator wants four SHA-256 digests
    manifest = out / "game_sources.cmake"
    if not manifest.exists() or "# translation-input: " + key not in manifest.read_text(encoding="utf-8"):
        print("leaf_ref: translating the reference generation into %s" % out, file=sys.stderr)
        subprocess.run([str(translator), "--dol", str(dol), "--cfg", str(cfg), "--cfg", str(reference_hle), "--out", str(out), "--shard", "150",
                        "--cache-key", key], check=True, stdout=subprocess.DEVNULL)
    return out / re.search(r"generation_[0-9a-f_]+", manifest.read_text(encoding="utf-8"))[0]

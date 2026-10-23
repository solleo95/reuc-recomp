"""Spike M3: write scripts/uctest/mods/arc_grow with one archive grown by N bytes in one member.

    python tools/spikes/u8_grow_mod.py <game files root>/Data <relative arc> <member> <extra bytes>
"""
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(__file__))
import u8_repack as u8

data_root, rel, member, extra = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 0)
mod = os.path.join(os.path.dirname(__file__), "..", "..", "scripts", "uctest", "mods", "arc_grow")
shutil.rmtree(mod, ignore_errors=True)
os.makedirs(mod)
json.dump({"id": "arc-grow", "name": "Arc grow", "version": "1.0", "api": "1.1"}, open(os.path.join(mod, "mod.json"), "w"))
blob = open(os.path.join(data_root, rel), "rb").read()
archive = u8.parse(blob)
index = u8.paths(archive).index(member)
out = u8.build(archive, {member: archive["nodes"][index].data + b"\0" * extra})
dest = os.path.join(mod, "files", "Data", rel)
os.makedirs(os.path.dirname(dest), exist_ok=True)
open(dest, "wb").write(out)
print(rel, len(blob), "->", len(out))

"""python -I tools/prune_generations.py <generated dir>

The translator never deletes the generation folders of earlier translations (one per change of hle.cfg, uc.cfg or the translator: ~50 MB each).
Only the one named in game_sources.cmake takes part in the build, so everything else, and the shards of the old flat layout, is removed.
"""
import re
import shutil
import sys
from pathlib import Path

gen = Path(sys.argv[1])
manifest = gen / "game_sources.cmake"
if not manifest.exists():
    sys.exit(0)   # nothing translated yet: nothing to keep, nothing to delete
keep = re.search(r"generation_[0-9a-f_]+", manifest.read_text(encoding="utf-8"))[0]
for folder in gen.glob("generation_*"):
    if folder.is_dir() and folder.name != keep:
        shutil.rmtree(folder, ignore_errors=True)
for legacy in [*gen.glob("game_*.cpp"), gen / "game_decl.h", gen / ".key"]:
    legacy.unlink(missing_ok=True)

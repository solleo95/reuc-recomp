"""Hash translation inputs before emitting the cache key; failures emit no key."""
import hashlib
import os
from pathlib import Path
import sys

try:
    digests = [hashlib.sha256(Path(os.environ[name]).read_bytes()).hexdigest()
               for name in ("GAME_DOL", "TRANSLATOR", "CFG", "HLECFG")]
except (OSError, KeyError) as error:
    sys.stderr.write("translation cache: %s\n" % error)
    sys.exit(1)
print("".join(digests))

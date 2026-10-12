import os
from pathlib import Path
import subprocess
import sys
import uuid

root, output = Path(sys.argv[1]), Path(sys.argv[2]) / str(uuid.uuid4())
output.mkdir(parents=True)
env = dict(os.environ)
for index, name in enumerate(("GAME_DOL", "TRANSLATOR", "CFG", "HLECFG")):
    path = output / name
    path.write_bytes(bytes([index, 10, 20]))
    env[name] = str(path)

def key():
    return subprocess.run([sys.executable, "-I", str(root / "tools/translate_key.py")], env=env, text=True, capture_output=True)

first = key()
assert first.returncode == 0 and len(first.stdout.strip()) == 256, first.stderr
assert key().stdout == first.stdout
Path(env["CFG"]).write_bytes(b"changed config")
assert key().stdout != first.stdout
env["GAME_DOL"] = str(output / "missing.dol")
missing = key()
assert missing.returncode != 0 and not missing.stdout.strip(), missing

# Verified GX symbols are accepted; ambiguous signature names fail without overwriting config.
project = output / "project"
project.mkdir()
(project / "identified_sdk.txt").write_text("80284FD0 GXBegin\n", encoding="utf-8")
(project / "gx_symbols.txt").write_text("80284C30 GXSetDrawDoneCallback\n", encoding="utf-8")
(project / "hle_map.txt").write_text("GXSetDrawDoneCallback\nGXBegin\n@802841DC GXSetArray\n", encoding="utf-8")
def config():
    return subprocess.run([sys.executable, "-I", str(root / "tools/hle_cfg.py"), str(project)], text=True, capture_output=True)
assert config().returncode == 0
generated = (project / "hle.cfg").read_text(encoding="utf-8")
assert "hle 80284C30 GXSetDrawDoneCallback" in generated and "hle 802841DC GXSetArray" in generated
(project / "identified_sdk.txt").write_text("80284FD0 GXBegin\n80000004 GXBegin\n", encoding="utf-8")
ambiguous = config()
assert ambiguous.returncode != 0 and "ambiguous" in ambiguous.stderr
assert (project / "hle.cfg").read_text(encoding="utf-8") == generated

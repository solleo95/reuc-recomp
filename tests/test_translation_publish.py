"""A failed publication must leave every previously active source intact."""
import ctypes
from ctypes import wintypes
from pathlib import Path
import re
import subprocess
import sys
import uuid

translator, dol, base = sys.argv[1:]
output = Path(base) / str(uuid.uuid4())
output.mkdir(parents=True)
def run(*extra):
    return subprocess.run([translator, "--dol", dol, "--out", str(output), "--shard", "100000", *extra], capture_output=True, text=True)
def sources():
    text = (output / "game_sources.cmake").read_text(encoding="utf-8")
    return {p: p.read_bytes() for name in re.findall(r'\$\{CMAKE_CURRENT_LIST_DIR\}/([^"\n]+\.cpp)', text) for p in [output / name]}

first = run()
assert first.returncode == 0, first.stderr
manifest = output / "game_sources.cmake"
old_manifest, old_sources = manifest.read_bytes(), sources()
config = output / "changed.cfg"
config.write_text("hle 80003100 DifferentNativeCall\n", encoding="utf-8")
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel.CreateFileW.restype = wintypes.HANDLE
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
kernel.CloseHandle.restype = wintypes.BOOL
handle = kernel.CreateFileW(str(manifest), 0x80000000, 1, None, 3, 0, None)
assert handle != ctypes.c_void_p(-1).value, ctypes.get_last_error()
try:
    failed = run("--cfg", str(config))
    assert failed.returncode != 0
    assert manifest.read_bytes() == old_manifest
    assert all(path.read_bytes() == data for path, data in old_sources.items()), "failed generation modified previously published sources"
finally:
    assert kernel.CloseHandle(handle)
second = run("--cfg", str(config))
assert second.returncode == 0, second.stderr
assert sources() != old_sources
restored = run()
assert restored.returncode == 0, restored.stderr
assert manifest.read_bytes() == old_manifest and sources() == old_sources

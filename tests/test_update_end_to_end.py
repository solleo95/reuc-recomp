"""Native updater acceptance with fixture launchers; never executes the game."""
import ctypes
from ctypes import wintypes
import hashlib
import importlib.util
import json
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

REPO, UPDATER, CHILD, SOURCES, LAUNCHER = map(lambda value: Path(value).resolve(), sys.argv[1:6])
del sys.argv[1:6]
spec = importlib.util.spec_from_file_location("build_release", REPO / "scripts/build_release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)
ID = "11111111-1111-4111-8111-111111111111"
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel.OpenProcess.restype = wintypes.HANDLE
kernel.CloseHandle.argtypes = [wintypes.HANDLE]
kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
user = ctypes.WinDLL("user32", use_last_error=True)
enum_callback = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user.EnumWindows.argtypes = [enum_callback, wintypes.LPARAM]
user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
user.GetWindowDpiAwarenessContext.argtypes = [wintypes.HWND]
user.GetWindowDpiAwarenessContext.restype = wintypes.HANDLE
user.AreDpiAwarenessContextsEqual.argtypes = [wintypes.HANDLE, wintypes.HANDLE]


def updater_window(process_id):
    found = []
    @enum_callback
    def visit(window, _):
        owner = wintypes.DWORD()
        user.GetWindowThreadProcessId(window, ctypes.byref(owner))
        name = ctypes.create_unicode_buffer(256)
        user.GetClassNameW(window, name, len(name))
        if owner.value == process_id and name.value == "UCUpdater":
            found.append(window)
        return True
    user.EnumWindows(visit, 0)
    return found[0] if found else None


def wait_until(predicate, seconds=30):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except PermissionError:
            time.sleep(.01)  # Recovery may briefly hold the journal exclusively while repairing its tail.
            continue
        time.sleep(.01)
    raise AssertionError("Timed out waiting for update fixture")


class OwnedHelper:
    def __init__(self, root):
        self.handle = kernel.OpenProcess(0x100000 | 0x1000 | 1, False, int((root / "helper-pid").read_text()))
        if self.handle:
            path = ctypes.create_unicode_buffer(32768)
            length = wintypes.DWORD(len(path))
            if not kernel.QueryFullProcessImageNameW(self.handle, 0, path, ctypes.byref(length)):
                raise ctypes.WinError(ctypes.get_last_error())
            assert Path(path.value) == root / ".uc-update" / ID / "recovery/uc_updater.exe"

    def stop(self):
        if self.handle and kernel.WaitForSingleObject(self.handle, 0) == 258:
            assert kernel.TerminateProcess(self.handle, 99)
            assert kernel.WaitForSingleObject(self.handle, 10000) == 0

    def close(self):
        self.stop()
        if self.handle:
            kernel.CloseHandle(self.handle)
            self.handle = None


class UpdateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="uc-native-update- ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.operation = self.root / ".uc-update" / ID
        self.stage = self.operation / "stage"
        sources = [Path(line) for line in SOURCES.read_text().splitlines() if line]
        self.helpers = ["uc_updater.exe", *[path.name for path in sources[1:]]]
        for folder in (self.root, self.stage):
            folder.mkdir(parents=True, exist_ok=True)
            shutil.copy2(CHILD, folder / "uc_launcher.exe")
            (folder / "uc.exe").write_bytes(b"NOT A GAME EXECUTABLE")
            for source, name in zip(sources, self.helpers):
                shutil.copy2(source, folder / name)
            (folder / "data/shaders").mkdir(parents=True)
            (folder / "data/shaders/test.spv").write_bytes(b"shader")
            (folder / "data/uc.map").write_bytes(b"map")
            (folder / "assets/launcher").mkdir(parents=True)
            (folder / "assets/launcher/graphite-atlas.png").write_bytes(b"fixture")
            (folder / "payload.bin").write_bytes((b"A" if folder == self.root else b"B") * (32 * 1024 * 1024))
            if folder == self.root:
                (folder / "obsolete.txt").write_bytes(b"OLD")
                release.write_manifest(folder, "0.0.1", self.helpers)
            else:
                (folder / "added.txt").write_bytes(b"NEW")
                for name in ("uc_updater.exe", "msvcp140.dll"):
                    with (folder / name).open("ab") as stream:
                        stream.write(b"UC fixture version 0.0.2")
                release.write_manifest(folder, "0.0.2", self.helpers)
        self.protected = {"config.json": b"{\"user\":1}", "saves/user.dat": b"SAVE", "mods/user.lua": b"MOD",
                          "unknown.txt": b"UNKNOWN", "data/pipeline_cache.bin": b"CACHE"}
        for name, data in self.protected.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.old_manifest = (self.root / "release.json").read_bytes()

    def terminal(self, phase):
        log = self.operation / "journal.log"
        return log.exists() and log.read_bytes().endswith(b'{"event":"' + phase.encode() + b'"}\n')

    def start(self):
        subprocess.run([str(CHILD), "--native-driver"], cwd=self.root, check=True, timeout=40)
        helper = OwnedHelper(self.root)
        self.addCleanup(helper.close)
        return helper

    def preserve(self):
        for name, data in self.protected.items():
            self.assertEqual((self.root / name).read_bytes(), data)

    def wait_fixture_exit(self, name):
        marker = self.root / name
        wait_until(lambda: marker.exists() and bool(marker.read_text().strip()))
        self.wait_launcher_exit(int(marker.read_text()))

    def wait_launcher_exit(self, process_id):
        handle = kernel.OpenProcess(0x100000 | 0x1000, False, process_id)
        if not handle:
            self.assertEqual(ctypes.get_last_error(), 87)  # Process already exited.
            return
        try:
            if kernel.WaitForSingleObject(handle, 0) == 0:
                return
            path = ctypes.create_unicode_buffer(32768)
            length = wintypes.DWORD(len(path))
            if not kernel.QueryFullProcessImageNameW(handle, 0, path, ctypes.byref(length)):
                self.assertEqual(kernel.WaitForSingleObject(handle, 10000), 0)  # Image may unload before process exit signals.
                return
            self.assertEqual(Path(path.value), self.root / "uc_launcher.exe")
            self.assertEqual(kernel.WaitForSingleObject(handle, 10000), 0)
        finally:
            kernel.CloseHandle(handle)

    def recover(self):
        # This is the same recovery entry point exposed by the saved native helper.
        (self.root / "old-restarted").unlink(missing_ok=True)
        process = subprocess.Popen([str(self.operation / "recovery/uc_updater.exe"), "--recover", str(self.operation)])
        try:
            self.assertEqual(process.wait(timeout=40), 0)
            self.wait_fixture_exit("old-restarted")
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)

    def test_success_and_committed_recovery(self):
        archive = release.package_release(self.stage, "0.0.2", self.root / "fixture.zip")
        subprocess.run([str(REPO / "build/tests/test_update_package.exe"), "--verify-release", str(archive), "0.0.2",
                        release.sha256(archive), str(archive.stat().st_size)], check=True)
        self.start()
        wait_until(lambda: self.terminal("committed") and (self.root / "child-committed").exists())
        self.wait_fixture_exit("child-committed")
        self.assertEqual(json.loads((self.root / "release.json").read_text())["version"], "0.0.2")
        self.assertEqual((self.root / "payload.bin").read_bytes()[:1], b"B")
        self.assertFalse((self.root / "obsolete.txt").exists())
        self.assertEqual((self.root / "added.txt").read_bytes(), b"NEW")
        self.assertEqual(release.sha256(self.root / "uc_updater.exe"), release.sha256(self.stage / "uc_updater.exe"))
        self.assertEqual(release.sha256(self.root / "msvcp140.dll"), release.sha256(self.stage / "msvcp140.dll"))
        self.preserve()
        time.sleep(.1)
        self.recover()
        self.assertEqual(json.loads((self.root / "release.json").read_text())["version"], "0.0.2")

    def test_real_launcher_bootstrap(self):
        for folder in (self.root, self.stage):
            shutil.copy2(LAUNCHER, folder / "uc_launcher.exe")
            shutil.copy2(LAUNCHER.parent / "SDL3.dll", folder / "SDL3.dll")
            shutil.copy2(REPO / "assets/launcher/graphite-atlas.png", folder / "assets/launcher/graphite-atlas.png")
            manifest = json.loads((folder / "release.json").read_text())
            manifest["files"] = [entry for entry in manifest["files"] if entry["path"] not in {
                "uc_launcher.exe", "SDL3.dll", "assets/launcher/graphite-atlas.png"}]
            for name in ("uc_launcher.exe", "SDL3.dll", "assets/launcher/graphite-atlas.png"):
                manifest["files"].append({"path": name, "size": (folder / name).stat().st_size, "sha256": release.sha256(folder / name)})
            (folder / "release.json").write_text(json.dumps(manifest), encoding="utf-8")
        self.start()
        wait_until(lambda: self.terminal("committed"))
        launcher_pid = None
        def launcher_exited():
            nonlocal launcher_pid
            logs = list((self.root / "logs").rglob("runtime.log"))
            for path in logs:
                if not path.is_file():
                    continue
                text = path.read_text(encoding="utf-8", errors="replace")
                if "[session] launcher exit=0" in text:
                    match = re.search(r"\[session\] app=launcher pid=(\d+)\b", text)
                    self.assertIsNotNone(match, "Launcher session log must identify its process")
                    launcher_pid = int(match.group(1))
                    return True
            return False
        wait_until(launcher_exited)
        self.wait_launcher_exit(launcher_pid)
        self.preserve()
        self.assertEqual(json.loads((self.root / "release.json").read_text())["version"], "0.0.2")

    def test_real_launcher_startup_error_exits_without_dialog(self):
        # Invalid artwork passes package hashes but fails the real launcher's trial startup.
        for name, source in (("uc_launcher.exe", LAUNCHER), ("SDL3.dll", LAUNCHER.parent / "SDL3.dll")):
            shutil.copy2(source, self.stage / name)
        manifest = json.loads((self.stage / "release.json").read_text())
        manifest["files"] = [entry for entry in manifest["files"] if entry["path"] not in {"uc_launcher.exe", "SDL3.dll"}]
        for name in ("uc_launcher.exe", "SDL3.dll"):
            manifest["files"].append({"path": name, "size": (self.stage / name).stat().st_size,
                                      "sha256": release.sha256(self.stage / name)})
        (self.stage / "release.json").write_text(json.dumps(manifest), encoding="utf-8")
        started = time.monotonic()
        helper = self.start()
        wait_until(lambda: self.terminal("rolled_back") and (self.root / "old-restarted").exists(), seconds=45)
        self.wait_fixture_exit("old-restarted")
        helper.stop()
        self.assertLess(time.monotonic() - started, 10, "Trial error left a blocking dialog until the helper timeout")
        self.assertEqual((self.root / "release.json").read_bytes(), self.old_manifest)
        self.preserve()

    def test_failed_start_rolls_back(self):
        (self.root / "exit-before-ack").write_bytes(b"fixture")
        helper = self.start()
        wait_until(lambda: self.terminal("rolled_back") and (self.root / "old-restarted").exists())
        self.wait_fixture_exit("old-restarted")
        window = updater_window(int((self.root / "helper-pid").read_text()))
        self.assertIsNotNone(window)
        self.assertTrue(user.AreDpiAwarenessContextsEqual(user.GetWindowDpiAwarenessContext(window), -4),
                        "Updater must render at per-monitor DPI instead of Windows bitmap stretching")
        helper.stop()  # Close the owned helper's diagnostic window after inspecting durable rollback.
        self.assertEqual((self.root / "release.json").read_bytes(), self.old_manifest)
        self.assertEqual((self.root / "payload.bin").read_bytes()[:1], b"A")
        self.assertFalse((self.root / "added.txt").exists())
        self.assertEqual((self.root / "obsolete.txt").read_bytes(), b"OLD")
        self.preserve()

    def test_killed_helper_then_repeated_recovery(self):
        helper = self.start()
        wait_until(lambda: (self.operation / "journal.log").exists() and b'"prepared"' in (self.operation / "journal.log").read_bytes())
        helper.stop()
        self.assertFalse(self.terminal("committed"))
        self.recover()
        self.recover()
        self.assertEqual((self.root / "release.json").read_bytes(), self.old_manifest)
        self.assertEqual((self.root / "payload.bin").read_bytes()[:1], b"A")
        self.preserve()


if __name__ == "__main__":
    unittest.main()

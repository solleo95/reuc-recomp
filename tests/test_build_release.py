import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

REPO = Path(sys.argv.pop(1)).resolve()
spec = importlib.util.spec_from_file_location("build_release", REPO / "scripts/build_release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="uc-release-кириллица ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "build/tests").mkdir(parents=True)
        (self.root / "build/tests/test_update_package.exe").write_bytes(b"fixture")
        (self.root / "build/updater-runtime-files.txt").write_text("uc_updater.exe\nmsvcp140.dll\n", encoding="utf-8")
        (self.root / "dist").mkdir()
        (self.root / "dist/uc_experiment.exe").write_bytes(b"LOCAL")
        (self.root / "dist/saves").mkdir()
        (self.root / "dist/saves/user.dat").write_bytes(b"USER")
        (self.root / "dist-release").mkdir()
        (self.root / "dist-release/previous").write_bytes(b"PREVIOUS")
        self.calls = []
        self.envs = []
        self.failure = None
        self.extra = None
        self.native_verify = False

    def runner(self, args, **kwargs):
        self.calls.append(args)
        self.envs.append(kwargs.get("env"))
        phase = "install" if "--install" in args else "verify" if "--verify-release" in args else "build"
        if self.failure == phase:
            raise subprocess.CalledProcessError(1, args)
        if phase == "verify" and self.native_verify:
            subprocess.run([str(REPO / "build/tests/test_update_package.exe"), *args[1:]], check=True)
        if phase == "install":
            stage = Path(args[args.index("--prefix") + 1])
            stage.mkdir(parents=True, exist_ok=True)
            files = ["uc.exe", "uc_avx2.exe", "uc_compat.exe", "uc_launcher.exe", "uc_updater.exe", "msvcp140.dll", "data/uc.map", "data/uc_compat.map",
                     "data/shaders/test.spv", "assets/launcher/graphite-train.png", "assets/launcher/graphite-brand.png"]
            for name in files:
                path = stage / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"runtime")
            (self.root / "build/install_manifest.txt").write_text("\n".join(str(stage / p) for p in files), encoding="utf-8")
            if self.extra:
                self.extra(stage)

    def build(self):
        return release.build_release(self.root, "0.0.1", runner=self.runner)

    def test_release_build_must_have_thinlto(self):
        cache = Path(self.temp.name) / "CMakeCache.txt"
        release.require_thinlto(cache)   # no cache (the tests' fake build): not checked
        cache.write_text("CMAKE_BUILD_TYPE:STRING=Release\nUC_THINLTO:BOOL=ON\n", encoding="utf-8")
        release.require_thinlto(cache)
        for text in ("UC_THINLTO:BOOL=OFF\n", "CMAKE_BUILD_TYPE:STRING=Release\n", "XUC_THINLTO:BOOL=ON\n"):
            cache.write_text(text, encoding="utf-8")
            with self.assertRaises(ValueError):
                release.require_thinlto(cache)

    def test_versions(self):
        self.assertEqual(release.parse_version("0.0.1"), "0.0.1")
        for value in ["v0.0.1", "00.0.1", "0.0", "1.2.65536", "0.0.1 & whoami", "0.0.1\n", "١.0.1"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                release.parse_version(value)

    def test_success_and_clean_stage(self):
        archive = self.build()
        self.assertEqual(archive.name, "UC-Windows-0.0.1.zip")
        self.assertEqual(self.calls[0][-1], "--no-install")
        self.assertEqual(self.envs[0]["UC_ALL_TESTS"], "1")  # a release runs every test
        with zipfile.ZipFile(archive) as zip_file:
            names = zip_file.namelist()
            self.assertIn("uc_launcher.exe", names)
            self.assertNotIn("saves/user.dat", names)
            self.assertNotIn("uc_experiment.exe", names)
            manifest = json.loads(zip_file.read("release.json"))
            self.assertEqual(manifest["version"], "0.0.1")
            self.assertEqual(manifest["helper_files"], ["uc_updater.exe", "msvcp140.dll"])
        self.assertEqual((self.root / "dist/saves/user.dat").read_bytes(), b"USER")
        self.assertFalse((self.root / "dist-release/previous").exists())
        self.assertTrue((self.root / "dist-release/release.json").exists())
        self.assertFalse(list(self.root.glob(".release-build-*")))

    def test_existing_zip_refused_before_build(self):
        archive = self.root / "UC-Windows-0.0.1.zip"
        archive.write_bytes(b"EXISTING")
        with self.assertRaises(FileExistsError):
            self.build()
        self.assertEqual(archive.read_bytes(), b"EXISTING")
        self.assertFalse(self.calls)

    def test_cpp_validator(self):
        self.native_verify = True
        self.assertTrue(self.build().is_file())

    def test_parallel_build_refused(self):
        with release.release_lock(self.root), self.assertRaises(OSError):
            self.build()
        self.assertFalse(self.calls)

    def test_failures_preserve_previous(self):
        for phase in ["build", "install", "verify"]:
            with self.subTest(phase=phase):
                self.failure = phase
                with self.assertRaises(subprocess.CalledProcessError):
                    self.build()
                self.assertEqual((self.root / "dist-release/previous").read_bytes(), b"PREVIOUS")
                self.assertFalse((self.root / "UC-Windows-0.0.1.zip").exists())

    def test_unknown_staged_file(self):
        self.extra = lambda stage: (stage / "uc_experiment.exe").write_bytes(b"UNKNOWN")
        with self.assertRaises(ValueError):
            self.build()

    def test_missing_helper_runtime(self):
        self.extra = lambda stage: (stage / "msvcp140.dll").unlink()
        with self.assertRaises(ValueError):
            self.build()

    def test_missing_install_manifest(self):
        self.extra = lambda stage: (self.root / "build/install_manifest.txt").unlink()
        with self.assertRaises((ValueError, FileNotFoundError)):
            self.build()

    def test_publication_rolls_back(self):
        original = Path.rename
        def rename(source, destination):
            if Path(destination).name == "UC-Windows-0.0.1.zip":
                raise OSError("fixture publication failure")
            return original(source, destination)
        with patch.object(Path, "rename", rename), self.assertRaises(OSError):
            self.build()
        self.assertEqual((self.root / "dist-release/previous").read_bytes(), b"PREVIOUS")
        self.assertFalse((self.root / "UC-Windows-0.0.1.zip").exists())

    def test_link_refused(self):
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "user").write_bytes(b"USER")
        def link(stage):
            try:
                os.symlink(outside, stage / "link", target_is_directory=True)
            except OSError as error:
                if os.name != "nt":
                    self.skipTest(f"symlink unavailable: {error}")
                import _winapi
                _winapi.CreateJunction(str(outside), str(stage / "link"))
        self.extra = link
        with self.assertRaises(ValueError):
            self.build()
        self.assertEqual((outside / "user").read_bytes(), b"USER")


    def printed(self, **options):
        import contextlib, io
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            release.build_release(self.root, "0.0.1", runner=self.runner, **options)
        return out.getvalue()

    def test_checklist_is_printed_unless_the_linux_archive_follows(self):
        self.assertIn("Publishing checklist", self.printed())
        (self.root / "UC-Windows-0.0.1.zip").unlink()
        self.assertNotIn("Publishing checklist", self.printed(show_checklist=False))


class IsolatedLaunchTests(unittest.TestCase):
    def test_script_loads_under_python_isolated_mode(self):
        """build-release.bat runs `python -I`, which leaves the script's folder off sys.path: the Linux archive step must still import."""
        code = "import runpy, sys; runpy.run_path(sys.argv[1], run_name='probe'); import build_release_deck; print(callable(build_release_deck.build_deck_release))"
        result = subprocess.run([sys.executable, "-I", "-c", code, str(REPO / "scripts/build_release.py")], capture_output=True, text=True)
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "True"), result.stderr)


if __name__ == "__main__":
    unittest.main()

import importlib.util
import json
from pathlib import Path
import re
import stat
import sys
import tempfile
import unittest
import zipfile

REPO = Path(sys.argv.pop(1)).resolve()
spec = importlib.util.spec_from_file_location("build_release_deck", REPO / "scripts/build_release_deck.py")
deck = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deck)

FILES = ["uc", "uc_launcher", "uc-launcher.sh", "uc.sh", "tools/wit/wit", "data/shaders/a.spv", "data/uc.map", "assets/launcher/graphite-train.png",
         "assets/launcher/graphite-brand.png", "licenses/wit/gpl-2.0.txt"]


spec_deck = importlib.util.spec_from_file_location("deck_stage", REPO / "scripts/linux/deck.py")
deck_stage = importlib.util.module_from_spec(spec_deck)
sys.modules["deck_stage"] = deck_stage
spec_deck.loader.exec_module(deck_stage)


class PackageTreeTests(unittest.TestCase):
    def test_package_tree_has_everything_the_release_needs(self):
        with tempfile.TemporaryDirectory() as tmp:
            build, out = Path(tmp) / "build", Path(tmp) / "out"
            for name, data in {"game/uc": b"uc", "launcher/uc_launcher": b"launcher", "game/data/shaders/a.spv": b"spv", "game/data/pipeline_keys.seed": b"seed",
                               "_deps/wit_linux-src/bin/wit": b"wit", "_deps/wit_linux-src/gpl-2.0.txt": b"gpl", "_deps/wit_linux-src/doc/wit-source.txt": b"src",
                               "_deps/nlohmann_json-src/LICENSE.MIT": b"mit", "_deps/mimalloc-src/LICENSE": b"mi", "_deps/imgui-src/LICENSE.txt": b"imgui",
                               "_deps/miniz-src/LICENSE": b"miniz", "_deps/lua-src/doc/readme.html": b"lua", "_deps/quickjs-src/LICENSE": b"qjs"}.items():
                path = build / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            deck_stage.package_tree(build, out)
            names = {path.relative_to(out).as_posix() for path in out.rglob("*") if path.is_file()}
            self.assertTrue(deck.REQUIRED <= names, deck.REQUIRED - names)
            for needed in ("data/shaders/a.spv", "data/pipeline_keys.seed", "licenses/wit/gpl-2.0.txt", "licenses/wit/wit-source.txt", "licenses/wit/NOTICE.txt", "licenses/stb/NOTICE.txt"):
                self.assertIn(needed, names)
            self.assertNotIn("libSDL3.so", " ".join(names))     # the Deck's own SDL3 is used
            self.assertIn("THIRD_PARTY_NOTICES.md", names)
            # every licence the Windows package installs is shipped too, except those of components that are not in the Deck build (Windows upscalers, netplay NAT
            # libraries, the SDL3 library that the Deck supplies itself): a licence added to a CMake install() must not be forgotten here
            windows_only = {"Streamline", "FidelityFX", "enet", "miniupnpc", "SDL3"}
            installed = set()
            for cmake in list((REPO / "cmake").glob("*.cmake")) + list(REPO.glob("*/CMakeLists.txt")) + [REPO / "CMakeLists.txt"]:
                installed |= set(re.findall(r"DESTINATION\s+licenses/([A-Za-z0-9_.+-]+)", cmake.read_text(encoding="utf-8", errors="replace")))
            shipped = {name.split("/")[1] for name in names if name.startswith("licenses/")}
            self.assertEqual(installed - windows_only - shipped, set(), "licences the Windows package ships and the Deck package lacks")
            for script in ("uc-launcher.sh", "uc.sh"):          # shell needs LF, whatever git checked out on Windows
                text = (out / script).read_bytes()
                self.assertTrue(text.startswith(b"#!/bin/sh\n") and b"\r" not in text, script)
            # and the release builder accepts exactly this tree
            repo = Path(tmp) / "repo"
            repo.mkdir()
            self.assertEqual(deck.build_deck_release(repo, "0.0.3", out).name, "UC-Linux-0.0.3.zip")

    def test_package_tree_refuses_a_missing_binary(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(FileNotFoundError):
                deck_stage.package_tree(Path(tmp) / "build", Path(tmp) / "out")


class DeckReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="uc-deck-кириллица ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.stage = self.root / "stage"
        for name in FILES:
            path = self.stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"x" + name.encode())

    def test_zip_has_manifest_and_modes(self):
        final = deck.build_deck_release(self.repo, "0.0.2", self.stage)
        self.assertEqual(final.name, "UC-Linux-0.0.2.zip")
        with zipfile.ZipFile(final) as archive:
            names = set(archive.namelist())
            self.assertEqual(names, set(FILES) | {"release.json"})
            manifest = json.loads(archive.read("release.json"))
            self.assertEqual((manifest["format"], manifest["version"], manifest["helper_files"]), (1, "0.0.2", []))
            self.assertEqual({entry["path"] for entry in manifest["files"]}, set(FILES))
            for info in archive.infolist():   # "made by Unix": extractors ignore the mode bits of an entry written by an MS-DOS host (a build on Windows)
                self.assertEqual(info.create_system, 3, info.filename)
            for name in ("uc", "uc_launcher", "uc-launcher.sh", "uc.sh", "tools/wit/wit"):
                self.assertEqual(stat.S_IMODE(archive.getinfo(name).external_attr >> 16), 0o755, name)
            self.assertEqual(stat.S_IMODE(archive.getinfo("data/uc.map").external_attr >> 16), 0o644)

    def test_rejects_missing_required_file(self):
        (self.stage / "uc_launcher").unlink()
        with self.assertRaises(ValueError):
            deck.build_deck_release(self.repo, "0.0.2", self.stage)

    def test_rejects_user_data(self):
        for name in ("config.json", "saves/a.dat", "game/DATA/sys/main.dol", "steam-env.txt", "next-run.sh", "launcher-state.json", "logs/x.txt"):
            path = self.stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"user")
            with self.subTest(name), self.assertRaises(ValueError):
                deck.build_deck_release(self.repo, "0.0.2", self.stage)
            path.unlink()
            (self.stage / "release.json").unlink(missing_ok=True)

    def test_failure_leaves_no_final_zip(self):
        (self.stage / "tools/wit/wit").unlink()
        with self.assertRaises(ValueError):
            deck.build_deck_release(self.repo, "0.0.2", self.stage)
        self.assertEqual(list(self.repo.iterdir()), [])

    def test_existing_archive_is_not_overwritten(self):
        (self.repo / "UC-Linux-0.0.2.zip").write_bytes(b"old")
        with self.assertRaises(FileExistsError):
            deck.build_deck_release(self.repo, "0.0.2", self.stage)
        self.assertEqual((self.repo / "UC-Linux-0.0.2.zip").read_bytes(), b"old")

    def test_names_every_client_would_refuse_are_refused_here(self):
        # the rules of path_key in updater/release.cpp: a release with such a name would fail as "Invalid manifest" on every Deck
        deck.check_names({"uc", "uc_launcher", "data/Shaders/a.spv", "tools/wit/wit", "ф.bin"})   # fine, also non-ASCII
        for bad in ({"uc", "UC"}, {"data/A.bin", "DATA/a.BIN"}, {"a:b"}, {"a~1"}, {"a\\b"}, {"a<b"}, {"a|b"}, {"a?b"}, {"a*b"}, {'a"b'}, {"a\tb"}, {"a\x7fb"},
                    {"dir/trailing."}, {"dir/trailing "}, {"CON"}, {"data/nul.txt"}, {"x/COM1.dll"}, {"a//b"}, {"/abs"}, {"../x"}, {"a/./b"}, {"bad\udcff"}):
            with self.subTest(bad), self.assertRaises(ValueError):
                deck.check_names(bad)

    def test_rejects_a_tree_without_shaders(self):
        (self.stage / "data/shaders/a.spv").unlink()
        with self.assertRaises(ValueError):
            deck.build_deck_release(self.repo, "0.0.2", self.stage)

    def test_stage_is_left_as_it_was_so_a_rerun_works(self):
        for _ in range(2):
            deck.build_deck_release(self.repo, "0.0.2" if _ == 0 else "0.0.3", self.stage)
            self.assertFalse((self.stage / "release.json").exists())
        (self.stage / "uc_launcher").unlink()
        with self.assertRaises(ValueError):
            deck.build_deck_release(self.repo, "0.0.4", self.stage)
        self.assertFalse((self.stage / "release.json").exists())

    def test_rejects_bad_version(self):
        with self.assertRaises(ValueError):
            deck.build_deck_release(self.repo, "0.0", self.stage)

    def test_checklist_mentions_latest_and_both_archives(self):
        text = deck.checklist("0.0.2")
        for needle in ("BOTH archives", "UC-Windows-<version>.zip", "UC-Linux-<version>.zip", "latest release", "0.0.2"):
            self.assertIn(needle, text)


if __name__ == "__main__":
    unittest.main()

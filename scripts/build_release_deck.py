"""Build the Steam Deck release archive (UC-SteamDeck-X.Y.Z.zip) from an installed Deck tree.

  cmake --install build-linux-znver2 --prefix <stage>
  python scripts/build_release_deck.py --version 0.0.2 --stage <stage>
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_release import checklist, files_in, parse_version, sha256  # noqa: E402,F401  (checklist is re-exported)

REQUIRED = {"uc", "uc_launcher", "uc-deck.sh", "uc-game.sh", "assets/launcher/graphite-train.png", "assets/launcher/graphite-brand.png", "tools/wit/wit"}
EXECUTABLES = {"uc", "uc_launcher", "uc-deck.sh", "uc-game.sh", "tools/wit/wit"}
PROTECTED_ROOTS = {"saves", "mods", "textures", "game", "debug", "logs", ".uc-update"}
PROTECTED_NAMES = {"cpu-samples.txt", "steam-env.txt", "next-run.sh", "launcher-state.json", "release.json",
                   "data/pipeline_cache.bin", "data/pipeline_cache.tmp", "data/pipeline_keys.bin"}


RESERVED_NAMES = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}


def check_names(names) -> None:
    """The rules every client applies to a manifest path (path_key in updater/release.cpp): a name that breaks them would make the release fail as "Invalid manifest" on every Deck."""
    seen = set()
    for name in names:
        try:
            name.encode("utf-8")
        except UnicodeEncodeError as error:
            raise ValueError(f"Path is not valid UTF-8: {name!r}") from error
        if not name or len(name) > 32760 or any(ord(c) < 32 or ord(c) == 127 or c in '\\:~<>"|?*' for c in name):
            raise ValueError(f"Invalid character in path: {name!r}")
        for part in name.split("/"):
            base = part.split(".")[0].upper()
            if part in ("", ".", "..") or part.endswith((".", " ")) or base in RESERVED_NAMES or (len(base) == 4 and base[:3] in ("COM", "LPT") and (base[3].isdigit() or base[3] in "¹²³")):
                raise ValueError(f"Invalid path component in: {name!r}")
        if name.upper() in seen:
            raise ValueError(f"Duplicate path (case-insensitive): {name!r}")
        seen.add(name.upper())


def write_manifest(stage: Path, version: str) -> dict:
    parse_version(version)
    paths = files_in(stage)
    names = {path.relative_to(stage).as_posix() for path in paths}
    if not REQUIRED <= names:
        raise ValueError(f"Deck install is missing required files: {sorted(REQUIRED - names)}")
    if not any(name.startswith("data/shaders/") and name.endswith(".spv") for name in names):
        raise ValueError("Deck install has no shaders (data/shaders/*.spv)")
    check_names(sorted(names))
    for name in names:
        lowered = name.casefold()
        if (lowered.split("/")[0] in PROTECTED_ROOTS or lowered.startswith(("config.json", "cpu-profile.json"))
                or lowered in PROTECTED_NAMES):
            raise ValueError(f"User data cannot be included in a release: {name}")
    manifest = {"format": 1, "version": version, "helper_files": [], "files": [
        {"path": path.relative_to(stage).as_posix(), "size": path.stat().st_size, "sha256": sha256(path)} for path in paths]}
    with (stage / "release.json").open("x", encoding="utf-8", newline="\n") as output:
        json.dump(manifest, output, ensure_ascii=False, indent=2)
        output.write("\n")
    return manifest


def package(stage: Path, output: Path) -> None:
    with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as archive:
        for path in files_in(stage):
            name = path.relative_to(stage).as_posix()
            info = zipfile.ZipInfo.from_file(path, name)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3   # Unix: from_file() says MS-DOS on Windows, and unzip/libarchive then drop the mode bits below
            info.external_attr = (stat.S_IFREG | (0o755 if name in EXECUTABLES else 0o644)) << 16
            with path.open("rb") as source, archive.open(info, "w") as target:
                shutil.copyfileobj(source, target, 1024 * 1024)


def build_deck_release(repo: Path, version: str, stage: Path) -> Path:
    parse_version(version)
    repo = repo.resolve(strict=True)
    final = repo / f"UC-SteamDeck-{version}.zip"
    if final.exists():
        raise FileExistsError(f"Release archive already exists: {final}")
    fd, temporary = tempfile.mkstemp(prefix=".deck-release-", suffix=".zip", dir=repo)
    os.close(fd)
    os.unlink(temporary)
    manifest = stage / "release.json"
    foreign = manifest.exists()   # not ours: refused by write_manifest and never deleted here
    try:
        write_manifest(stage, version)   # a failed check leaves nothing in the repo folder
        package(stage, Path(temporary))
        os.rename(temporary, final)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
        if not foreign and manifest.is_file() and not manifest.is_symlink():   # the caller's folder is left as it was, so that a rerun (another version, a fix) works
            manifest.unlink()
    print(f"Release: {final}\nSHA256:  {sha256(final)}\n{checklist(version)}")
    return final


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--version", required=True, help="X.Y.Z")
    parser.add_argument("--stage", required=True, type=Path, help="the cmake --install tree of the Linux build")
    args = parser.parse_args()
    try:
        build_deck_release(Path(__file__).resolve().parent.parent, args.version, args.stage)
        return 0
    except (OSError, ValueError) as error:
        print(f"Release failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

"""Build and publish a clean Windows release locally."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import uuid
import zipfile


def parse_version(text: str) -> str:
    if not re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", text):
        raise ValueError("Version must be X.Y.Z, for example 0.0.1 (digits only, no leading zeros)")
    if any(len(part) > 5 or int(part) > 65535 for part in text.split(".")):
        raise ValueError("Each version component must be 0..65535")
    return text


def ordinary(path: Path):
    info = path.lstat()
    if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
        raise ValueError(f"Links and reparse points are not allowed: {path}")
    return info


def files_in(root: Path) -> list[Path]:
    if not stat.S_ISDIR(ordinary(root).st_mode):
        raise ValueError(f"Not a directory: {root}")
    files = []
    def visit(directory):
        for path in sorted(directory.iterdir()):
            mode = ordinary(path).st_mode
            if stat.S_ISDIR(mode):
                visit(path)
            elif stat.S_ISREG(mode):
                files.append(path)
            else:
                raise ValueError(f"Not an ordinary file: {path}")
    visit(root)
    return sorted(files, key=lambda path: path.relative_to(root).as_posix())


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_manifest(stage: Path, version: str, helper_files: list[str]) -> dict:
    parse_version(version)
    paths = files_in(stage)
    names = {path.relative_to(stage).as_posix() for path in paths}
    required = {"uc.exe", "uc_launcher.exe", "uc_updater.exe", "data/uc.map", "assets/launcher/graphite-train.png", "assets/launcher/graphite-brand.png"}
    if not required <= names or not any(name.startswith("data/shaders/") and name.endswith(".spv") for name in names):
        raise ValueError("Clean install is missing required client files or shaders")
    if "release.json" in names:
        raise ValueError("Staging already contains release.json")
    if not helper_files or helper_files[0] != "uc_updater.exe" or len(set(map(str.casefold, helper_files))) != len(helper_files):
        raise ValueError("Invalid helper runtime metadata")
    for name in helper_files:
        if name not in names or "/" in name or "\\" in name or (name != "uc_updater.exe" and not name.lower().endswith(".dll")):
            raise ValueError(f"Missing or invalid helper dependency: {name}")
    protected = {"saves", "mods", "textures", "game", "debug", "logs", ".uc-update"}
    for name in names:
        lowered = name.casefold()
        if lowered.split("/")[0] in protected or lowered.startswith(("config.json", "cpu-profile.json")) or lowered in {
            "cpu-samples.txt", "nvngx_dlssnr.dll", "data/pipeline_cache.bin", "data/pipeline_cache.tmp", "data/pipeline_keys.bin"
        }:
            raise ValueError(f"User data cannot be included in a release: {name}")
    manifest = {"format": 1, "version": version, "files": [
        {"path": path.relative_to(stage).as_posix(), "size": path.stat().st_size, "sha256": sha256(path)} for path in paths
    ], "helper_files": helper_files}
    with (stage / "release.json").open("x", encoding="utf-8", newline="\n") as output:
        json.dump(manifest, output, ensure_ascii=False, indent=2)
        output.write("\n")
    return manifest


def package_release(stage: Path, version: str, output: Path) -> Path:
    parse_version(version)
    paths = files_in(stage)
    manifest = json.loads((stage / "release.json").read_text(encoding="utf-8"))
    if manifest["version"] != version:
        raise ValueError("Archive and manifest versions differ")
    with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as archive:
        for path in paths:
            archive.write(path, path.relative_to(stage).as_posix())
    return output


def cleanup_workspace(work: Path, repo: Path, owner: str):
    if work.parent != repo or not work.name.startswith(".release-build-") or work.resolve() != work:
        raise ValueError(f"Refusing cleanup outside owned workspace: {work}")
    ordinary(work)
    if json.loads((work / ".owner.json").read_text(encoding="utf-8")) != {"owner": owner}:
        raise ValueError(f"Workspace ownership changed: {work}")
    # Python 3.8+ rmtree removes junctions without traversing their contents.
    shutil.rmtree(work)


@contextmanager
def release_lock(repo: Path):
    import msvcrt
    path = repo / ".release-build.lock"
    if path.exists():
        ordinary(path)
    with path.open("a+b") as lock:
        lock.seek(0)
        try:
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError as error:
            raise OSError("Another release build is already running") from error
        try:
            lock.seek(0)
            marker = lock.read()
            if marker not in (b"", b"UC release builder\n"):
                raise ValueError(f"Unexpected release lock file: {path}")
            if not marker:
                lock.write(b"UC release builder\n")
                lock.flush()
            yield
        finally:
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)


def build_release(repo: Path, version: str, *, runner=None) -> Path:
    version = parse_version(version)
    repo = repo.resolve(strict=True)
    with release_lock(repo):
        return _build_release(repo, version, runner=runner)


def _build_release(repo: Path, version: str, *, runner=None) -> Path:
    version = parse_version(version)
    repo = repo.resolve(strict=True)
    runner = runner or subprocess.run
    final = repo / f"UC-Windows-{version}.zip"
    destination = repo / "dist-release"
    if final.exists():
        raise FileExistsError(f"Release archive already exists: {final}")
    if destination.exists():
        ordinary(destination)
        if not destination.is_dir():
            raise ValueError(f"Release destination is not a directory: {destination}")
    owner = str(uuid.uuid4())
    work = Path(tempfile.mkdtemp(prefix=".release-build-", dir=repo))
    (work / ".owner.json").write_text(json.dumps({"owner": owner}), encoding="utf-8")
    stage, previous = work / "client", work / "previous"
    archive = work / final.name
    retain = False
    try:
        # UC_ALL_TESTS makes scripts/tests-run.ps1 run every test, not only the ones changed since their last pass.
        runner([str(repo / "build.bat"), "--no-install"], cwd=repo, check=True, env={**os.environ, "UC_ALL_TESTS": "1"})
        runner(["cmake", "--install", str(repo / "build"), "--prefix", str(stage)], cwd=repo, check=True)
        actual = {path.resolve() for path in files_in(stage)}
        listed = {Path(line).resolve() for line in (repo / "build/install_manifest.txt").read_text(encoding="utf-8").splitlines() if line}
        if actual != listed or any(not path.is_relative_to(stage) for path in listed):
            raise ValueError("Clean staging differs from CMake install_manifest.txt")
        helpers = (repo / "build/updater-runtime-files.txt").read_text(encoding="utf-8").splitlines()
        write_manifest(stage, version, helpers)
        package_release(stage, version, archive)
        validator = repo / "build/tests/test_update_package.exe"
        if not validator.is_file():
            raise ValueError("Native release validator is missing")
        digest = sha256(archive)
        runner([str(validator), "--verify-release", str(archive), version, digest, str(archive.stat().st_size)], cwd=repo, check=True)
        moved_previous = published_stage = False
        try:
            if destination.exists():
                destination.rename(previous)
                moved_previous = True
            stage.rename(destination)
            published_stage = True
            archive.rename(final)
        except OSError:
            try:
                if published_stage:
                    destination.rename(work / "failed-client")
                if moved_previous:
                    previous.rename(destination)
            except OSError as rollback:
                retain = True
                raise OSError(f"Publication rollback failed; previous output retained at {previous}: {rollback}") from rollback
            raise
        print(f"Release: {final}\nClient:  {destination}\nSHA256:  {digest}")
        return final
    finally:
        if not retain:
            try:
                cleanup_workspace(work, repo, owner)
            except (OSError, ValueError) as error:
                print(f"Cleanup deferred: {work}: {error}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", help="X.Y.Z; otherwise asks interactively")
    args = parser.parse_args()
    try:
        version = args.version if args.version is not None else input("Версия релиза (например, 0.0.1): ")
        build_release(Path(__file__).resolve().parent.parent, version)
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError, EOFError) as error:
        print(f"Release failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

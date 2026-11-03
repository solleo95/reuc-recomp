#!/bin/sh
# Installs Resident Evil: The Umbrella Chronicles on a Steam Deck (run it in Desktop Mode, in Konsole):
#   curl -fsSL https://raw.githubusercontent.com/solleo95/reuc-recomp/main/scripts/linux/install-steamdeck.sh | bash
# or download this file and run `bash install-steamdeck.sh`. Run it again to update; saves, mods and the game folder are never touched.
# Started without a terminal (a double click in the file manager) it shows a progress window and an error dialog (zenity) instead of text.
#
# It downloads the newest UC-Linux-X.Y.Z.zip release into ~/Games/UmbrellaChronicles, adds uc-launcher.sh to Steam as a non-Steam game with
# its artwork, switches Steam Input off for it and gives it a gamepad layout (right trackpad as a mouse, its haptic feedback and trackball mode off) without gyro. Steam is closed for a moment (it rewrites its
# files on exit) and started again.
#
# Options (for testing): --zip <file|url>   --art <folder|url holding cover.jpg widebanner-capsule.jpg banner.jpg game-logo.png>
#                        --dest <folder>    --steam <Steam folder>
command -v python3 >/dev/null 2>&1 || { echo "error: python3 is required" >&2; exit 1; }
exec python3 - "$@" <<'PY'
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time
import urllib.request
import zipfile
import zlib

REPO = "solleo95/reuc-recomp"
TITLE = "Resident Evil: The Umbrella Chronicles"
ENTRY = "uc-launcher.sh"
EXECUTABLES = ("uc", "uc_launcher", "uc-launcher.sh", "uc.sh", "tools/wit/wit")
LAYOUT_KEY = re.sub(r"[^a-z0-9 -]", "", TITLE.lower())   # how Steam names the controller layout folder of a non-Steam game: its name in lower case without punctuation
LAYOUT_TEMPLATE = "controller_neptune_gamepad+mouse.vdf"   # Steam's own "Gamepad with Mouse Trackpad": right trackpad is a mouse, no gyro group
ART = (("cover.jpg", "{id}p.jpg"), ("widebanner-capsule.jpg", "{id}.jpg"), ("banner.jpg", "{id}_hero.jpg"), ("game-logo.png", "{id}_icon.png"))   # cover, wide cover, banner (hero), icon; no logo. The wide cover is widebanner.jpg centred on black at Steam's 2.14:1 (the 3.1:1 original is cropped by Steam)


class Fail(Exception):
    pass


dialog = None   # the zenity progress window; set only when the script runs without a terminal


def open_dialog() -> None:
    """Started without a terminal (from the file manager, a shortcut): the messages also go to a window with a progress bar."""
    global dialog
    if sys.stdout.isatty() or not shutil.which("zenity") or not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        return
    dialog = subprocess.Popen(["zenity", "--progress", "--no-markup", "--no-cancel", "--width=460", "--title", TITLE, "--text", "Starting..."],
                              stdin=subprocess.PIPE, text=True)


def say(text: str, percent: int | None = None, label: str | None = None, error: bool = False) -> None:
    """Prints a line and, if there is a window, shows it as the window's text (or `label`) with the progress `percent`."""
    global dialog
    print(text, file=sys.stderr if error else sys.stdout, flush=True)
    if dialog:
        try:
            dialog.stdin.write(f"# {label or text.strip()}\n" + (f"{percent}\n" if percent is not None else ""))
            dialog.stdin.flush()
        except OSError:   # the window was closed; the install goes on
            dialog = None


def close_dialog(failure: str | None = None) -> None:
    """After success the window keeps its last text until the player presses OK; after a failure it becomes an error dialog."""
    if not dialog:
        return
    if failure:
        dialog.terminate()
        subprocess.run(["zenity", "--error", "--no-markup", "--width=460", "--title", TITLE, "--text", failure])
        return
    try:
        dialog.stdin.close()
    except OSError:
        pass
    dialog.wait()


def fetch(source: str, target: Path, span: tuple[int, int] | None = None) -> None:
    """Copies a local file or downloads a URL; a big one (with `span`, its share of the window's progress) gets a progress line."""
    target.parent.mkdir(parents=True, exist_ok=True)
    if not source.startswith(("http://", "https://")):
        shutil.copyfile(source, target)
        return
    request = urllib.request.Request(source, headers={"User-Agent": "uc-install-steamdeck"})
    with urllib.request.urlopen(request, timeout=60) as response, open(target, "wb") as output:
        total = int(response.headers.get("Content-Length") or 0)
        done = 0
        shown = -1
        while chunk := response.read(1 << 20):
            output.write(chunk)
            done += len(chunk)
            if span and total and done * 10 // total != shown:
                shown = done * 10 // total
                say(f"  {done >> 20} of {total >> 20} MB", span[0] + (span[1] - span[0]) * done // total,
                    f"Downloading the game: {done >> 20} of {total >> 20} MB")


def latest_linux_release() -> dict:
    request = urllib.request.Request(f"https://api.github.com/repos/{REPO}/releases?per_page=20", headers={"User-Agent": "uc-install-steamdeck"})
    with urllib.request.urlopen(request, timeout=30) as response:
        releases = json.load(response)
    for release in releases:
        if release.get("draft") or release.get("prerelease"):
            continue
        for asset in release.get("assets", []):
            if re.fullmatch(r"UC-Linux-\d+\.\d+\.\d+\.zip", asset["name"]):
                return asset
    raise Fail("no UC-Linux-X.Y.Z.zip in the latest releases of " + REPO)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        while chunk := source.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def install_game(zip_path: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path) as archive:
        if "release.json" not in archive.namelist() or ENTRY not in archive.namelist():
            raise Fail(f"{zip_path.name} is not a Linux release of the game (no {ENTRY})")
        archive.extractall(dest)   # sanitises the member names; the archive holds program files only, user data in dest stays
    for name in EXECUTABLES:   # zipfile drops the Unix mode bits
        (dest / name).chmod(0o755)


def running(name: str) -> bool:
    try:
        return subprocess.run(["pgrep", "-x", name], capture_output=True).returncode == 0
    except FileNotFoundError:
        return False


def stop_steam() -> bool:
    """Closes Steam if it runs (it rewrites shortcuts.vdf and localconfig.vdf when it exits); True if it did."""
    if not running("steam"):
        return False
    say("Closing Steam (in Game Mode the session restarts by itself)...", 90)
    subprocess.run(["steam", "-shutdown"], capture_output=True)
    for _ in range(120):
        if not running("steam") and not running("steamwebhelper"):
            time.sleep(1)
            return True
        time.sleep(0.5)
    raise Fail("Steam did not close; close it and run the installer again")


def write(path: Path, data) -> None:
    """Replaces a file, keeping the first version as <name>.uc-backup."""
    path.parent.mkdir(parents=True, exist_ok=True)
    backup = path.with_name(path.name + ".uc-backup")
    if path.exists() and not backup.exists():
        shutil.copyfile(path, backup)
    temporary = path.with_name(path.name + ".uc-tmp")
    temporary.write_bytes(data if isinstance(data, bytes) else data.encode("utf-8"))
    os.replace(temporary, path)


def add_shortcut(path: Path, appid: int, exe: str, start: str, icon: str) -> bool:
    """Appends the game to the binary shortcuts.vdf; False if an entry with the same exe is already there."""
    data = path.read_bytes() if path.exists() else b"\x00shortcuts\x00\x08\x08"
    if re.search(rb"\x01exe\x00" + re.escape(exe.encode()) + rb"\x00", data):
        return False
    text = lambda key, value: b"\x01" + key.encode() + b"\x00" + value.encode() + b"\x00"
    number = lambda key, value: b"\x02" + key.encode() + b"\x00" + struct.pack("<I", value)
    entry = (number("appid", appid) + text("appname", TITLE) + text("exe", exe) + text("StartDir", start) + text("icon", icon)
             + text("ShortcutPath", "") + text("LaunchOptions", "") + number("IsHidden", 0) + number("AllowDesktopConfig", 1)
             + number("AllowOverlay", 1) + number("OpenVR", 0) + number("Devkit", 0) + text("DevkitGameID", "")
             + number("DevkitOverrideAppID", 0) + number("LastPlayTime", 0) + text("FlatpakAppID", "") + text("sortas", "")
             + b"\x00tags\x00\x08\x08")
    index = max([int(i) for i in re.findall(rb"\x00(\d+)\x00[\x01\x02]", data)], default=-1) + 1
    if not data.endswith(b"\x08\x08"):
        raise Fail(f"{path} has an unexpected format")
    write(path, data[:-2] + b"\x00" + str(index).encode() + b"\x00" + entry + b"\x08\x08")
    return True


def disable_steam_input(path: Path, keys) -> None:
    """localconfig.vdf: "UseSteamControllerConfig" "0" (Steam Input off) in the top-level "apps" block for each key."""
    text = path.read_text(encoding="utf-8")
    line = '\t\t\t"UseSteamControllerConfig"\t\t"0"\n'
    for key in keys:
        apps = re.search(r'^\t"apps"\n\t\{\n(.*?)^\t\}\n', text, re.M | re.S)
        if not apps:
            tail = text.rstrip()
            if not tail.endswith("}"):
                raise Fail(f"{path} has an unexpected format")
            text = tail[:-1] + f'\t"apps"\n\t{{\n\t\t"{key}"\n\t\t{{\n{line}\t\t}}\n\t}}\n}}\n'
            continue
        app = re.search(rf'^\t\t"{key}"\n\t\t\{{\n(.*?)^\t\t\}}\n', apps.group(1), re.M | re.S)
        if not app:
            body = apps.group(1) + f'\t\t"{key}"\n\t\t{{\n{line}\t\t}}\n'
        elif '"UseSteamControllerConfig"' in app.group(1):
            changed = re.sub(r'("UseSteamControllerConfig"\t\t)"[^"]*"', r'\1"0"', app.group(1))
            body = apps.group(1).replace(app.group(1), changed)
        else:
            body = apps.group(1).replace(app.group(1), line + app.group(1))
        text = text[:apps.start(1)] + body + text[apps.end(1):]
    write(path, text)


def plain_right_trackpad(template: str, url: str) -> str:
    """Steam's template as a per-game layout with the haptic feedback and the trackball mode of the right trackpad off."""
    text = template.replace("\r\n", "\n").replace("{\n", f'{{\n\t"progenitor"\t\t"default://{LAYOUT_KEY}"\n\t"url"\t\t"autosave://{url}"\n', 1)
    active = re.search(r'"(\d+)"\t\t"right_trackpad active"', text)
    group = active and re.search(rf'^\t"group"\n\t\{{\n\t\t"id"\t\t"{active.group(1)}"\n.*?^\t\}}\n', text, re.M | re.S)
    if not group:
        raise Fail("the Steam template has no right trackpad group; the layout was not written")
    block = group.group(0)
    settings = re.search(r'^\t\t"settings"\n\t\t\{\n(.*?)^\t\t\}\n', block, re.M | re.S)
    body = settings.group(1) if settings else ""
    for key in ("haptic_intensity", "trackball"):
        if f'"{key}"' in body:
            body = re.sub(rf'("{key}"\t\t)"[^"]*"', r'\1"0"', body)
        else:
            body += f'\t\t\t"{key}"\t\t"0"\n'
    if settings:
        changed = block.replace(settings.group(1), body)
    else:
        changed = block[:block.rindex("\t}\n")] + f'\t\t"settings"\n\t\t{{\n{body}\t\t}}\n\t}}\n'
    return text[:group.start()] + changed + text[group.end():]


def set_layout(steam: Path, account: str) -> None:
    """The Deck layout of the game: Steam's "Gamepad with Mouse Trackpad" (right trackpad is a mouse, no gyro group), right trackpad haptics and trackball off.
    Written only if the game has none yet: Steam saves the player's changes in that file."""
    config = steam / "steamapps" / "common" / "Steam Controller Configs" / account / "config"
    layout = config / LAYOUT_KEY / "controller_neptune.vdf"
    if not layout.exists():
        write(layout, plain_right_trackpad((steam / "controller_base" / "templates" / LAYOUT_TEMPLATE).read_text(encoding="utf-8-sig"), str(layout)))
    path = config / "configset_controller_neptune.vdf"
    text = path.read_text(encoding="utf-8") if path.exists() else '"controller_config"\n{\n}\n'
    entry = f'\t"{LAYOUT_KEY}"\n\t{{\n\t\t"autosave"\t\t"1"\n\t}}\n'
    old = re.search(rf'^\t"{LAYOUT_KEY}"\n\t\{{\n.*?^\t\}}\n', text, re.M | re.S)   # an earlier install may have set a template instead
    if old:
        write(path, text[:old.start()] + entry + text[old.end():])
        return
    tail = text.rstrip()
    if not tail.endswith("}"):
        raise Fail(f"{path} has an unexpected format")
    write(path, tail[:-1] + entry + "}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--zip")
    parser.add_argument("--art", default=f"https://raw.githubusercontent.com/{REPO}/main/assets")
    parser.add_argument("--dest", type=Path, default=Path.home() / "Games" / "UmbrellaChronicles")
    parser.add_argument("--steam", type=Path, default=Path.home() / ".local" / "share" / "Steam")
    args = parser.parse_args()
    dest, steam = args.dest.resolve(), args.steam
    profiles = sorted((steam / "userdata").glob("*/config/localconfig.vdf"), key=lambda p: p.stat().st_mtime) if (steam / "userdata").is_dir() else []
    profiles = [p for p in profiles if p.parent.parent.name != "0"]
    if not profiles:
        raise Fail(f"no Steam user found in {steam / 'userdata'}; sign in to Steam once and run the installer again")
    config = profiles[-1].parent   # the profile Steam wrote last
    account = config.parent.name
    work = dest.parent / ".uc-install-tmp"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    restart = False
    try:
        if args.zip:
            source = args.zip
        else:
            asset = latest_linux_release()
            source = asset["browser_download_url"]
            say(f"Latest release: {asset['name']}", 2)
        archive = work / "game.zip"
        say("Downloading the game...", 5)
        fetch(source, archive, (5, 75))
        digest = (asset.get("digest") or "") if not args.zip else ""
        if digest.startswith("sha256:") and sha256(archive) != digest[7:]:
            raise Fail("the downloaded archive is damaged (checksum mismatch); run the installer again")
        say(f"Unpacking into {dest} ...", 78)
        install_game(archive, dest)
        exe, start = f'"{dest / ENTRY}"', f'"{dest}/"'
        appid = zlib.crc32((exe + TITLE).encode()) | 0x80000000   # how Steam numbers a non-Steam game
        grid = config / "grid"
        say("Adding the game to Steam...", 85)
        for name, target in ART:
            try:
                fetch(args.art.rstrip("/") + "/" + name if args.art.startswith("http") else str(Path(args.art) / name), work / name)
                grid.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(work / name, grid / target.format(id=appid))
            except OSError as error:   # a missing picture must not stop the install
                say(f"warning: {name} not set: {error}", error=True)
        restart = stop_steam()
        added = add_shortcut(config / "shortcuts.vdf", appid, exe, start, str(grid / f"{appid}_icon.png"))
        disable_steam_input(config / "localconfig.vdf", {str(appid), str(appid - (1 << 32))})   # Steam writes the key unsigned or signed
        set_layout(steam, account)
    finally:
        shutil.rmtree(work, ignore_errors=True)
        if restart and not running("gamescope-wl"):   # in Game Mode the session starts Steam again by itself
            subprocess.Popen(["steam"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    say(f"Done: {TITLE} {'added to' if added else 'was already in'} Steam (installed in {dest}).", 100)
    return 0


open_dialog()
try:
    code = main()
    close_dialog()
    sys.exit(code)
except (Fail, OSError, zipfile.BadZipFile) as error:
    print(f"error: {error}", file=sys.stderr)
    close_dialog(f"Installation failed: {error}")
    sys.exit(1)
PY

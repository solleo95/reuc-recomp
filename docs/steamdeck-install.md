# Steam Deck: installing, updating and releasing

Player's guide (controls, aiming, menus, troubleshooting): [steamdeck.md](steamdeck.md) · [по-русски](steamdeck.ru.md). This page is the technical side.

## Automatic install

In Desktop Mode, in Konsole:

```bash
curl -fsSL https://raw.githubusercontent.com/solleo95/reuc-recomp/main/scripts/linux/install-steamdeck.sh | bash
```

(or download `scripts/linux/install-steamdeck.sh` and run `bash install-steamdeck.sh`). The script downloads the newest `UC-Linux-<version>.zip` release into
`~/Games/UmbrellaChronicles`, adds `uc-launcher.sh` to Steam as "Resident Evil: The Umbrella Chronicles" with the cover, wide cover, banner and icon from
`assets/` (`cover.jpg`, `widebanner-capsule.jpg` (`widebanner.jpg` centred on black at Steam's wide-cover proportions, so Steam does not crop it), `banner.jpg`, `game-logo.png`, fetched from the repository's `main` branch), switches Steam Input off for it and gives it
a gamepad layout (right trackpad as a mouse, its haptic feedback and trackball mode off) without gyro: Steam's own "Gamepad with Mouse Trackpad" template, saved as the game's
layout (only if the game has none yet, so a layout you changed in Steam is kept). Steam looks for it in a folder named after the game (`resident evil the umbrella chronicles`). Steam is closed for a moment and started again (in Game Mode the session restarts it). The first version of each Steam file it
changes is kept next to it as `*.uc-backup`. Running it again updates the game; saves, mods and the `game` folder stay. Needs `python3` (SteamOS has it).
Test options: `--zip <file|url>`, `--art <folder|url>`, `--dest <folder>`, `--steam <Steam folder>`.

## Playing

1. Download `UC-Linux-<version>.zip` from the latest release on GitHub (Desktop Mode, any browser) and extract it where there is room: the game folder
   (about 4 GB) and the saves live next to the program, so the SD card works as well as the internal drive.
2. In Desktop Mode: Steam > Games > Add a Non-Steam Game > Browse, show all files, pick `uc-launcher.sh` from the extracted folder. Rename the entry if you like.
3. Start it from Game Mode. `uc-launcher.sh` opens the launcher; the launcher starts the game through `uc.sh` when you press **Singleplayer**.
4. On the **Game** tab choose the game: **Browse** a folder with the extracted game, or **Select a disc image** (`.iso`, `.wbfs`, `.ciso`, `.wdf`, `.wia` of the
   NTSC-U disc); a disc image is unpacked into the `game` folder next to the launcher when you press Play (about a minute, 4 GB of free space).
5. **Graphics** and **Controls** hold the settings of the Steam Deck (gyro, sticks, bindings, reticle). Every change is saved at once.
6. **Mods** lists the mods found in the `mods` folder next to the launcher (a mod is a folder or a `.zip` with a `mod.json`): switch them on and off, change their
   load order (the arrows), allow native (DLL) mods and confirm a DLL, and open a mod's settings with its gear. This is the same manager as in the game's menu, saved
   in `config.json` before the game starts. What a mod draws by itself (its own tabs and live status) shows only in the game.

Pad: the D-pad works on the right part at once: up/down move between rows, left/right change the focused row (a slider, a switch, the neighbouring choice) or, on a
mod, select its switch, gear and arrows; A switches or opens a list, X adds and Y clears a binding. L1 / R1 move the selection on the left: Game, Graphics,
Controls, Mods, then the Language select (A opens its list). B in a list goes back; B anywhere else on the main screen asks whether to close the launcher and
return to Steam. Touch works too.

## Game Mode and the desktop view

The launcher and the in-game menu come in two views:

- **Deck view** (the pages above, the Deck menu on the **View** button): used in Game Mode, that is when the program runs inside a gamescope session
  (`GAMESCOPE_WAYLAND_DISPLAY` is set), on the Steam Deck and on other SteamOS devices with a Game Mode.
- **Desktop view**: everywhere else (Desktop Mode, any Linux desktop). The launcher is the one of the Windows version (Game, Graphics, Controls, Controllers,
  Mods; no Direct3D choice, DLSS or network play, which Linux does not have) and the game's menu opens on **Insert**, as on Windows.

Start `uc_launcher` or `uc-launcher.sh` (or the game itself, `uc` or `uc.sh`) from a terminal or a file manager. To force a view, whatever the session: `--ui desktop`
or `--ui deck`, or the variable `UC_UI=desktop|deck` (the flag wins). Anything else is an error. The launcher gives the game the view it runs in, so a forced view
stays for the game. A line `[ui] mode <view> (flag|env|gamescope|default)` in the game's log and `[ui] launcher view <view>` in the launcher's tell which one started.

To check the desktop launcher without opening a window by hand: `uc_launcher --ui desktop --snapshot <folder>` draws the five pages and writes
`desktop-launcher-<page>.bmp` (pages 0 Game, 1 Graphics, 2 Controls, 3 Controllers, 5 Mods) without touching the install's `config.json`.

## Updating

When a newer `UC-Linux-<version>.zip` is the latest release, the launcher offers to install it (once a start; "Later" skips it until the next start).
The update replaces only the program files; `config.json`, `saves/`, `mods/`, `textures/`, `game/` and the logs are never touched. An update that was
interrupted (power cut, crash) is rolled back the next time the launcher starts.

## Releasing (for the maintainer)

Every release carries **both** archives and is published as the **latest** release:

- `UC-Windows-<version>.zip` and `UC-Linux-<version>.zip` (the Linux archive is the one the Steam Deck uses). A platform that did not change keeps its previous archive (with its own version in the name).
- The tag is the release number; the version of an archive is the one in its name and in its `release.json`. Each launcher compares the version of its own
  archive in the latest release with the installed one, so a release that changes one platform is invisible to the other.
- Wait until every asset is uploaded before publishing (the launchers need the size and SHA-256 GitHub computes).
- The first release with this logic should change both platforms: Windows launchers released before it expect `UC-Windows-<tag>.zip` and show a dismissible
  error window on a release whose Windows archive has an older version.

Building the Steam Deck archive:

```bash
pwsh scripts/linux/build.ps1 -ThinLTO -Target uc_launcher,uc     # build-linux-lto: a release is always built with ThinLTO
python scripts/linux/deck.py package build/deck-release --build build-linux-lto
python scripts/build_release_deck.py --version 0.0.2 --stage build/deck-release
```

The result is `UC-Linux-0.0.2.zip` in the repository folder, and the script prints the publishing checklist. `build-release.bat` does all of this after the Windows archive (record the optimizer profile first with `pgo.bat linux`).

Renaming note: launchers released before the `UC-Linux-` name look for `UC-SteamDeck-<version>.zip` and cannot see a release that only has `UC-Linux-` archives. To let them update once, attach the same archive under both names to the first release with the new name (the second name can be dropped later).

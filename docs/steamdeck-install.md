# Steam Deck: installing, updating and releasing

## Playing

1. Download `UC-SteamDeck-<version>.zip` from the latest release on GitHub (Desktop Mode, any browser) and extract it where there is room: the game folder
   (about 4 GB) and the saves live next to the program, so the SD card works as well as the internal drive.
2. In Desktop Mode: Steam > Games > Add a Non-Steam Game > Browse, show all files, pick `uc-deck.sh` from the extracted folder. Rename the entry if you like.
3. Start it from Game Mode. `uc-deck.sh` opens the launcher; the launcher starts the game through `uc-game.sh` when you press **Play**.
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

## Updating

When a newer `UC-SteamDeck-<version>.zip` is the latest release, the launcher offers to install it (once a start; "Later" skips it until the next start).
The update replaces only the program files; `config.json`, `saves/`, `mods/`, `textures/`, `game/` and the logs are never touched. An update that was
interrupted (power cut, crash) is rolled back the next time the launcher starts.

## Releasing (for the maintainer)

Every release carries **both** archives and is published as the **latest** release:

- `UC-Windows-<version>.zip` and `UC-SteamDeck-<version>.zip`. A platform that did not change keeps its previous archive (with its own version in the name).
- The tag is the release number; the version of an archive is the one in its name and in its `release.json`. Each launcher compares the version of its own
  archive in the latest release with the installed one, so a release that changes one platform is invisible to the other.
- Wait until every asset is uploaded before publishing (the launchers need the size and SHA-256 GitHub computes).
- The first release with this logic should change both platforms: Windows launchers released before it expect `UC-Windows-<tag>.zip` and show a dismissible
  error window on a release whose Windows archive has an older version.

Building the Steam Deck archive:

```bash
pwsh scripts/linux/build.ps1 -Target uc_launcher,uc
python scripts/linux/deck.py package build/deck-release
python scripts/build_release_deck.py --version 0.0.2 --stage build/deck-release
```

The result is `UC-SteamDeck-0.0.2.zip` in the repository folder, and the script prints the publishing checklist.

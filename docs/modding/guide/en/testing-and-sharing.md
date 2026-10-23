# Testing and sharing

[Русский](../ru/testing-and-sharing.md) · **English**

You are here: [Modding guide](README.md) › **Testing and sharing**

How to try a mod, find out why it does not work, make it safe for others, pack it and publish it.

## Contents

- [The test loop](#the-test-loop)
- [Your tools](#your-tools)
- [Reading the Mods tab](#reading-the-mods-tab)
- [Reading the log](#reading-the-log)
- [Common messages](#common-messages)
- [When the game misbehaves](#when-the-game-misbehaves)
- [Checklist before you share](#checklist-before-you-share)
- [Versions and compatibility](#versions-and-compatibility)
- [Packing a mod](#packing-a-mod)
- [Telling players how to install](#telling-players-how-to-install)
- [Writing the description](#writing-the-description)
- [Updating a published mod](#updating-a-published-mod)
- [See also](#see-also)

## The test loop

1. Edit your files in `mods\<your mod>`.
2. In the game, press **Insert**, open the **Mods** tab, and (re)start the mod:
   - scripts: **Reload scripts**;
   - data patches: untick and tick the mod (they are applied at the mod's start);
   - textures, audio, disc files: they are noticed the next time the game opens them. Change the scene or **restart the game**;
   - text: **restart the game**;
   - DLLs: untick and tick the mod; to load a new build of a DLL restart the game, because Windows keeps the old file in use.
3. Look at the result in the game and at `debug\mods.log`.

Make the loop short: a test save that starts right at the interesting place saves minutes per try.

## Your tools

| Tool | What it gives you |
|---|---|
| `debug\mods.log` next to `uc.exe` | Every `log.write` of your scripts with the mod id in front, plus the game's messages about mods (skipped data keys, refused writes, errors). |
| The console output of the game | The same lines, and more: start the game from a command prompt (`uc.exe`) to see them live, including `[mods] conflict on ...` lines. |
| `uc.exe --log-dvd` | Prints every file the game opens and reads (useful for [disc files](files.md) and [audio](audio.md)). |
| `uc.exe --no-mods` | Starts the game with every mod off: use it to check whether a problem is yours. |
| **Reload scripts** (Mods tab) | Restarts the scripts without leaving the game. |
| The Mods tab itself | Shows each mod's state, the last error, the list of conflicting files, conversion progress. |
| `log.write(...)` | The simplest and best debugger. Log the values you doubt. |

## Reading the Mods tab

Under the name of each mod:

| You see | Meaning |
|---|---|
| `enabled` | Switched on and running. |
| `disabled` | Switched off. |
| `incompatible` | The mod needs another API version or another game version. |
| `missing dependency` / dependency is off | A mod listed in `depends` is missing or switched off. |
| `error` and a reason | `mod.json` is broken (the reason says where) or the mod could not start. |
| `stopped: ...` | The mod failed three times and was switched off; the last error follows. Untick and tick it to restart. |
| `waiting` | A native mod: native code is off or not confirmed. |
| a grey `converting audio (1 of 3)` | Audio is being converted. |
| red `audio: ...`, `text: ...` or `arc: ...` lines | Problems with the mod's audio or text files, or with the archive members under `files` ([Archive members](../../archives.md)). |

## Reading the log

Each line starts with the mod id in brackets:

```
[my-mod] hello from my mod
[my-mod] data/handgun.json: weapon.0.firerate: unknown member
```

Start from the **first** error: later ones are often consequences. Errors in a script include the file name; the line of a failing frame inside a function is approximate.

## Common messages

| Message or symptom | What it means and what to do |
|---|---|
| An error that names `touches_gameplay` | The mod writes to the game but does not declare it. Add `"touches_gameplay": true` to `mod.json`. |
| `player 1 is not in the game right now` | You read or wrote a player outside a mission. Check `uc.player(1).present` first. |
| `address 0x... is outside guest memory` | A wrong address in `uc.mem`. |
| a "time budget" error | A handler ran too long (1 ms per step for all `on_step` handlers). Do less per step. |
| `... is not implemented` or a `planned` member | The API member exists in the schema only; check its status in the [reference](../../api/README.md). |
| Three failures, then `stopped` | The mod is switched off for the session. Fix the error; untick and tick. |
| A data key is skipped | The log names the file, key and reason ([Data patches](data-patches.md#what-gets-skipped)). |
| The mod does not appear in the Mods tab | `mod.json` is missing or invalid, or the folder is not directly inside `mods`. Look for an `error` entry or the log. |
| The DLL is refused | Wrong API or ImGui version, not 64-bit, or no `uc_mod_get_info` ([Native DLL](native-dll.md#troubleshooting)). |

## When the game misbehaves

- **Run with `--no-mods`.** If the problem disappears, it is a mod.
- **Untick mods one by one**, or rename their folders, to find the culprit. Conflicts between mods that change the same thing are a common cause: reorder them.
- **Delete the mod's folder** (or rename it) to remove it completely. Settings stay in `config.json` and do no harm.
- A game that **crashes at once** after a mod is switched on: the mod probably writes a wrong value. Start with `--no-mods`, then switch mods on one at a time.
- Gameplay mods use the **normal saves**. After a bad experiment restore `saves\` from a backup.

## Checklist before you share

- [ ] `mod.json` has a unique `id`, a `name`, a `version` and the right `api`.
- [ ] `touches_gameplay` is `true` if (and only if) the mod changes how the game plays.
- [ ] It works when switched on **in a menu**, **in a mission**, and when switched **off again** mid-mission.
- [ ] It works with **other popular mods** switched on, and the load order does not matter more than necessary.
- [ ] Nothing in the log but your own lines (no errors, no skipped keys).
- [ ] The files are in the right folders: `scripts`, `data`, `textures`, `audio`, `text`, `files`, `native`.
- [ ] There are no leftover test files, big dumps, or your own saves in the folder.
- [ ] A DLL: the source is available or described, built for the right API and ImGui version.
- [ ] The description says what the mod changes, which game version it targets, whether it needs a restart, and what to back up.

## Versions and compatibility

- **`version`** is yours: raise it with every release (`1.1`, `1.2`, `2.0`). The Mods tab shows it. Dotted numbers compare numerically (`1.10` is newer than `1.2`).
- **`api`** is the API version you wrote for. A game with the same major version and an equal or newer minor version can run the mod; an older game shows the mod as `incompatible`. Use the **lowest** version that has what you need so more players can use it. The [changelog](../../api-changelog.md) lists what each version added.
- **`game`** can list game ids the mod supports (`["RBUE08"]` is the US release); leave it out if it works with any.
- **`depends`** lists mods that must be on (`{ "id": "core-lib", "min_version": "1.0" }`). They load before yours.
- Keep **`id`** and the **setting `key`s** stable between versions: the player's saved settings are filed under them.

## Packing a mod

Zip the mod folder. A mod can be a folder or a `.zip` in `mods\`:

```
my-mod.zip
  my-mod\            (one top-level folder is fine)
    mod.json
    scripts\main.lua
    ...
```

or with the files directly in the zip root (`mod.json` at the top). Both work. The game unpacks a zip once into `mods\.cache\`.

Rules the game enforces for zips: at most **20 000 entries** and **512 MiB** unpacked, and every path must stay inside the mod: an entry with `..` or an absolute path makes the game refuse the **whole zip**.

Before zipping, delete everything that is not part of the mod: test dumps, work files, backups.

## Telling players how to install

A short text for your page:

1. Download and unpack (or leave the `.zip` as it is).
2. Put the mod's folder (or the `.zip`) into the `mods` folder next to `uc.exe`.
3. Start the game, press **Insert**, open the **Mods** tab and tick the mod.
4. For text, textures and audio mods: restart the game.
5. Gameplay mods use the normal saves: back up the `saves` folder first.
6. Native (DLL) mods: turn on **Allow native mods** and confirm the DLL; do this only if you trust the author.
7. If something is wrong, start the game with `--no-mods` or untick the mod.

## Writing the description

Players want to know:

- what the mod does, with a screenshot or a short video;
- the game version it needs and the mod's own version;
- whether it changes gameplay (and so the saves);
- whether it needs a restart, **Allow native mods**, other mods;
- known problems and conflicts;
- how to uninstall (delete the folder);
- your name and where to report bugs.

## Updating a published mod

- Raise `version`, keep `id` and the setting `key`s, and say what changed.
- If you changed the meaning of a setting, give it a **new key** so the old saved value is not misread.
- Keep the old file name conventions of your textures, audio and text: they are the names the game looks up.
- If a newer game version changes the API, read the [changelog](../../api-changelog.md) and raise `api` only when you use something new.

## See also

- [Overview](README.md) - where everything lives and how mods combine.
- [Safety reference](../../safety.md) - what the port does to protect players.
- The topic guides: [Textures](textures.md), [Audio](audio.md), [Text](text.md), [Disc files](files.md), [Data patches](data-patches.md), [Lua](lua.md), [JavaScript](javascript.md), [Native DLL](native-dll.md).
- [mod.json reference](../../manifest.md)

Previous: [Native DLL](native-dll.md) · Back to [Overview](README.md)

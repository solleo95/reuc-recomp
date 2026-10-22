# Modding guide for Umbrella Chronicles PC

**English** · [Русский](../ru/README.md)

You are here: **Modding guide** (overview)

This is the starting point for everyone who wants to change the game: swap textures, music or text, tweak weapons, write scripts or build native DLLs. It explains how mods work in general and tells you which of the topic guides to read next. Every guide exists in English and in Russian and has the same name in both folders.

## Contents

- [How to use this guide](#how-to-use-this-guide)
- [What you can change](#what-you-can-change)
- [Quick start - your first mod in five minutes](#quick-start---your-first-mod-in-five-minutes)
  - [Step 1 - create the folder](#step-1---create-the-folder)
  - [Step 2 - write mod.json](#step-2---write-modjson)
  - [Step 3 - write the script](#step-3---write-the-script)
  - [Step 4 - switch it on](#step-4---switch-it-on)
  - [Step 5 - look at the log](#step-5---look-at-the-log)
- [Where everything lives](#where-everything-lives)
- [mod.json in one page](#modjson-in-one-page)
- [The Mods tab](#the-mods-tab)
- [How several mods work together](#how-several-mods-work-together)
- [A mod's life - switching on and off while the game runs](#a-mods-life---switching-on-and-off-while-the-game-runs)
- [Safety in short](#safety-in-short)
- [Which approach do I need](#which-approach-do-i-need)
- [Glossary](#glossary)
- [All guides](#all-guides)
- [Technical reference](#technical-reference)

## How to use this guide

- If you have never made a mod, read this page from the top, do the quick start, then open the topic guide that matches your idea (see [Which approach do I need](#which-approach-do-i-need)).
- If you already know what you want, jump straight to the topic guide from the table in [All guides](#all-guides). Each one is self-contained and starts with the same navigation block: language switch, a short "you are here" line, a table of contents, and links to the neighbouring guides at the end.
- The guides explain **how to do things**. The exact list of every field, function and value is in the [technical reference](#technical-reference); the guides link to it where it matters.
- Examples use the Lua language unless a guide is about JavaScript or C++. Everything a Lua script can do, a JavaScript script can do as well ([Lua guide](lua.md), [JavaScript guide](javascript.md)).

## What you can change

| You want to | What you write | Guide |
|---|---|---|
| Make the game look different: textures, UI pictures, fonts, effects | PNG files converted to DDS | [Textures](textures.md) |
| Replace music, voices or cutscene sound | `.ogg` or `.wav` files | [Audio](audio.md) |
| Change subtitles, item names, menu text, translate the game | JSON files | [Text](text.md) |
| Replace any other file of the game: models, movies, archives, layouts | Files in the disc layout | [Disc files](files.md) |
| Change numbers such as damage, magazine size, reload time without code | A JSON data patch | [Data patches](data-patches.md) |
| React to what happens in the game, change players, enemies, weapons | A Lua script | [Lua](lua.md) |
| The same, if you prefer JavaScript or TypeScript | A JavaScript module | [JavaScript](javascript.md) |
| Add sliders, checkboxes and buttons the player can use | Declared settings or UI code | [Settings and UI](settings-and-ui.md) |
| Use everything the game exposes: players, weapons, enemies, camera, input, events | The game API | [Game API](game-api.md) |
| Run your code inside a game function, or replace it | A hook | [Hooks](hooks.md) |
| Write something scripts are too slow or too limited for | A native DLL | [Native DLL](native-dll.md) |
| Test a mod, find out why it does not work, pack and share it | - | [Testing and sharing](testing-and-sharing.md) |

You can mix kinds freely. One mod can contain textures, a Lua script and a data patch at once.

## Quick start - your first mod in five minutes

We will make a mod that gives player 1 endless ammunition.

### Step 1 - create the folder

Open the folder that contains `uc.exe`. Inside it make the folders `mods\infinite-ammo\scripts` (the `mods` folder may already exist; create it if not).

### Step 2 - write mod.json

Create `mods\infinite-ammo\mod.json` with this content:

```json
{
  "id": "infinite-ammo",
  "name": "Infinite Ammo",
  "version": "1.0",
  "api": "1.0",
  "touches_gameplay": true,
  "script": "scripts/main.lua"
}
```

`touches_gameplay` is `true` because the mod changes how the game plays. Without it the game refuses every change the script tries to make.

### Step 3 - write the script

Create `mods\infinite-ammo\scripts\main.lua`:

```lua
uc.player(1).infinite_ammo = true
```

### Step 4 - switch it on

Start the game. Press **Insert** to open the menu and go to the **Mods** tab. Your mod is in the list; tick its box. It starts at once, even in the middle of a mission. Untick the box and it stops; everything it changed is given back.

### Step 5 - look at the log

Open `debug\mods.log` next to `uc.exe`. Every line your script writes with `log.write('text')` ends up there with the mod id in front. Errors appear there as well. Add `log.write('hello')` to the script, press **Reload scripts** in the Mods tab, and look at the log again.

Congratulations, that is a complete mod. The rest of the guides show how to do much more.

## Where everything lives

```
Umbrella Chronicles\              the folder with uc.exe
  uc.exe                          the game
  config.json                     settings of the game and the mods
  mods\                           your mods, one folder (or .zip) each
    my-mod\
      mod.json                    the description of the mod (the only required file)
      scripts\                    Lua or JavaScript            -> Lua / JavaScript guides
      data\                       data patches (JSON)          -> Data patches guide
      textures\                   replacement textures (DDS)   -> Textures guide
      audio\                      replacement audio (.ogg/.wav)-> Audio guide
      text\                       replacement text (JSON)      -> Text guide
      files\                      replacement disc files       -> Disc files guide
      native\                     a DLL                        -> Native DLL guide
    .cache\                       made by the game: unpacked zips, converted audio and text
  textures\                       the base folder for replacement textures
  tools\                          texture extractor and converter, text dump tool
  debug\mods.log                  the log of the mods
  saves\                          your saves (back them up before trying gameplay mods)
  game\DATA\files\                the game's own files (never edit them)
  docs\                           this documentation
  mods-examples\                  complete example mods
```

All paths inside a mod are **relative to the mod's folder**, use `/` as the separator in `mod.json`, and may not contain `..`.

## mod.json in one page

`mod.json` is the only required file of a mod. The minimum is five fields:

```json
{ "id": "my-mod", "name": "My Mod", "version": "1.0", "api": "1.1" }
```

| Field | What it means |
|---|---|
| `id` | A unique short name: lower-case letters, digits, `_` and `-`, up to 48 characters, starting with a letter or a digit. It is used in the config, the log and the cache. |
| `name`, `version` | Shown in the Mods tab. Versions are dotted numbers (`1.10` is newer than `1.2`). |
| `api` | The mod API version you wrote the mod for, `"major.minor"`. Use `"1.1"` for anything that uses enemies, events, game state, input or calling game functions, `"1.0"` otherwise. |
| `touches_gameplay` | Set `true` if the mod changes how the game plays. The game enforces it: without it, scripts cannot write to the game, and data patches and hooks are refused. |
| `script` | The Lua or JavaScript entry file. |
| `native` | A DLL ([Native DLL](native-dll.md)). |
| `settings` | Settings the player can change ([Settings and UI](settings-and-ui.md)). |
| `depends` | Other mods that must be on. |
| `audio` | Loop points for replaced music ([Audio](audio.md)). |
| `author`, `description`, `names`, `game` | Information for players. |

The full description of every field is in the [manifest reference](../../manifest.md).

## The Mods tab

Press **Insert** in the game to open the menu, then choose the **Mods** tab.

- **Checkbox** - switches a mod on or off at once. A mod that cannot work shows a reason (for example: wrong API version, missing dependency, error in `mod.json`).
- **Up and down arrows** - move the mod in the load order. The order decides which mod wins when two change the same thing.
- **Gear button** - opens the mod's settings window. It appears when the mod declares settings or draws its own UI.
- **Allow native mods** - a switch that is off by default. DLL mods run only when it is on and you have confirmed the DLL ([Native DLL](native-dll.md), [Safety in short](#safety-in-short)).
- **Reload scripts** - restarts the scripts of the running mods, so you can test an edit without restarting the game.
- Under each mod you may see grey or red lines: progress of audio conversion, and problems with audio or text files.

## How several mods work together

The order in the Mods tab is the **load order**. Mods are applied top to bottom; **the lower mod wins** when two provide the same thing.

| Kind | If two mods provide the same thing |
|---|---|
| Disc files (`files\`) | The lower mod's file is used. The Mods tab lists every conflicting path. |
| Textures (`textures\`) | The lower mod's texture wins; mods win over the base `textures\` folder. |
| Audio (`audio\`) | The lower mod wins per stream name. A ready `.brstm` in the same mod's `files\` beats its `audio\` file. |
| Text (`text\`) | Per record id, the lower mod wins. The game's console logs `text id N is set by A and B`. |
| Data patches and scripts | The last one that sets a value wins. |
| Hooks | All hooks run, in load order. |

`depends` in `mod.json` makes the game load a mod after the mods it needs and switch it off when one of them is off.

## A mod's life - switching on and off while the game runs

Players can tick and untick mods at any time, including in the middle of a fight. Write your mod for that:

- **Switched on:** the script (or DLL) loads, `on_load` runs, then the data patches apply. The game might be in a menu or a loading screen, so check `uc.player(1).present` before you assume a mission is running.
- **Switched off:** `on_unload` runs, the script is destroyed, hooks are removed, and **everything you set through the game API is given back** (switches like `invulnerable`, weapon values, data patches). Clean up anything of your own in `on_unload`.
- When a mod is switched on, off or moved, the mods **below** it restart in order so the result is the same as a fresh start. Do not keep state in a script that you cannot rebuild in `on_load`.
- Replaced files, textures, audio and text show up **when the game next opens them**. Some, like the text file, are read only once at start, so tell your players to restart the game after changing them ([Disc files](files.md#when-the-game-notices-a-change)).

## Safety in short

- **Scripts** (Lua, JavaScript) run in a sandbox: they cannot touch files, start programs or use the network. They also have time and memory limits; a script that fails three times in a session is switched off.
- **DLLs** run with the player's full rights. The game refuses them unless the player turns on **Allow native mods** and confirms each DLL. Only publish a DLL when you must, and tell players what it does.
- **Audio files** are decoded inside the game; damaged files can crash it.
- **Gameplay mods use the normal saves.** There are no separate saves for mods. Tell players to back up the `saves` folder.
- If something goes wrong, start the game with `--no-mods` to run without any mod, or untick the mod, or rename its folder.

More in the [safety reference](../../safety.md) and in [Testing and sharing](testing-and-sharing.md).

## Which approach do I need

| Your idea | Best approach | Why |
|---|---|---|
| "Make the handgun hit harder" | [Data patch](data-patches.md) | Numbers only: no code at all. |
| "Doubled damage, but the player can change the factor" | [Lua](lua.md) with [declared settings](settings-and-ui.md) | Needs logic and a slider. |
| "Heal the player when an enemy dies" | [Lua](lua.md) with [events](game-api.md#events) | React to `on_enemy_killed`. |
| "Another title music" | [Audio](audio.md) | Drop in an `.ogg`; no tools needed. |
| "Translate the game / rename items" | [Text](text.md) | JSON with the new strings. |
| "HD textures" | [Textures](textures.md) | PNG to DDS with the shipped converter. |
| "Replace a 3D model or a movie" | [Disc files](files.md) | Put the game-format file in the right place. |
| "Change what the weapon update does" | [Hooks](hooks.md) | Run your code in the game function. |
| "A heavy algorithm, an own overlay, calling Windows" | [Native DLL](native-dll.md) | Full speed and full rights. |

When in doubt, start with the simplest option that can do the job: a data patch beats a script, a script beats a DLL.

## Glossary

| Term | Meaning |
|---|---|
| **Mod** | A folder (or zip) in `mods\` with a `mod.json`. |
| **Game API** | Everything scripts and DLLs can read and change: the `uc` object, `log`, `settings`, `ui`, `hook`, `on`. |
| **Guest** | The game itself. The port runs the Wii game's code translated to PC; "guest memory" is the memory the Wii game sees. |
| **Guest address** | An address in that memory, like `0x80400000`. Main memory is `0x80000000` to `0x817FFFFF` (MEM1) and `0x90000000` to `0x93FFFFFF` (MEM2). Values are big-endian. |
| **Game step** | One update of the game. `on_step` is called about 30 times per game second. |
| **Slot** | Player 1 or player 2. |
| **Record id** | The number of a weapon in the weapon table (0 to 19) or of a text in a text file. |
| **Disc layout** | The folder structure of the game's own files (`Data\sound\stream\...`). |
| **Overlay** | How replacement files are laid over the game's files without changing them. |
| **Hook** | A function of yours called when the game calls one of its own functions. |
| **BRSTM** | The game's streaming audio format. |
| **DDS / BC7** | A texture file format and its compression; what the texture converter produces. |
| **TRES / txtres** | The game's text file format. |
| **Native mod** | A mod with a DLL. |

## All guides

| Guide | In one line | Русский |
|---|---|---|
| [Overview](README.md) | This page | [README](../ru/README.md) |
| [Textures](textures.md) | Extract, edit and convert textures | [Текстуры](../ru/textures.md) |
| [Audio](audio.md) | Replace music and voices with `.ogg` / `.wav` | [Звук](../ru/audio.md) |
| [Text](text.md) | Replace and translate texts with JSON | [Текст](../ru/text.md) |
| [Disc files](files.md) | Replace any file of the game | [Файлы диска](../ru/files.md) |
| [Data patches](data-patches.md) | Change values without code | [Патчи данных](../ru/data-patches.md) |
| [Lua](lua.md) | Scripts in Lua | [Lua](../ru/lua.md) |
| [JavaScript](javascript.md) | Scripts in JavaScript and TypeScript | [JavaScript](../ru/javascript.md) |
| [Settings and UI](settings-and-ui.md) | Sliders, checkboxes, your own window | [Настройки и интерфейс](../ru/settings-and-ui.md) |
| [Game API](game-api.md) | Players, weapons, enemies, camera, input, events, memory | [Игровой API](../ru/game-api.md) |
| [Hooks](hooks.md) | Run code in game functions | [Хуки](../ru/hooks.md) |
| [Native DLL](native-dll.md) | Mods in C or C++ | [Нативные DLL](../ru/native-dll.md) |
| [Testing and sharing](testing-and-sharing.md) | Test, debug, pack, publish | [Тестирование и публикация](../ru/testing-and-sharing.md) |

Suggested reading orders:

- **Beginner:** this page, then [Data patches](data-patches.md), then [Lua](lua.md), then [Game API](game-api.md), then [Testing and sharing](testing-and-sharing.md).
- **Artist:** [Textures](textures.md), then [Disc files](files.md), then [Testing and sharing](testing-and-sharing.md).
- **Musician or translator:** [Audio](audio.md) or [Text](text.md), then [Testing and sharing](testing-and-sharing.md).
- **Programmer:** [Lua](lua.md) or [JavaScript](javascript.md), [Game API](game-api.md), [Hooks](hooks.md), then [Native DLL](native-dll.md).

## Technical reference

These pages list every field and function. They are in English only.

| Document | What is in it |
|---|---|
| [Game API reference](../../api/README.md) | Every domain, member, argument and event, with status and examples |
| [mod.json reference](../../manifest.md) | Every manifest field |
| [Safety](../../safety.md) | What the port does and cannot do for safety |
| [API changelog](../../api-changelog.md) | What each API version added |
| [Example mods](../../../../mods-examples) | Complete, working mods to copy from |
| [uc.d.ts](../../uc.d.ts) | TypeScript declarations for editor completion |
| [lua/uc.lua](../../lua/uc.lua) | Lua declarations for lua-language-server |

Next: [Textures](textures.md) · [Data patches](data-patches.md) · [Lua](lua.md)

# Disc files

[Русский](../ru/files.md) · **English**

You are here: [Modding guide](README.md) › **Disc files**

How to replace or add any file of the game: 3D models, scene data, movies, fonts, archives, layouts. This is the most general kind of mod; textures, audio and text have friendlier guides of their own, but they build on the same idea.

## Contents

- [How the overlay works](#how-the-overlay-works)
- [Quick start](#quick-start)
- [What is in the game's folder](#what-is-in-the-games-folder)
- [Finding the file you need](#finding-the-file-you-need)
- [Rules](#rules)
- [Archives](#archives)
- [Movies](#movies)
- [When the game notices a change](#when-the-game-notices-a-change)
- [Conflicts between mods](#conflicts-between-mods)
- [Adding files](#adding-files)
- [Limits and risks](#limits-and-risks)
- [Troubleshooting](#troubleshooting)
- [See also](#see-also)

## How the overlay works

The game reads its files from the **disc layout**, the folder `game\DATA\files` of your installation. The port never changes those files. Instead, a mod's `files` folder is laid **over** the disc layout like a transparent sheet: when the game asks for a file, the port looks in the mods first. The game cannot tell the difference.

```
mods\my-mod\files\Data\movie\MV_000_0.sfd     is shown to the game as    Data\movie\MV_000_0.sfd
```

The path inside `files` is exactly the path inside the disc layout. Names are case-insensitive.

## Quick start

1. Find the file you want (see [Finding the file you need](#finding-the-file-you-need)), for example `Data\movie\MV_000_0.sfd`.
2. Make the same folders inside your mod: `mods\my-movie\files\Data\movie\`.
3. Put your file there under the same name.
4. Create `mods\my-movie\mod.json`:

   ```json
   { "id": "my-movie", "name": "My opening movie", "version": "1.0", "api": "1.0" }
   ```
5. Start the game, tick the mod in the **Mods** tab, and go where the file is used (see [When the game notices a change](#when-the-game-notices-a-change)).

## What is in the game's folder

`game\DATA\files\Data` is about 4 GB. The main folders:

| Folder | Contents |
|---|---|
| `bg` | Backgrounds and level geometry |
| `chara` | Characters and enemies (archives with models and textures) |
| `effect` | Effects |
| `event` | Event and cutscene data |
| `HomeButton2` | The Wii Home menu replacement |
| `layout` | Screen layouts of the menus and the HUD |
| `message` | Texts (`mes_<language>.txtres`) and subtitle tables (`movie_jimaku*.csvtbl`); see [Text](text.md) |
| `movie` | The 59 cutscene movies (`.sfd`) |
| `script` | Scene scripts and effect data (`.arc`, `.emt`) |
| `sky` | Skies |
| `sound` | Sound banks (`.brsar`) and the streams in `sound\stream` (`.brstm`); see [Audio](audio.md) |
| `sysres` | System resources: fonts (`font0..2\*.brfnt`), shared archives |

File types you will meet:

| Extension | What it is |
|---|---|
| `.arc` | An archive (the Wii U8 format) holding models, textures, scripts. |
| `.brres` | A model/texture/animation resource. |
| `.brstm`, `.brsar` | Streaming audio and sound banks. |
| `.sfd` | A movie (CRI Sofdec). |
| `.brfnt` | A font. |
| `.txtres`, `.csvtbl` | Texts and subtitle tables. |

The port ships `tools\arc_tool.bat` to look inside archives ([Archives](#archives)). For models and other formats use the general tools of the Wii modding community, and the matching guides of this folder for textures, audio and text.

## Finding the file you need

Start the game from a command prompt with the `--log-dvd` option:

```bat
uc.exe --log-dvd
```

The console then prints a line for every file the game **opens** and every **read** it makes:

```
[dvd] open Data/message/mes_us.txtres
[dvd] read Data/message/mes_us.txtres offset 0 length 257184
[dvd] open Data/sound/stream/00_Title.brstm
```

Play the part of the game you are interested in (a menu, a mission, a movie) and read the paths from the console. That tells you exactly which file is used where.

## Rules

- A file in `files` with the **same path** as a game file **replaces** it.
- A file whose path does not exist in the game is **added**; the game can open it by name ([Adding files](#adding-files)).
- Names are case-insensitive; use `/` or `\` freely when you make the folders.
- Symbolic links and junctions inside `files` are ignored.
- The file may have **any length**; the port serves it as it is.

## Archives

The game keeps many assets (models, layouts, scripts) inside `.arc` archives. You do not have to ship a whole archive to change one file in it: put the file in a folder named like the archive **with `!` added**, and the port rebuilds the archive for you.

```
mods\my-mod\files\Data\chara\EZ01.arc!\EZ01.brres        replaces the model inside Data\chara\EZ01.arc
mods\my-mod\files\Data\chara\EZ01.arc!\extra\note.txt     adds a file (and its folder) to the archive
```

- The path inside the archive is the path below `EZ01.arc!`. Use `tools\arc_tool.bat list <archive>` to see the paths, and `unpack` to get the original files.
- Mods apply in load order, and a mod that ships the whole archive replaces everything before it. Two mods changing one member: the later one wins.
- **Size matters.** The game loads the archive into a small pool of memory: an archive that grows by more than about 256 KB can stop the game (the Mods tab warns). Replacements of about the original size are safe.
- Not supported: an archive inside an archive.

Full rules, the measured size limits and the tool: [Archive members](../../archives.md).

## Movies

A movie is a whole file too: put it at `files\Data\movie\<name>.sfd`. The only extra program you need to make one is [ffmpeg](https://ffmpeg.org): `tools\sfd_tool.bat make my-video.mp4 MV_000_0.sfd` turns any video into a file the game plays. The game's own decoder plays your picture and sound, and its subtitles are drawn over it. Details, options and limits: [Replacing a movie](../../video.md).

## When the game notices a change

You can switch mods on and off while the game runs. The port rebuilds the overlay at once, but the **game uses it the next time it opens the file**:

- A file the game **already holds open or has loaded into memory** (a level, a sound that plays now) stays as it was until the game loads it again, for example in the next area.
- A file the game reads **once at start** (the text files) needs a restart.
- If a file is replaced while the game reads it, the game keeps the version it opened. So a stream that plays when you switch a mod on or off does not break.

Rule of thumb for testing: after changing a mod, **restart the game** unless you are sure the game reloads the file in the scene you test.

## Conflicts between mods

If two active mods have the same path, **the lower mod in the Mods tab list wins**. The Mods tab lists every path that two or more mods provide, and the game's console prints `[mods] conflict on ...` lines. Reorder the mods with the arrows to choose the winner.

## Adding files

A new file does nothing by itself: the game opens only the files its code knows. Adding files is useful when another part of your mod uses them: a script, a hook, or a replaced file that refers to an extra file by name.

## Limits and risks

- A replacement must be **valid for the game**: same format, and the structure the game expects. The port cannot check that. A wrong model or archive can crash the game or show garbage.
- Replacing a file used at **every start** (system archives, fonts) breaks the game for everyone who uses the mod; test with care and say what you changed in the description.
- Movies are decoded by the game's own code: an `.sfd` movie must be a valid Sofdec file of a size and shape that the game's player accepts.
- The game's files are never changed. Switching the mod off, or deleting it, restores everything.

## Troubleshooting

| Problem | Likely cause and fix |
|---|---|
| The game ignores my file | The path differs from the game's (check with `--log-dvd`), or the game loaded the original before the mod was on: restart. |
| The game crashes after I replaced a file | The file is not valid for the game. Remove it and try again with a file in the original format and structure. |
| Two mods fight | Look at the conflict lines and reorder them in the Mods tab. |
| The mod seems to do nothing, but the file is there | Is the mod ticked? Is there a `mod.json`? The folder must be `mods\<name>\files\...` with the game's own folder names under `files`. |

## See also

- [Textures](textures.md), [Audio](audio.md), [Text](text.md) - friendlier ways to change those parts.
- [Testing and sharing](testing-and-sharing.md) - testing, packing and publishing.
- [How several mods work together](README.md#how-several-mods-work-together)

Previous: [Text](text.md) · Next: [Data patches](data-patches.md)

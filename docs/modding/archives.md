# Archive members

A mod can replace or add **one file inside a game archive** (`.arc`, the Wii U8 format) without shipping the whole archive. Put the file under the archive's path with `!` added to its name:

```
mods\my-mod\files\Data\chara\EZ01.arc!\EZ01.brres
mods\my-mod\files\Data\layout\game_us.arc!\lyt_data\timg\2SHT_B.tpl
```

The folder `EZ01.arc!` stands for the archive `Data\chara\EZ01.arc` of the disc; everything below it is a path inside the archive. The port rebuilds the archive when the mod is switched on, keeps the result in `mods\.cache\_arc\` and serves it to the game instead of the original. The disc files are never changed. Switching the mod off brings the original back (a file the game already holds open keeps its version until the game opens it again).

## Rules

- The archive path is relative to `DATA\files` like any file of the mod; the member path is the path inside the archive. Names are matched case-insensitively. A path that does not exist in the archive is **added**, with its folders.
- Load order decides: the base is the disc's archive; a mod that ships the whole archive (`files\Data\chara\EZ01.arc`) replaces everything before it; the members of a mod apply on top of that. Two mods setting one member: the later one wins, and the console prints `archive member ... is set by A and B`.
- Not supported: an archive inside an archive (`a.arc!\b.arc!\x`), compressed archives (the game's own are not), members larger than 64 MiB, archives (the result) larger than 256 MiB.
- A damaged disc archive (a member past the end of the file, bad names) is refused with a message in the Mods tab, and the game keeps the original. If the damaged one is a **mod's own whole archive**, the game is served that file as it is; members of later mods cannot be applied to it (they say so), unless a still later mod ships a good whole archive.
- Problems show under the mod in the Mods tab as `arc: <archive>!/<member>: <what>`; the rest of the mod still applies.

## The size limit

The game loads an archive whole into memory it takes from its own heap, and that heap has little room to spare. Measured on the train chapter:

| Archive | Safe growth | Game stops at |
|---|---|---|
| `layout\*.arc`, `sysres_g.arc`, `script\*.arc` (always loaded) | about 250 KB | 500 KB to 1 MB |
| `chara\*.arc`, `bg\*.arc` (stage data) | about 1 MB | 2 MB |

So a replacement of about the **same size** is always safe, and an archive that grows more than 256 KB gets the warning `N KiB bigger than the original; the game may run out of memory loading it` in the Mods tab (it is still applied; "the original" is the archive the members were applied to: the disc's, or a mod's whole archive). When the game runs out of memory it closes with the message `DVDReadPrio into an invalid guest buffer`. Smaller is always fine: for a bigger texture, lower its resolution or use a more compact format.

## Looking inside an archive: `arc_tool`

`tools\arc_tool.bat` (Python 3) helps you find member paths and make a ready archive:

```
tools\arc_tool.bat list   "game\DATA\files\Data\chara\EZ01.arc"
tools\arc_tool.bat unpack "game\DATA\files\Data\chara\EZ01.arc" EZ01_unpacked
tools\arc_tool.bat pack   EZ01_unpacked EZ01_new.arc
```

`unpack` writes every member to a folder (the folder must be empty or new), `pack` builds an archive from a folder. You rarely need `pack`: put the changed member in `files\...\EZ01.arc!\` instead and let the port do it.

## See also

- [Files](guide/en/files.md), [Testing and sharing](guide/en/testing-and-sharing.md)
- [Files and textures](files-and-textures.md)

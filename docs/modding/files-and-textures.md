# Replacing files and textures

## Disc files

Everything under `mods/<id>/files/` is laid over the game's disc files. The folder structure is the disc's (`DATA/files/...` content, so `files/sound/bgm.brstm` replaces `sound/bgm.brstm` on the disc). Names are case-insensitive.

- A file with the same path as a disc file replaces it; the game cannot tell the difference.
- A file that does not exist on the disc is added; the game can open it by name.
- Later mods in the load order win over earlier ones. The Mods tab lists every path that two or more enabled mods provide.
- Symbolic links and junctions inside `files/` are ignored.
- Changes take effect when the game starts (enabling, disabling and reordering need a restart; the tab says so).

**Archives:** the overlay works on whole files. If the game keeps its assets inside packed archives that it opens as one file and reads itself, such an archive can only be replaced as a whole file; editing a single asset inside it is not supported by the overlay.

## Textures

Put replacement textures in `mods/<id>/textures/`, named like the dumps of the texture extractor (`tex1_<width>x<height>_<hash>_<format>.dds`, BC7 with a full mip chain, see [texture-extraction.md](../texture-extraction.md)). The base `textures/` folder next to the exe still works. When two folders hold the same texture, the later mod wins over the earlier one and over the base folder.

## Data patches

Changes to values (weapon damage and the like) need no files: see [data-patches.md](data-patches.md).

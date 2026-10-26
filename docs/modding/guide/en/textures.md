# Textures

[Русский](../ru/textures.md) · **English**

You are here: [Modding guide](README.md) › **Textures**

How to replace the pictures the game draws: characters, rooms, weapons, menus, fonts and effects, for example with an HD texture pack.

## Contents

- [How replacement works](#how-replacement-works)
- [What you need](#what-you-need)
- [Step 1 - extract the original textures](#step-1---extract-the-original-textures)
- [Step 2 - edit or upscale the pictures](#step-2---edit-or-upscale-the-pictures)
- [Step 3 - convert to DDS](#step-3---convert-to-dds)
- [Step 4 - put the textures into a mod](#step-4---put-the-textures-into-a-mod)
- [Step 5 - test it](#step-5---test-it)
- [Anatomy of a texture file name](#anatomy-of-a-texture-file-name)
- [Several mods and the base folder](#several-mods-and-the-base-folder)
- [Limits](#limits)
- [Troubleshooting](#troubleshooting)
- [Tips for big packs](#tips-for-big-packs)
- [See also](#see-also)

## How replacement works

The game stores its pictures in the formats of the Wii (TPL, BRRES, font sheets, effect tables). When the game decodes a texture for the first time, the port computes the texture's **name** from its pixels, in the same way the Dolphin emulator does: `tex1_<width>x<height>_<hash>_<format>`. If a file `tex1_..._.dds` with that name exists in a `textures` folder, the port uses its pixels instead. All other textures stay as they are.

So a texture mod is just a folder of `.dds` files with the right names. No script, no code, and the original files are never changed.

You get the right names by **extracting** the original textures: the extractor saves every picture as a PNG whose file name is already the name the game will look for.

## What you need

- The game folder with `tools\extract_textures.bat` (needs Python 3.10 or newer; no extra Python packages) and `tools\uc_texture_convert.exe`. Both are installed next to the game.
- A program to edit or upscale PNG pictures. Any will do: an image editor, or an AI upscaler such as an ESRGAN-based tool. The port does not care how the picture was made.
- Disk space: the full dump is several gigabytes of PNG files, and a full HD pack can be large too.

## Step 1 - extract the original textures

Run in a command prompt in the game folder:

```bat
tools\extract_textures.bat --input "game\DATA" --output "<work>\dump"
```

- `--input` is the game's data folder (it also accepts one archive or container file).
- `--output` is where the PNG files go. Use a folder of your own, outside the game folder.

The extractor reads every archive of the game (U8, TPL with palettes, BRRES, fonts, effects, banners), and writes:

- one **PNG per unique picture**, named like `tex1_256x256_0123456789abcdef_14.png`. The file name is what the game looks up later: **never rename these files**;
- `extractor_manifest.json`: which game file each picture came from, plus the alternative names under which one picture may be needed.

Useful variants:

```bat
tools\extract_textures.bat --full
```

`--full` also saves the original mip levels (`_mip1`, `_mip2`, ...). You rarely need this; the converter builds new mip levels from your picture.

Running the extractor again is safe: files with the same contents are kept, and a changed PNG is never overwritten (the tool reports a conflict instead).

## Step 2 - edit or upscale the pictures

Edit the PNG files with any program. Rules that keep the replacement working:

1. **Keep the file name exactly.** Copy edited files under the same name into a separate folder (for example `<work>\upscaled`). You may leave out every picture you do not change.
2. **Keep the shape.** The new picture must have the same width-to-height ratio as the original. Doubling or quadrupling the size is typical for an HD pack (256x256 becomes 512x512 or 1024x1024).
3. **Keep the alpha channel.** Many game textures have transparency (leaves, hair, glass, UI). Save PNG with alpha, 8 bits per channel (RGBA).
4. **Do not change the picture's meaning.** The same pixel must stay in the same place: textures are mapped to models by position.

Special textures need care:

- **Font and UI sheets** are pictures with letters or icons in a grid; keep the grid layout.
- **Normal-like or distortion maps** (used by some water and heat effects) are read as data. Upscaling them smoothly is fine, but do not recolour them.
- Pictures with a **palette** are extracted as ordinary RGBA; the palette is applied again by name, nothing for you to do.

## Step 3 - convert to DDS

The game loads **BC7-compressed DDS files with a full mip chain**. The shipped converter makes them from your PNG files:

```bat
tools\uc_texture_convert.exe --input "<work>\upscaled" --output "mods\my-textures\textures" --manifest "<work>\dump\extractor_manifest.json"
```

| Option | Meaning |
|---|---|
| `--input` | The folder with your PNG files. |
| `--output` | Where the `.dds` files go. Point it at the `textures` folder inside your mod. |
| `--manifest` | The `extractor_manifest.json` from Step 1. It tells the converter which extra names one picture needs; those get hard-linked copies, so they cost no extra space. |
| `--threads N` | How many threads to use (default: all). |
| `--force` | Convert again even when the DDS is newer than the PNG. |

The converter uses all processor threads (about 30 seconds for 8000 pictures) and skips files whose DDS is already newer than the PNG, so you can run it again after changing a few pictures.

## Step 4 - put the textures into a mod

A texture mod needs nothing but a `mod.json` and the `textures` folder:

```
mods\my-textures\
  mod.json
  textures\
    tex1_256x256_0123456789abcdef_14.dds
    tex1_512x256_fedcba9876543210_6.dds
    ...
```

```json
{
  "id": "my-textures",
  "name": "My HD textures",
  "version": "1.0",
  "api": "1.0",
  "author": "you",
  "description": "Sharper characters and rooms."
}
```

Cosmetic mods leave `touches_gameplay` out (it is `false` by default). If you also give the mod a script, a data patch or other content, the other guides tell you what to add.

## Step 5 - test it

1. Start the game, press **Insert**, open the **Mods** tab and tick your mod.
2. Go to a place where one of the textures is visible. A texture changes **the next time the game decodes it**: textures already in use may need a scene change or a restart of the game to be reloaded.
3. If nothing changes, see [Troubleshooting](#troubleshooting).

You can test with a single file first: convert one picture, put it in the `textures` folder, restart, and look for it.

## Anatomy of a texture file name

```
tex1_256x256_0123456789abcdef_14.dds
 |    |       |                |
 |    |       |                +-- the GX pixel format number
 |    |       +------------------- a 64-bit hash of the original pixel data (XXH64)
 |    +--------------------------- width x height of the ORIGINAL texture
 +-------------------------------- the prefix of this kind of name (the Dolphin emulator's naming)
```

- Some names carry one more part, the hash of the palette: `tex1_64x64_<hash>_<palette hash>_9`.
- A name with `$` in place of the palette hash is a wildcard for a palette that cannot be known in advance.
- The size in the name is the **original** size. Your picture may be bigger.
- The Dolphin variant of a name with `_m` (mipmapped) is the same texture: the port ignores the `_m`.

## Several mods and the base folder

The game looks for a texture in this order: the mods' `textures` folders from the lowest mod in the list to the highest (the lower one wins), then the base `textures` folder next to `uc.exe`. If two mods contain the same file, the mod lower in the **Mods** tab list wins ([how mods combine](README.md#how-several-mods-work-together)).

The base `textures` folder works without any mod; it is where a pack from before the mod system lives.

## Limits

- **Textures made while the game runs** are not replaced: copies of the screen (EFB copies), dynamic effects, and the video frames of cutscenes.
- **Indirect textures** (some water and heat-haze effects) use the size of the map; an upscaled distortion map can shift the effect slightly.
- A **broken DDS** (wrong format, truncated file) is skipped. The game prints a message to its console output and keeps the original texture.
- Everything is **BC7**. Other DDS formats are not read.
- **Video memory:** a big pack makes the game use more video memory. The port keeps a cache and raises its budget when replacements exist; a very large pack on a small graphics card can hurt performance.
- Replacements follow the game version: the names depend on the original pixels, so a pack made for the US release (`RBUE08`) works with that release.

## Troubleshooting

| Problem | Likely cause and fix |
|---|---|
| Nothing changes | The file name differs from the extracted one (renamed, `.png` left in, extra characters). Compare it with the dump. |
| Nothing changes, name is right | The texture was loaded before the mod was ticked. Change the scene or restart the game. |
| The mod shows in the Mods tab but the textures do not | The files are not directly in `textures\` (check there is no extra subfolder), or the mod is ticked but a lower mod wins. |
| The texture shows but looks stretched or wrong | The aspect ratio differs from the original, or the alpha channel was lost. |
| The game prints "corrupt DDS" or similar | The DDS is not BC7 or is cut off. Convert it again with `uc_texture_convert`. |
| Everything shows some wrong texture | A conversion with the wrong `--input`: check the PNG folder holds the pictures you meant. |
| The game stutters after loading a big pack | Too much video memory. Cut the pack down, or keep the biggest textures at 2x instead of 4x. |

## Tips for big packs

- Work in a separate folder and keep only the pictures you really changed; the base game is used for the rest.
- Make the pack in several mods (characters, rooms, UI) so players can combine them.
- Run the converter on the whole folder once, then only on changed files (it skips what is up to date).
- Put a short `description` and a preview picture in the download page, not in the mod folder.
- Zip the mod folder ([Testing and sharing](testing-and-sharing.md#packing-a-mod)). DDS files do not compress much, so expect a big zip.
- You own your edits; the original pictures belong to the game's authors. Do not publish unmodified extracted pictures.

## See also

- [Disc files](files.md) - when you need to replace something that is not a texture.
- [Testing and sharing](testing-and-sharing.md) - checking, packing and publishing.
- [How several mods work together](README.md#how-several-mods-work-together)
- [mod.json reference](../../manifest.md)

Previous: [Overview](README.md) · Next: [Audio](audio.md)

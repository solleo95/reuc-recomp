# mod.json reference

Every mod is a folder (or a `.zip` of that folder) inside `mods/` next to `uc.exe`. The folder holds a `mod.json` and whatever the mod uses:

```
mods/my-mod/
  mod.json
  files/        replacements and additions for the game's disc files (see files-and-textures.md)
  textures/     replacement textures
  data/         data patches, one JSON file each (see data-patches.md)
  scripts/      Lua or JavaScript
  native/       a DLL (see native-dll.md)
```

A zip may hold the files directly or inside one top-level folder. It is unpacked once into `mods/.cache/`.

## Fields

```json
{
  "id": "infinite-ammo",
  "name": "Infinite Ammo",
  "version": "1.2.0",
  "author": "someone",
  "description": "Never reload.",
  "api": "1.0",
  "game": ["RBUE08"],
  "depends": [{ "id": "core-lib", "min_version": "1.0" }],
  "touches_gameplay": true,
  "script": "scripts/main.lua",
  "native": "native/mod.dll",
  "names": { "ru": "Бесконечные патроны" },
  "settings": [ ... ]
}
```

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | Unique, lower-case letters, digits, `_` and `-`, 1 to 48 characters, starting with a letter or digit. `enabled`, `order`, `allow_native` and `native_hash` are reserved (they are keys of the config's `mods` section). |
| `name`, `version` | yes | Shown in the Mods tab. Versions are dotted numbers (`1.10` is newer than `1.2`). |
| `api` | yes | The mod API version the mod was written for, `"major.minor"`. A game with the same major version and an equal or newer minor version can run it. |
| `author`, `description` | no | Shown in the Mods tab. |
| `game` | no | Game ids the mod supports (`RBUE08` is the US release). Empty or missing: any. |
| `depends` | no | Other mods that must be enabled and working. `min_version` is optional. Mods load after the mods they depend on. |
| `touches_gameplay` | no | `true` when the mod changes how the game plays. Cosmetic mods leave it `false`. The flag has no effect on saves (the game always uses its normal saves). It is enforced: without it the game API's setters (`uc.player(1).magazine = 5`), `mem.write_*`, data patches and hooks are refused with an error that names the flag. Reading the game is always allowed. |
| `script` | no | Entry file inside the mod folder, `.lua` for Lua, `.js` or `.mjs` for JavaScript. |
| `native` | no | A DLL inside the mod folder. It needs the "Allow native mods" switch and a confirmation (see safety.md). |
| `names` | no | Display names per language code. |
| `audio` | no | Loop points for replaced music streams: `{ "00_Title": { "loop_start": 1000, "loop_end": 900000 } }`, in samples of the mod's own file (end exclusive). Without an entry the loop comes from the file itself, or there is none. See audio.md. |
| `settings` | no | The mod's settings, drawn in its settings window (gear button in the Mods tab of the Insert menu) (see settings-and-ui.md). |

Paths in a manifest are relative to the mod folder, use `/`, and may not contain `..`, a drive letter or a leading `/`.

A broken `mod.json` never stops other mods: the broken mod appears in the Mods tab as an error with the reason.

## Load order

The Mods tab lets the player enable, disable and reorder mods. The order decides who wins when two mods provide the same file or texture (the later one wins). Dependencies are always loaded first. The choice is saved in `config.json`, section `mods`.

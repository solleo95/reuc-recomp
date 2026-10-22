# Making a mod

A mod is a folder in `mods/` next to `uc.exe`. The smallest useful one is two files.

`mods/infinite-ammo/mod.json`

```json
{ "id": "infinite-ammo", "name": "Infinite Ammo", "version": "1.0", "api": "1.0", "touches_gameplay": true, "script": "scripts/main.lua" }
```

`mods/infinite-ammo/scripts/main.lua`

```lua
uc.player(1).infinite_ammo = true
```

Start the game, open the menu with **Insert**, go to the **Mods** tab and tick the mod; restart the game. (Enabling mods is a restart-time choice; scripts can be reloaded from the same tab.) The log is `debug/mods.log`.

## What a mod can contain

| You want to | Use | Read |
|---|---|---|
| change numbers (damage, magazine size, reload time) | a data patch, no code | [data-patches.md](data-patches.md) |
| react to the game, change the player, draw a menu | a Lua or JavaScript script | [lua.md](lua.md), [javascript.md](javascript.md) |
| show sliders and checkboxes the player can change | declared settings in `mod.json` | [settings-and-ui.md](settings-and-ui.md) |
| replace a disc file or a texture | files in `files/`, `textures/` | [files-and-textures.md](files-and-textures.md) |
| something scripts are too slow or too limited for | a native DLL | [native-dll.md](native-dll.md) |
| hook into a game function | `hook.pre` | [hooks.md](hooks.md) |

Every field of `mod.json` is described in [manifest.md](manifest.md). The game API (everything a script can read and change) is in [api/README.md](api/README.md); each member says whether it is `implemented`, `unverified` or only `planned`. If you write TypeScript or want editor completion, [`uc.d.ts`](uc.d.ts) (JavaScript) and [`lua/uc.lua`](lua/uc.lua) (lua-language-server) describe the same API.

The examples in [`mods-examples/`](../../mods-examples) are complete mods; [recipes.md](recipes.md) walks through each.

## Good to know before you ship

- Mods run in a sandbox (scripts) or with full rights (DLLs): read [safety.md](safety.md).
- Mods that change gameplay use a separate save folder: [saves.md](saves.md).
- State the API version you wrote the mod for (`"api": "1.0"`). Minor versions only add; a major version change may break mods. [api-changelog.md](api-changelog.md) lists changes.
- The game runs at its own step rate; do per-step work in `on_step`, not by counting frames yourself.

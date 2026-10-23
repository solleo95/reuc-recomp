# Recipes: the example mods

The folder [`mods-examples/`](../../mods-examples) holds complete mods. Copy one into `mods/` to try it, or read it as a starting point. Each is exercised by an automated test against the real runtimes (`tests/test_mod_examples.cpp`).

## Infinite ammo (Lua) — `infinite_ammo`

Two declared settings (player 1, player 2) and five lines of script:

```lua
local function apply()
  uc.player(1).infinite_ammo = settings.get('player1')
  uc.player(2).infinite_ammo = settings.get('player2')
end
apply()
on('on_settings_changed', apply)
```

`infinite_ammo` is a switch of the game API: while it is on, the API refills the magazine at every game step to the largest magazine it has seen for the equipped weapon. Shows: declared settings, `on_settings_changed`, a gameplay mod (`touches_gameplay`).

## God mode (JavaScript) — `god_mode`

Invulnerability per player through `uc.player(n).invulnerable`, and a free-drawing settings window: the damage meter of player 1 and a **Heal now** button (`ui.draw`). Shows: an ES-module entry file, settings plus free drawing in one window, checking `present` before reading a player.

## Inspector (JavaScript) — `inspector`

A read-only window onto everything the API exposes, for mod authors: open its window with the gear button in the Mods tab. Always shown: both players (health, weapon, magazine, reserve, grenades and the result counters). Sections: **Mission and camera** (mission clock, pause, stars, camera position in meters and direction), **Pad** (held buttons, fire, aim), **Enemies** (the zombies nearest to the camera, with hit points and distance), **Events** (a log of the game events, newest first, with the mission time), **Memory watch** (four rows: type an address in decimal or `0x` hex, or a symbol name, pick `u8`, `u16`, `u32`, `s32` or `f32`, and see the live value) and **Snapshot** (one line of JSON in `debug\mods.log`: players, mission, camera and enemies, for bug reports). Settings: how many enemies are listed (1 to 30), how many events are kept (5 to 100) and `log_every`, a snapshot every N game seconds (0 = never; the game keeps only the first 2000 log lines of a mod per session, so a snapshot every second lasts about half an hour; it waits while the pause menu is open). It never writes to the game and needs no `touches_gameplay`. Outside a mission it says `not in a mission` and no part throws. Shows: `ui.draw` with collapsible sections, reading every part of the API defensively (a missing scene is `null`, not an error), ES-module helpers in `scripts/lib/inspect.js`, and the snapshot line as the way to hand data to somebody else.

## Randomizer (JavaScript) — `randomizer`

Shuffles the game from a seed. **Weapons:** damage, fire rate, magazine size and reload time of every gun get a random factor (a record without a magazine is left alone). **Zombies:** every zombie's hit points get a factor, once, when it is first seen. **Start weapon** (off by default): each player who is in a mission that has just begun gets a random gun three game seconds in (a player who joins within 15 seconds gets one then; the reserve is infinite like the handgun's). The same seed always gives the same game, so a seed can be shared. Settings: `seed` (1 to 999999), `strength` in percent (0 changes nothing; 50 changes values up to half or double, 100 up to a third or triple), and a switch each for weapons, zombies and the start weapon. The gear window has a **New seed** button and lists the changes (the weapon table, the latest zombies, the start weapons). Every plan is made from the game's own weapon values as they were the first time the table was filled, so changing the seed or the strength never compounds, and switching weapons off puts those values back; a value the mod never changed (strength 0, or a field whose factor is exactly 1) is not touched, and one it changed is kept by the API for the whole session, replacing anything the game writes there later (for example upgrades). Limit: after a start weapon the HUD may still show the pistol's picture and name; the weapon, its magazine and its shots are the new weapon's. Shows: seeded random draws that do not depend on event order, reading the game's values once and planning from them, `on_enemy_spawned` plus a periodic scan for zombies that were there before the mod saw the scene, `give_weapon` and `select_weapon`.

## Pistol auto-fire (Lua) — `pistol_autofire`

```lua
for _, id in ipairs({ 0, 19 }) do uc.weapon(id).fire_mode = 'auto' end
```

Weapon records 0 and 19 are the handgun. With `fire_mode = 'auto'` a held trigger keeps firing (checked in the game: about four shots in 90 frames instead of one). How fast it fires is set by `weapon.fire_rate` (the handgun is 1 shot per second at rest; a rate of 2 gives about twice the shots).

## Damage multiplier (Lua) — `damage_multiplier`

One number setting. The script waits until the game has filled its weapon table (the first step where `uc.weapon(0).damage > 0`), remembers every weapon's original damage, and writes `original * scale`. Changing the setting in the menu recomputes from the originals. Shows: why reading game data belongs in `on_step` rather than `on_load`, and that weapon values stay set when the game refills its tables.

## Faster handgun (data only) — `faster_handgun`

No script: `data/handgun.json` sets a 30-round magazine and a 0.3 s reload for weapon records 0 and 19 (see [data-patches.md](data-patches.md)).

## Hello native (C++ DLL) — `hello_native`

A DLL with its own ImGui settings window. Built by the project's CMake (`mods-examples/hello_native/CMakeLists.txt`) into `dist/mods-examples/hello_native/native/hello_native.dll`. Enable "Allow native mods" in the Mods tab, enable the mod, confirm its native code. It starts at once. See [native-dll.md](native-dll.md).

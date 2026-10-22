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

## Pistol auto-fire (Lua) — `pistol_autofire`

```lua
for _, id in ipairs({ 0, 19 }) do uc.weapon(id).fire_mode = 'auto' end
```

Weapon records 0 and 19 are the handgun. With `fire_mode = 'auto'` a held trigger keeps firing (checked in the game: about four shots in 90 frames instead of one). How fast it fires is fixed by the weapon's animation; `weapon.fire_rate` is not available yet.

## Damage multiplier (Lua) — `damage_multiplier`

One number setting. The script waits until the game has filled its weapon table (the first step where `uc.weapon(0).damage > 0`), remembers every weapon's original damage, and writes `original * scale`. Changing the setting in the menu recomputes from the originals. Shows: why reading game data belongs in `on_step` rather than `on_load`, and that weapon values stay set when the game refills its tables. `weapon.damage` is `unverified` in the API reference.

## Faster handgun (data only) — `faster_handgun`

No script: `data/handgun.json` sets a 30-round magazine and a 0.3 s reload for weapon records 0 and 19 (see [data-patches.md](data-patches.md)).

## Hello native (C++ DLL) — `hello_native`

A DLL with its own ImGui settings window. Built by the project's CMake (`mods-examples/hello_native/CMakeLists.txt`) into `dist/mods-examples/hello_native/native/hello_native.dll`. Enable "Allow native mods" in the Mods tab, enable the mod, confirm its native code. It starts at once. See [native-dll.md](native-dll.md).

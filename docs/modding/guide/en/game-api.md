# Game API

[Русский](../ru/game-api.md) · **English**

You are here: [Modding guide](README.md) › **Game API**

A tour of everything a mod can read and change in the game: players, weapons, enemies, mission state, the camera, controls, events, memory and calls into the game's own code. Examples are in Lua; JavaScript is identical apart from syntax ([JavaScript guide](javascript.md#lua-and-javascript-side-by-side)). The exact list of every member, with its status, is the [API reference](../../api/README.md); this guide explains how to use them.

## Contents

- [The basics](#the-basics)
- [Players](#players)
- [Weapons](#weapons)
- [Enemies](#enemies)
- [Mission state](#mission-state)
- [Weapons in the player's hands](#weapons-in-the-players-hands)
- [Camera](#camera)
- [Input](#input)
- [Events](#events)
- [Memory](#memory)
- [Symbols and calling game functions](#symbols-and-calling-game-functions)
- [Recipes](#recipes)
- [Good manners](#good-manners)
- [API versions](#api-versions)
- [See also](#see-also)

## The basics

Everything lives under the global **`uc`**:

| Domain | What |
|---|---|
| `uc.player(slot)` | A player: magazine, damage, health, switches (slot 1 or 2) |
| `uc.weapon(id)` | A weapon type: damage, rate, magazine size (id 0 to 19) |
| `uc.enemy(id)`, `uc.enemies` | Zombies: health, position; the list of all |
| `uc.game` | Mission clock, pause, stars, giving weapons |
| `uc.camera` | Position and direction of the view |
| `uc.input` | The buttons and the pointer, as the game sees them |
| `uc.mem` | Raw game memory |
| `uc.symbols`, `uc.guest` | Names of game functions; calling them |

Besides `uc` there are the per-mod globals `on` (events), `hook` ([Hooks](hooks.md)), `settings` and `ui` ([Settings and UI](settings-and-ui.md)) and `log`.

Three rules apply everywhere:

1. **Handles.** `uc.player(1)`, `uc.weapon(0)` and `uc.enemy(10)` give a **handle**. Read and write properties on it: `uc.player(1).magazine = 15`.
2. **Writes need `touches_gameplay`.** A mod that changes the game must say so in `mod.json`; reading is always allowed.
3. **Statuses.** The reference marks every member `implemented`, `unverified` (works but was not confirmed in every situation) or `planned` (not available; using it is an error).

Errors from the API (a missing player, an unknown id, an address outside memory) are normal script errors; catch them with `pcall` (Lua) or `try/catch` (JavaScript) when you expect them.

## Players

`uc.player(1)` is player 1 and `uc.player(2)` is player 2 in co-op. **Always check `present`**: a player does not exist in menus or between missions.

| Member | Meaning |
|---|---|
| `present` | Is the player in the current scene (read only) |
| `magazine` | Rounds in the loaded magazine of the current weapon |
| `grenades` | Grenades in stock (one stock for both players) |
| `damage` | The damage meter: 0 is unhurt, 50 is dead; writing 0 heals |
| `health` | The same as a fraction: 1 is unhurt, 0 is dead (`health = 1 - damage / 50`) |
| `invulnerable` | While true the damage meter is reset every step |
| `infinite_ammo` | While true the magazine is refilled every step |
| `weapon` | The weapon id the player holds (read only) |
| `reserve` | Spare ammo of the held weapon; `nil` for the handgun, which has an infinite reserve (setting it is an error) |
| `hits`, `critical_hits`, `objects_destroyed` | The counters of the result screen for this mission |

```lua
local p = uc.player(1)
if p.present then
  p.health = 1                 -- heal fully
  p.grenades = 9
  log.write('holding weapon ' .. p.weapon .. ', spare ' .. tostring(p.reserve))
end
```

`invulnerable` and `infinite_ammo` are **switches**: you turn them on, and the game keeps doing the work every step. They go off by themselves when your mod stops.

## Weapons

`uc.weapon(id)` addresses one of the 20 weapon **records** (id 0 to 19): the handgun is 0 and 19, the shotgun is 1, the submachine gun is 2; the [data patches guide](data-patches.md#weapon-table) lists all values. A change applies to every weapon of that type from its next shot or reload.

| Member | Meaning |
|---|---|
| `damage` | Damage per hit (per pellet for shotguns), 0 to 10000 |
| `fire_rate` | Shots per second, 0.05 to 60 |
| `capacity` | Magazine size, 0 to 999 (takes effect at the next reload) |
| `reload_time` | Seconds to reload, 0 to 1000 |
| `fire_mode` | `'semi'`, `'auto'` or `'burst'` |

```lua
local w = uc.weapon(0)
w.fire_mode = 'auto'      -- a held trigger keeps firing
w.fire_rate = 3
w.damage = w.damage * 2
```

The game fills its weapon table at the start and at every mission start; the port **writes your values again whenever the game's own differ**, for the whole session. Read the original values in `on_step` (not at the top of the script) and keep a copy.

## Enemies

Only **zombies** are supported at present. Get the list, then address single enemies by `id`:

```lua
for _, e in ipairs(uc.enemies.list()) do
  -- e = { id, class, x, y, z, hp }
  if e.hp > 0 and e.x * e.x + e.z * e.z < 40 * 40 then     -- within 4 m of the origin (units are 10 cm)
    uc.enemy(e.id).hp = 0                                  -- kill it
  end
end
```

| Member of `uc.enemy(id)` | Meaning |
|---|---|
| `hp` | Health; zombies start with 10 to 30; 0 or less is dead (the actor stays until the wave ends) |
| `alive` | `hp > 0` (read only) |
| `head` | Is the head still on (read only; it comes off with a head shot) |
| `position` | `{x, y, z}` in units of 10 cm, y up; writing moves the zombie at once |

An id is valid **until the zombie leaves the scene**; another id or an enemy of another kind is an error. To react to enemies appearing and dying, use the [events](#events) instead of polling the list.

## Mission state

| Member | Meaning |
|---|---|
| `uc.game.time` | The mission clock in whole seconds (read or write) |
| `uc.game.paused` | True while the game's pause menu is open (read only) |
| `uc.game.stars` | The upgrade money, 0 to 9999 (the game keeps it in its save data) |

The clock and counters keep their last values outside a mission. `on_step` does not run while the game is paused.

## Weapons in the player's hands

Give a weapon with `uc.game.give_weapon`, then make it the held one with `uc.game.select_weapon`:

```lua
local index = uc.game.give_weapon(1, 2)        -- player 1, weapon record 2 (submachine gun); returns the loadout slot
uc.game.select_weapon(1, index)                -- the game's own weapon switch runs: object, magazine and HUD follow
```

`give_weapon(slot, id, key, magazine)`: `key` is the ammo key (0 to 20, default 0 = infinite reserve like the handgun, 7 uses the spare ammo of key 7) and `magazine` the rounds (default: the weapon's capacity). The loadout has 16 slots; a weapon the player already has returns its slot. `select_weapon` works only from the game's own events (`on_step`, the other events, hook handlers), **not** from the settings window.

## Camera

The game is on rails: both players share one viewpoint, the camera.

| Member | Meaning |
|---|---|
| `uc.camera.position` | `{x, y, z}` of the eye, units of 10 cm (about 15 high) |
| `uc.camera.forward`, `right`, `up` | Unit vectors of the view |
| `uc.camera.view` | The world-to-view matrix as 12 numbers |

They throw when there is no scene. A direction test, "is this enemy in front of me":

```lua
local cam = uc.camera.position
local f = uc.camera.forward
local e = uc.enemies.list()[1]
if e then
  local dx, dy, dz = e.x - cam[1], e.y - cam[2], e.z - cam[3]
  local dist = math.sqrt(dx*dx + dy*dy + dz*dz)
  local ahead = (dx*f[1] + dy*f[2] + dz*f[3]) / dist     -- 1 straight ahead, 0 to the side
  log.write(string.format('distance %.1f, ahead %.2f', dist, ahead))
end
```

## Input

What the game sees on **player 1's** pad.

| Member | Meaning |
|---|---|
| `uc.input.held` | The buttons held now, after the mods: a mask of A `0x800`, B `0x400` (the trigger), minus `0x10`, plus `0x1000` |
| `uc.input.fire_held` | The trigger is held |
| `uc.input.aim` | The pointer `{x, y}`, each -1 to 1 (0, 0 is the middle, y down) |
| `uc.input.force_held` | Buttons the game sees as held although the player does not hold them (a mask; 0 clears) |
| `uc.input.block_held` | Buttons that never reach the game (a mask; 0 clears) |
| `uc.input.aim_override` | Makes the game see the pointer at `{x, y}`; `nil` gives it back |

```lua
uc.input.force_held = 0x400          -- the trigger is held for ever: automatic fire without the player pressing
-- ...
uc.input.force_held = 0              -- let go
```

Masks of several mods add up; yours is taken back when your mod stops. A write needs `touches_gameplay`.

## Events

Handlers registered with `on(name, fn)` run on the game thread. The payloads:

| Event | Payload | When |
|---|---|---|
| `on_load` | none | The mod started |
| `on_unload` | none | The mod is stopping |
| `on_step` | none | About 30 times per game second; not while paused |
| `on_settings_changed` | `{ key }` | A declared setting changed |
| `on_weapon_fired` | `{ slot }` | A player fired |
| `on_reload` | `{ slot }` | A player started reloading |
| `on_damage_taken` | `{ slot, amount }` | A player's damage meter rose |
| `on_player_died` | `{ slot }` | A damage meter reached the maximum (once per death) |
| `on_enemy_spawned` | `{ id, x, y, z, hp }` | A zombie appeared (zombies already there when the mod or the scene starts do not count) |
| `on_enemy_killed` | `{ id, x, y, z }` | A zombie's hp dropped to 0 or less (once per zombie) |
| `on_enemy_headshot` | `{ id, x, y, z }` | A zombie's head was shot off (once per zombie) |

The last four events need `"api": "1.1"`. State changes are detected between game steps, so two things that happen in one step arrive together.

```lua
on('on_enemy_headshot', function(e)
  uc.player(1).grenades = uc.player(1).grenades + 1     -- a grenade for every head shot
  log.write('head shot at ' .. e.x .. ', ' .. e.z)
end)
```

## Memory

`uc.mem` reads and writes the game's memory directly. It is the most powerful and the most dangerous tool: a wrong write can crash the game or corrupt a save.

- **Addresses** are Wii addresses: MEM1 `0x80000000` to `0x817FFFFF`, MEM2 `0x90000000` to `0x93FFFFFF`. An address outside is an error, never a crash.
- **Values are big-endian in the game**; the functions convert for you.
- Functions: `read_u8`, `read_u16`, `read_u32`, `read_s8`, `read_s16`, `read_s32`, `read_f32`, `read_f64`, `read_bytes(address, length)` (up to 4096 bytes) and the matching `write_*` (`write_bytes(address, list)`).
- Writes need `touches_gameplay`.

```lua
local value = uc.mem.read_u32(0x80400000)      -- 0x80400000 is only an example of a valid address
uc.mem.write_f32(0x80400004, 1.5)              -- never write an address you have not checked in the game
```

Prefer the named members; they find the right addresses for you. Use memory when the API does not cover what you want, and **verify every address** in the game before you rely on it. Objects such as the player move between runs; follow pointers from fixed places instead of copying an address.

## Symbols and calling game functions

`uc.symbols.lookup(name)` gives the address of a named function or variable (most functions have no name) and `uc.symbols.name_of(address)` the reverse.

`uc.guest.call(address, {args})` **runs a function of the game** with integer arguments (up to 8, in registers r3 to r10) and returns its result:

```lua
local record = uc.guest.call(0x8006C8C8, { 2 })     -- the address of weapon record 2
```

It works only from the game's own events (`on_step`, the events, hook handlers), never from the settings window, and the arguments are **not checked**: a wrong pointer can crash the game. Calls can nest up to four deep. Needs `touches_gameplay`.

## Recipes

**A grenade for every head shot** - see [Events](#events).

**Heal when an enemy dies**

```lua
on('on_enemy_killed', function()
  local p = uc.player(1)
  if p.present then p.health = math.min(1, p.health + 0.05) end
end)
```

**Double damage while the player is below half health**

```lua
local base
on('on_step', function()
  if not base and uc.weapon(0).damage > 0 then
    base = {}
    for id = 0, 19 do base[id] = uc.weapon(id).damage end
  end
  local p = uc.player(1)
  if not base or not p.present then return end
  local factor = p.health < 0.5 and 2 or 1
  for id = 0, 19 do uc.weapon(id).damage = base[id] * factor end
end)
```

**Stars for a long mission**

```lua
on('on_step', function()
  if uc.player(1).present and uc.game.time == 120 then uc.game.stars = uc.game.stars + 100 end
end)
```

**Log the weapon the player holds when it changes**

```lua
local last
on('on_step', function()
  local p = uc.player(1)
  if p.present and p.weapon ~= last then
    last = p.weapon
    log.write('now holding weapon ' .. last)
  end
end)
```

## Good manners

- **Do the minimum every step.** All `on_step` handlers of all mods share 1 ms per step. Keep a copy instead of recomputing, and do heavy work only when something changes.
- **Check `present`** before touching a player.
- **Use events** instead of watching values yourself.
- **Say what you change** in the mod's description, and set `touches_gameplay`.
- **Leave the game as you found it.** Switches and weapon values are given back automatically; undo your own memory writes in `on_unload`.
- **Do not depend on addresses** of moving objects.

## API versions

The API has a version, `major.minor`, written in your `mod.json` as `"api"`. A new minor version only **adds** things; a new major version may change them. `"api": "1.0"` covers players, weapons, the camera, memory, hooks (`pre`) and the first events; `"api": "1.1"` adds enemies, game state, weapons in hands, input, the enemy and death events, `hook.post`, `hook.replace` and calling game functions. The [changelog](../../api-changelog.md) has the full list. A mod for an older minor version keeps working.

## See also

- [Lua](lua.md), [JavaScript](javascript.md) - the languages.
- [Hooks](hooks.md) - code inside game functions.
- [Settings and UI](settings-and-ui.md) - let players adjust values.
- [API reference](../../api/README.md) - every member; [changelog](../../api-changelog.md)

Previous: [Settings and UI](settings-and-ui.md) · Next: [Hooks](hooks.md)

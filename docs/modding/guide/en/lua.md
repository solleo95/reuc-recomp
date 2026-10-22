# Lua mods

[Русский](../ru/lua.md) · **English**

You are here: [Modding guide](README.md) › **Lua**

How to write mods in Lua 5.4: reacting to events, changing players, weapons and enemies, and showing a window. Lua is the best language to start with: small, quick to write, and every example in this guide set uses it.

## Contents

- [What a Lua mod is](#what-a-lua-mod-is)
- [Quick start](#quick-start)
- [How a script runs](#how-a-script-runs)
- [Everything a script can use](#everything-a-script-can-use)
- [Events](#events)
- [Reading and changing the game](#reading-and-changing-the-game)
- [Patterns that work](#patterns-that-work)
- [Errors](#errors)
- [Splitting a script into files](#splitting-a-script-into-files)
- [Examples](#examples)
- [What is not available](#what-is-not-available)
- [Limits](#limits)
- [Debugging](#debugging)
- [Common mistakes](#common-mistakes)
- [See also](#see-also)

## What a Lua mod is

A folder in `mods` with a `mod.json` that names a script file:

```json
{ "id": "my-lua-mod", "name": "My Lua mod", "version": "1.0", "api": "1.1", "touches_gameplay": true, "script": "scripts/main.lua" }
```

and the script itself, `scripts/main.lua`. The script is loaded when the mod is switched on. Its job is to **register handlers** for events and, if it wants, to draw a settings window.

Set `touches_gameplay` to `true` as soon as the script **changes** anything in the game (writes a value, sets a switch, calls a game function). A script that only **reads** the game or only draws a window does not need it.

## Quick start

`mods\hello\mod.json`:

```json
{ "id": "hello", "name": "Hello", "version": "1.0", "api": "1.1", "script": "scripts/main.lua" }
```

`mods\hello\scripts\main.lua`:

```lua
log.write('hello is on')

on('on_step', function()
  if uc.player(1).present and uc.game.time % 10 == 0 then
    log.write('mission time: ' .. uc.game.time)
  end
end)
```

(`uc.game.time` is part of API 1.1, which is why `mod.json` says `"api": "1.1"`.)

Tick the mod in the **Mods** tab and look at `debug\mods.log`. While you work, **Reload scripts** in the Mods tab restarts the scripts with your changes.

## How a script runs

1. The file runs **once from top to bottom** when the mod starts. Everything at the top level (registering handlers, making variables) happens then.
2. After that, the game calls your **handlers** when something happens: every game step, when a player fires, and so on.
3. When the mod is switched off or the scripts are reloaded, `on_unload` runs and the whole Lua state is thrown away. The next start begins from scratch. Do not rely on a variable surviving.

The game state is not ready when the file first runs: the game may be in a menu. Do the real work in handlers, and **check `present`** before reading a player.

## Everything a script can use

| Name | What |
|---|---|
| `on(event, fn)` | Register a handler for an event. Several handlers per event are fine; they run in registration order. |
| `uc` | The game: `uc.player(1)`, `uc.weapon(0)`, `uc.enemy(id)`, `uc.enemies`, `uc.game`, `uc.camera`, `uc.input`, `uc.mem`, `uc.symbols`, `uc.guest`. See [Game API](game-api.md). |
| `settings.get(key)`, `settings.set(key, value)` | The mod's own declared settings ([Settings and UI](settings-and-ui.md)). |
| `ui.draw(fn)` and `ui.*` | Your own settings window ([Settings and UI](settings-and-ui.md)). |
| `hook.pre`, `hook.post`, `hook.replace`, `hook.list` | Code inside game functions ([Hooks](hooks.md)). |
| `log.write(...)` | Writes a line to `debug\mods.log` and to the console. `print` does the same. |
| `require('lib.util')` | Loads `scripts/lib/util.lua` from your mod folder. |

The standard libraries `string`, `table`, `math` and `utf8` and the base functions (`pairs`, `ipairs`, `type`, `tostring`, `pcall`, ...) work as in normal Lua 5.4. Integers stay integers and floats stay floats.

## Events

Register handlers with `on`:

```lua
on('on_weapon_fired', function(e)
  log.write('player ' .. e.slot .. ' fired')
end)
```

| Event | When | Payload |
|---|---|---|
| `on_load` | The mod started | none |
| `on_unload` | The mod is stopping | none |
| `on_step` | About 30 times per game second, not while paused | none |
| `on_settings_changed` | One of the mod's settings changed | `{ key }` |
| `on_weapon_fired` | A player fired | `{ slot }` |
| `on_reload` | A player started to reload | `{ slot }` |
| `on_damage_taken` | A player's damage meter rose | `{ slot, amount }` |
| `on_player_died` | A player's damage meter reached the maximum | `{ slot }` |
| `on_enemy_spawned` | A zombie appeared | `{ id, x, y, z, hp }` |
| `on_enemy_killed` | A zombie's hit points dropped to 0 or less | `{ id, x, y, z }` |
| `on_enemy_headshot` | A zombie's head was shot off | `{ id, x, y, z }` |

The last four need `"api": "1.1"`. Details and examples for every event are in the [Game API](game-api.md#events) guide and the [events reference](../../api/events.md).

`on_step` is the heart of most mods. It runs at the **game's own step rate** (about 30 times per second of game time), whatever the real frame rate. Never count frames yourself.

## Reading and changing the game

Domains that need an id or a slot are **called like functions** and give back a handle:

```lua
local p = uc.player(1)          -- slot 1 or 2
p.magazine = 15                 -- write a property
local damage = p.damage         -- read a property

local w = uc.weapon(0)          -- weapon record 0..19
w.damage = w.damage * 2
```

Domains without an id are plain tables: `uc.camera.position`, `uc.game.time`, `uc.mem.read_u32(addr)`, `uc.enemies.list()`.

Reading or writing a member that does not exist gives an **error with a message that names it**; every member in the [API reference](../../api/README.md) is marked `implemented`, `unverified` or `planned`.

A write needs `touches_gameplay: true`. Without it the script gets an error that names the missing flag.

## Patterns that work

**Wait for the player.** The player does not exist in menus:

```lua
on('on_step', function()
  local p = uc.player(1)
  if not p.present then return end
  -- safe to use p here
end)
```

**Remember the original values.** Read game data in a handler, not at the top of the file (the game fills its tables later), and keep a copy so you can recompute from the original:

```lua
local base
on('on_step', function()
  if not base and uc.weapon(0).damage > 0 then
    base = {}
    for id = 0, 19 do base[id] = uc.weapon(id).damage end
  end
end)
```

**React to settings.** Apply at start, and again when the player changes a setting:

```lua
local function apply() uc.weapon(0).damage = 10 * settings.get('scale') end
apply()
on('on_settings_changed', apply)
```

**Do something every N seconds.** Count steps (30 per second):

```lua
local steps = 0
on('on_step', function()
  steps = steps + 1
  if steps % (30 * 10) == 0 then log.write('ten seconds passed') end
end)
```

**Leave things as you found them.** Switches and weapon values you set through the API are given back by the game when the mod stops. Only your **own** changes (e.g. memory you wrote with `uc.mem`) need cleaning in `on_unload`.

## Errors

Errors from the game API (a bad address, a missing player, a refused write) are ordinary Lua errors. If you expect one, catch it:

```lua
local ok, err = pcall(function() return uc.camera.position end)
if not ok then log.write('no scene yet: ' .. tostring(err)) end
```

An uncaught error in a handler stops that call and counts as a failure of the mod. After **three** failures in a session the mod is switched off until the player unticks and ticks it again; the Mods tab shows the last error.

## Splitting a script into files

```lua
-- scripts/main.lua
local util = require('lib.util')       -- loads scripts/lib/util.lua
on('on_step', function() util.heal(1) end)
```

```lua
-- scripts/lib/util.lua
local M = {}
function M.heal(slot)
  local p = uc.player(slot)
  if p.present then p.damage = 0 end
end
return M
```

`require` only loads files from your own mod folder.

## Examples

**Heal a little for every kill** (`touches_gameplay: true`, `api` 1.1):

```lua
on('on_enemy_killed', function()
  local p = uc.player(1)
  if p.present then p.health = math.min(1, p.health + 0.05) end
end)
```

**Faster automatic pistol** (`touches_gameplay: true`):

```lua
for _, id in ipairs({ 0, 19 }) do
  local w = uc.weapon(id)
  w.fire_mode = 'auto'
  w.fire_rate = 4
end
```

**A kill counter in your own window** (read only: no `touches_gameplay`, `api` 1.1):

```lua
local kills, heads = 0, 0
on('on_enemy_killed', function() kills = kills + 1 end)
on('on_enemy_headshot', function() heads = heads + 1 end)

ui.draw(function()
  ui.text('Kills: ' .. kills)
  ui.text('Head shots: ' .. heads)
  if ui.button('Reset') then kills, heads = 0, 0 end
end)
```

**Give the submachine gun when the mission starts** (`touches_gameplay: true`, `api` 1.1):

```lua
local given = false
on('on_step', function()
  local p = uc.player(1)
  if not p.present then given = false return end
  if not given then
    given = true
    uc.game.select_weapon(1, uc.game.give_weapon(1, 2, 7))   -- weapon 2, ammo key 7
  end
end)
```

More complete mods are in the [mods-examples](../../../../mods-examples) folder.

## What is not available

A script **cannot** read or write files, start programs or use the network. Not available: `io`, `os`, `package`, `debug`, `coroutine`, `dofile`, `loadfile`, `load`, `string.dump`, and `collectgarbage` other than `collectgarbage('count')`. `setmetatable` refuses a metatable with `__gc`.

## Limits

| Limit | Value |
|---|---|
| Time for all `on_step` handlers of all mods together | 1 ms per game step |
| Time for one drawing call | 4 ms |
| Time to load the file | 2 s |
| Memory | 64 MiB per mod |
| Failures before the mod is switched off | 3 per session |

A script that runs too long is stopped with an error that `pcall` cannot catch. Keep handlers short: do not scan memory or build big tables every step.

## Debugging

- `log.write(...)` and the log file `debug\mods.log` are your friends. Log the values you doubt.
- **Reload scripts** in the Mods tab restarts the scripts without leaving the game.
- The Mods tab shows the last error of a mod.
- More in [Testing and sharing](testing-and-sharing.md).

## Common mistakes

| Mistake | Fix |
|---|---|
| "The mod does nothing" | Is it ticked? Is `script` set? Is `touches_gameplay` set when you write? Look at the log. |
| Reading a player in `on_load` | The player does not exist yet. Read in `on_step` after checking `present`. |
| Counting frames | Use `on_step`: it is already in game time. |
| Doing slow work every step | Spread it out or do it once; stay under 1 ms. |
| Keeping state in a global and expecting it to survive a reload | The state is thrown away on every restart. |
| Writing `uc.player(1).infinite_ammo = true` without the flag | Set `touches_gameplay: true` in `mod.json`. |

## See also

- [JavaScript](javascript.md) - the same API from JavaScript.
- [Game API](game-api.md) - everything you can read and change.
- [Settings and UI](settings-and-ui.md) - sliders and your own window.
- [Hooks](hooks.md) - code inside game functions.
- [Lua reference](../../lua.md) and [API reference](../../api/README.md)

Previous: [Data patches](data-patches.md) · Next: [JavaScript](javascript.md)

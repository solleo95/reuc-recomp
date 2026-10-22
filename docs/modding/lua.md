# Lua mods

Set `"script": "scripts/main.lua"` in `mod.json`. The file runs once when the mod starts; it registers handlers and may draw a tab. Lua 5.4 runs in a sandbox.

```lua
log.write('hello from ' .. 'my mod')

on('on_step', function()
  -- runs once per game step, not while paused
  uc.player(1).magazine = 15
end)
```

## What a script can use

| Global | What |
|---|---|
| `on(event, fn)` | Register a handler. Events: `on_load`, `on_unload`, `on_step`, `on_settings_changed`, `on_weapon_fired`, `on_damage_taken`, `on_reload` (the [API reference](api/events.md) says which are implemented). |
| `uc` | The game: `uc.player(slot)`, `uc.mem`, `uc.camera`, `uc.enemies`, ... See [api/README.md](api/README.md). |
| `hook.pre(name, fn)` | Run `fn(regs)` before a hookable game function (see hooks.md). |
| `settings.get(key)`, `settings.set(key, value)` | The mod's own declared settings. |
| `ui.*` | Widgets, only inside the function given to `ui.draw`. |
| `log.write(...)` | Writes to the log (`debug/mods.log` next to the exe, and the console). `print` does the same. |
| `require('lib.util')` | Loads `scripts/lib/util.lua` from the mod folder. Nothing outside the folder can be loaded. |

Standard libraries: `string`, `table`, `math`, `utf8` and the base functions. Not available: `io`, `os`, `package`, `debug`, `coroutine`, `dofile`, `loadfile`, `load`, `string.dump`, and `collectgarbage` other than `collectgarbage('count')`. A script cannot read or write files, start programs or use the network.

## Game objects

Scoped domains are called like functions and return a handle:

```lua
local p = uc.player(1)       -- slot 1 or 2
p.magazine = 15              -- property write
local hp = p.damage          -- property read
local w = uc.weapon('pistol')   -- by name (once the fields are implemented)
```

Domains without a scope are plain tables: `uc.camera.view`, `uc.enemies.list()`, `uc.mem.read_u32(addr)`.

Reading or writing a member that is not implemented yet is an error with a message that names it; the API reference marks every member `implemented`, `unverified` or `planned`. Errors from the API (a bad address, a missing player) are Lua errors: catch them with `pcall` if you expect them.

Numbers: integers stay integers (`uc.mem.read_u32` gives an integer), floats are floats.

## Limits

- Time: all `on_step` handlers of all mods share 1 ms per game step; a menu drawing function gets 4 ms; the file itself gets 2 s to load. A script that runs too long is stopped with an error.
- Memory: 64 MiB per mod.
- Three errors or overruns in one session switch the mod off until the next start (the Mods tab shows why). "Reload scripts" in the Mods tab gives it a fresh start.
- The time limit cannot be caught: `pcall` and `xpcall` pass the "time budget" error on, and pattern matching (`string.find`, `gsub`, ...) checks the clock too, so even a pathological pattern is stopped.
- Known limit: a single long library call that is not pattern matching (for example `table.sort` of a huge table) is checked only when it returns.

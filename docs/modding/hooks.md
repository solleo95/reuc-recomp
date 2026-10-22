# Hooks

A hook runs your function at the start of a game function, before its body. Hooks are for things the game API does not cover yet: you see the arguments the game passes and may change them. Hooks need `"touches_gameplay": true` in `mod.json`.

```lua
hook.pre('GameWeaponUpdate', function(regs)
  -- regs.r3 is the weapon object (a guest address), regs.r4 the "allowed" flag
  log.write(string.format('weapon %08X', regs.r3))
end)
```

```js
hook.pre('GameWeaponUpdate', (regs) => { regs.r4 = 1; });
```

`regs` holds `r3` to `r10` (integers, the PowerPC argument registers) and `f1` to `f8` (floats). Whatever you assign to these fields is written back before the game function runs. Registers you do not touch stay as they were. A callback that raises an error or runs out of its budget changes nothing.

The values are written back as they are: the port does not check them. A register that holds a guest address and gets a wrong one, or a float set to NaN, can crash the game or corrupt its state, just like a wrong `mem.write_*`. Change only what you understand, and keep a hook to the few registers it needs.

## Which functions can be hooked

Only the functions in the list below (also `hook.list()`). Each one has a hook point compiled into the game; for every other function there is no code at all, so mods cannot slow down the game anywhere else. The list grows with releases.

| Name | Address | What |
|---|---|---|
| `GameWeaponUpdate` | `0x8006C268` | Per-step update of a weapon object; r3 is the object (the player's weapon is `player + 0x48`). |
| `PlayerEventHandler` | `0x800676A0` | The player's event handler (damage, firing and similar events). |

(The generated table in [api/README.md](api/README.md) is authoritative.)

A hook point is one load of a pointer that is null until a mod installs a hook, so nothing happens while no mod uses it.

## Timing and safety

- Hooks run on the game thread inside the game function: keep them short. They share the 1 ms budget class of step callbacks (the budget applies to each call).
- Three failures switch the mod off for the session; its hooks stop being called at once.
- Reloading scripts or leaving the game removes the hooks of that runtime.

## `hook.post` and `hook.replace` (API 1.1)

```lua
hook.replace('GameWeaponUpdate', function(regs) end)          -- the original update does not run
hook.post('GameWeaponUpdate', function(regs) log.write(regs.r3) end)
```

- **Order** at one function: `pre` hooks, then `replace` hooks (if there is any, the original body is skipped), then the body, then `post` hooks. A `post` hook runs also when the body was replaced.
- **`replace`:** the callback gets the argument registers. Whatever it leaves in `r3` and `f1` is the function's return value (PowerPC convention); other registers written back stay as the caller sees them. With several replacements installed all of them run, in install order, and the body is skipped. A mod that only wants to look or tweak arguments should use `pre`.
- **`post`:** `r3` and `f1` hold the return value and may be changed; `r4` to `r10` and `f2` to `f8` are whatever the function left in them.
- Both need `"touches_gameplay": true`, like `pre`, run on the game thread under the same time budget and failure rules, and disappear with the mod (switched off or script reloaded).
- Native DLLs: `hook_replace` and `hook_post` in `UcApi` (API 1.1, `uc_mod.h`). A DLL built for 1.1 does not load in a game that offers only 1.0.
- A game function that is not in the list above cannot be replaced either: there is no hook point in it.

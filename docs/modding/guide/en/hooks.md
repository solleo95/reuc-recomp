# Hooks

[Русский](../ru/hooks.md) · **English**

You are here: [Modding guide](README.md) › **Hooks**

How to run your own code at the moment the game calls one of its own functions: look at the arguments, change them, skip the function or react to its result. Hooks are the sharpest tool of the mod system; use them only for what the [game API](game-api.md) cannot do.

## Contents

- [What a hook is](#what-a-hook-is)
- [When to use a hook](#when-to-use-a-hook)
- [Quick start](#quick-start)
- [The registers](#the-registers)
- [The three kinds of hook](#the-three-kinds-of-hook)
- [Which functions can be hooked](#which-functions-can-be-hooked)
- [Examples](#examples)
- [Timing and safety](#timing-and-safety)
- [Hooks in native mods](#hooks-in-native-mods)
- [Troubleshooting](#troubleshooting)
- [See also](#see-also)

## What a hook is

The game is made of thousands of functions. A **hook** attaches a function of yours to one of them. The game calls the function as usual, and just before, just after or instead of it, it also calls yours. You receive the function's **arguments** (the PowerPC registers `r3` to `r10` and `f1` to `f8`), and whatever you change there goes back into the game.

Only functions that are on the **list of hookable functions** accept hooks. Each has a hook point built into the game; everywhere else there is no hook code at all, so mods cannot slow the game down.

## When to use a hook

Use the normal API when you can: `uc.player(1).magazine = 15` is simpler and safer than any hook. A hook is the answer when:

- the game decides something inside a function that you want to veto or change (for example "may this weapon fire this step");
- you need to see the **arguments** of a call that no event reports;
- you want to **replace** what a function does.

Hooks need `"touches_gameplay": true` in `mod.json`.

## Quick start

Stop player 1 from firing while the minus button is held:

```lua
hook.pre('GameWeaponUpdate', function(regs)
  if uc.input.held & 0x10 ~= 0 then     -- minus button
    regs.r4 = 0                         -- "firing is not allowed this step"
  end
end)
```

```js
hook.pre('GameWeaponUpdate', (regs) => {
  if (uc.input.held & 0x10) regs.r4 = 0;
});
```

`regs.r4` is the "allowed" flag of `GameWeaponUpdate`; clearing it stops the held trigger from firing (checked in the game).

## The registers

The PowerPC calling convention passes the first arguments in registers. A hook callback receives a table `regs`:

| Field | Meaning |
|---|---|
| `regs.r3` ... `regs.r10` | The integer arguments, as whole numbers. Pointers are **guest addresses** (`0x80...`). |
| `regs.f1` ... `regs.f8` | The floating point arguments, as numbers. |

What you assign to these fields is **written back before the game function runs**; registers you do not touch stay as they were. A callback that raises an error or runs out of time changes nothing.

The values are written back **as they are**. The port does not check them. A register that holds an address and gets a wrong one, or a number set to NaN, can crash the game or corrupt its state, just like a wrong `uc.mem.write_*`. Change only what you understand and only the registers you need.

`r3` is also where a function's **return value** is passed back (and `f1` for decimals).

## The three kinds of hook

```lua
hook.pre('GameWeaponUpdate', function(regs) ... end)       -- before the function
hook.replace('GameWeaponUpdate', function(regs) ... end)   -- instead of the function
hook.post('GameWeaponUpdate', function(regs) ... end)      -- after the function
```

| Kind | When it runs | What you can do |
|---|---|---|
| `hook.pre(name, fn)` | Before the function body | Look at and change the arguments. |
| `hook.replace(name, fn)` | Instead of the body | The original does **not** run. Leave the return value in `r3` (and `f1`). |
| `hook.post(name, fn)` | After the function returned | `r3` and `f1` hold the return value; you may change them. |

The order at one function is: all `pre` hooks, then all `replace` hooks (if there is any, the body is skipped), then the body, then all `post` hooks. A `post` hook also runs when the body was replaced. With several replacements installed, all of them run in install order and the body is skipped.

`replace` and `post` need `"api": "1.1"`. If you only want to look at arguments or tweak them, use `pre`: it never skips the game's own work.

## Which functions can be hooked

`hook.list()` returns the names. Today:

| Name | Address | What |
|---|---|---|
| `GameWeaponUpdate` | `0x8006C268` | Per-step update of a player's weapon. `r3` is the weapon **object** (the player's is at `player + 0x48`), `r4` the "allowed" flag. Checked in the game. |
| `PlayerEventHandler` | `0x800676A0` | The player's event handler (damage, firing and similar events). Unit-tested, not yet confirmed in a live fight (`unverified`). |
| `StringCopy` | `0x802933D8` | The game's `strcpy`: `r3` is the destination, `r4` the source. Every string copy of the game passes through it, so keep the callback short (`unverified`). The port itself hooks it to give the character names of `dol/names.json`. |

The list grows with every release; the [API reference](../../api/README.md#hookable-functions) always has the current table.

## Examples

**Look at the weapon object** (the weapon id sits at offset `0x480` and the magazine at `0x48C` of the object):

```lua
local last = -1
hook.pre('GameWeaponUpdate', function(regs)
  local id = uc.mem.read_u32(regs.r3 + 0x480)
  if id ~= last then
    last = id
    log.write(string.format('weapon object %08X holds weapon %d', regs.r3, id))
  end
end)
```

**Forbid firing completely** (a replacement that does nothing: the original update never runs, so the weapon does not even fire or reload):

```lua
hook.replace('GameWeaponUpdate', function(regs) end)
```

Test replacements carefully: skipping the whole update also skips things you may not have meant to skip.

**React after the function** (`post` sees the result):

```lua
hook.post('GameWeaponUpdate', function(regs)
  -- regs.r3 is whatever the function returned
end)
```

## Timing and safety

- Hooks run on the **game thread, inside the game function**. Keep them very short: the callback shares the budget class of step callbacks (1 ms per call). Do not scan memory or build tables in a hook.
- **Three failures** (errors or overruns) switch the mod off for the session; its hooks stop being called at once.
- Switching the mod off, or reloading scripts, **removes the hooks** of that script.
- A hook that changes a register to a wrong value can crash the game. Test with care and save the game before you try.
- A function that is not on the list **cannot** be hooked or replaced: there is no hook point in it.
- You can call game functions from a hook with `uc.guest.call` ([Game API](game-api.md#symbols-and-calling-game-functions)); the API allows it inside hook handlers.

## Hooks in native mods

A DLL uses `hook_pre`, `hook_replace` and `hook_post` of the API table, with a callback `void callback(UcHookRegs* regs, void* user)`. `regs->r[0]` is `r3` ... `regs->r[7]` is `r10`; `regs->f[0]` is `f1` ... `regs->f[7]` is `f8`. The functions return 0, or non-zero when the name is not hookable. More in the [Native DLL guide](native-dll.md#hooks). A DLL built for API 1.1 does not load in a game that only offers 1.0.

## Troubleshooting

| Problem | Likely cause and fix |
|---|---|
| An error that names `touches_gameplay` | Add `"touches_gameplay": true` to `mod.json`. |
| "not hookable" or an unknown name | The function is not in `hook.list()`. Use the exact name. |
| The hook is never called | The function is not used in the scene you test; or the mod is switched off after failures (check the Mods tab). |
| The game crashes after a hook | A register was set to a wrong value. Remove the assignment, narrow the change. |
| The effect disappears | Hooks are removed when the mod is switched off or scripts reload; load the mod before the scene you test. |

## See also

- [Game API](game-api.md) - try the normal API first.
- [Native DLL](native-dll.md) - hooks from C/C++.
- [Lua](lua.md), [JavaScript](javascript.md)
- [Hooks reference](../../hooks.md) and [hook API](../../api/hook.md)

Previous: [Game API](game-api.md) · Next: [Native DLL](native-dll.md)

# Data patches

[Русский](../ru/data-patches.md) · **English**

You are here: [Modding guide](README.md) › **Data patches**

How to change numbers in the game - weapon damage, magazine size, reload time, invulnerability - with a small JSON file and **no code at all**.

## Contents

- [What a data patch is](#what-a-data-patch-is)
- [Quick start](#quick-start)
- [The file format](#the-file-format)
- [Keys you can use](#keys-you-can-use)
- [Weapon table](#weapon-table)
- [Order and conflicts](#order-and-conflicts)
- [What gets skipped](#what-gets-skipped)
- [When a patch is not enough](#when-a-patch-is-not-enough)
- [Troubleshooting](#troubleshooting)
- [See also](#see-also)

## What a data patch is

A data patch is a JSON file in the `data` folder of a mod. It maps **names of game values** to **new values**. When the mod starts, the port writes each value into the game, exactly as a script's assignment would. When the mod is switched off, the game's own values come back.

Use a data patch when you only want to **set** numbers once. If the value must depend on something (the player's health, an enemy dying, a setting), write a [Lua](lua.md) or [JavaScript](javascript.md) script instead.

## Quick start

A handgun with a 30-round magazine and a fast reload:

`mods\faster-handgun\mod.json`

```json
{
  "id": "faster-handgun",
  "name": "Faster handgun",
  "version": "1.0",
  "api": "1.0",
  "touches_gameplay": true
}
```

`mods\faster-handgun\data\handgun.json`

```json
{
  "weapon.0.capacity": 30,
  "weapon.0.reload_time": 0.3,
  "weapon.19.capacity": 30,
  "weapon.19.reload_time": 0.3
}
```

Weapon records 0 and 19 are both handgun variants, so both are changed. Start the game, tick the mod in the **Mods** tab, and the handgun reloads in a third of a second and holds 30 rounds (the magazine is refilled at the next reload).

A mod may consist of data files only; it needs no `script`. A mod that changes how the game plays still sets `"touches_gameplay": true`.

## The file format

- A data file is **one JSON object**: `{ "key": value, "key": value }`.
- Put as many files as you like in the `data` folder; every `.json` file is read.
- Files larger than 1 MiB, files that are not valid JSON and files whose root is not an object are skipped with a message in the log.

## Keys you can use

A key has the form `<domain>.<id>.<member>`, or `<domain>.<member>` when the domain has no id. It is the same name you would use in a script, with dots instead of brackets: `weapon.0.damage` is `uc.weapon(0).damage`.

Writable values that make sense in a data patch:

| Key | Value | Meaning |
|---|---|---|
| `weapon.<id>.damage` | number 0 to 10000 | Damage per hit |
| `weapon.<id>.fire_rate` | number 0.05 to 60 | Shots per second |
| `weapon.<id>.capacity` | whole number 0 to 999 | Magazine size |
| `weapon.<id>.reload_time` | number 0 to 1000 | Reload time in seconds |
| `weapon.<id>.fire_mode` | `"semi"`, `"auto"`, `"burst"` | Single shots, held trigger repeats, burst |
| `player.<slot>.invulnerable` | `true` / `false` | Player never takes damage |
| `player.<slot>.infinite_ammo` | `true` / `false` | The magazine is refilled every step |
| `game.stars` | whole number 0 to 9999 | The upgrade money |

`<id>` is a weapon record number 0 to 19, `<slot>` is 1 or 2.

Other members of the [game API](game-api.md) are writable too (`player.<slot>.magazine`, `player.<slot>.grenades`, `game.time`, ...), but a data patch is applied **once when the mod starts**. Values of a player that does not exist yet cannot be set then. For anything that needs a running mission, use a script.

The complete list of members and their limits is in the [game API reference](../../api/README.md).

## Weapon table

The values below are what the US game starts with (they are what a mod sees before changing anything). The game has 20 weapon records:

| Id | Magazine | Damage | Shots per second | Fire mode | Reload time |
|---|---|---|---|---|---|
| 0 | 15 | 10 | 1 | semi | 1.0 |
| 1 (shotgun) | 6 | 9 per pellet | 1 | semi | 2.3 |
| 2 (submachine gun) | 50 | 4 | 13 | auto | 0.5 |
| 3 | 30 | 5 | 15 | burst | 1.0 |
| 4 | 4 | 180 | 1 | semi | 2.7 |
| 5 | 1 | 100 | 1 | semi | 1.0 |
| 7 | 4 | 120 | 1 | semi | 2.3 |
| 8 | 6 | 100 | 1 | semi | 1.7 |
| 9 | 6 | 7 per pellet | 1 | semi | 2.0 |
| 10 | 50 | 7 | 15 | auto | 0.4 |
| 11 | 6 | 80 | 1 | semi | 2.0 |
| 12 | 8 | 60 | 1 | semi | 1.2 |
| 13 | 3 | 120 | 1 | semi | 2.3 |
| 14 | 8 | 8 per pellet | 1 | semi | 1.7 |
| 15 | 40 | 8 | 10 | auto | 0.7 |
| 16 | 1 | 180 | 1 | semi | 2.3 |
| 17 | 3 | 300 | 1 | semi | 2.0 |
| 18 | 1 | 350 | 1 | semi | 2.5 |
| 19 | 15 | 10 | 1 | semi | 1.0 |

Ids 6 and others not named here are special records; read their values with a script (`uc.weapon(6).damage`) before you change them. Only ids 0, 1 and 2 are named with certainty; the rest are identified by their numbers.

Weapon values are **kept for the whole session**: the game fills its weapon table again at the start of a mission, and the port writes your values again whenever the game's own differ.

## Order and conflicts

- Files in `data` apply in **name order**; keys apply in the order they appear in the file.
- Mods apply in **load order** (top of the Mods tab list first).
- When two patches set the same value, **the last one wins**.
- Patches apply once when the mod starts, **after the script's `on_load`** if there is a script.

## What gets skipped

A key that does not work is skipped and logged (file name and reason), and the rest of the file still applies:

- the key does not exist, or the member is not implemented;
- the member is read-only (like `camera.view`);
- the id is not a whole number, or the key has a missing or extra part;
- the value has the wrong type or is outside the allowed range.

Read `debug\mods.log` after trying a patch: every skipped key is listed there.

## When a patch is not enough

- The value must depend on a **setting** the player picks: use a script with [declared settings](settings-and-ui.md) (the [damage multiplier example](../../recipes.md) does exactly that).
- The value must **change during a mission** (refill, healing, a timer): use a script and `on_step`.
- You want to react to **events** (an enemy died, the player fired): [Lua](lua.md) with [events](game-api.md#events).

## Troubleshooting

| Problem | Likely cause and fix |
|---|---|
| Nothing changes | The mod has no `touches_gameplay: true` (the log says so), the file is not in `data\`, or the JSON is invalid. Check `debug\mods.log`. |
| "unknown key" in the log | A typo in the key; compare with the table above or the API reference. |
| The value is out of range | See the limits in the table; the port refuses instead of clamping. |
| A weapon value reverts | It does not: values are kept. Another mod below yours may set the same value; reorder them. |
| The magazine does not change | The new `capacity` takes effect at the next reload. |

## See also

- [Lua](lua.md) - when you need logic.
- [Game API](game-api.md) - every value you can read or write.
- [Settings and UI](settings-and-ui.md) - let players change a value.
- [Data patches reference](../../data-patches.md) - the technical description.

Previous: [Disc files](files.md) · Next: [Lua](lua.md)

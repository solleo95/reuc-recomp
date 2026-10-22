# JavaScript mods

[Русский](../ru/javascript.md) · **English**

You are here: [Modding guide](README.md) › **JavaScript**

How to write mods in JavaScript (or TypeScript). A JavaScript mod can do everything a Lua mod can; choose the language you know better. This guide shows the differences and the JavaScript way of doing the same things. Read the [Lua guide](lua.md) for the ideas behind the API, since they are identical.

## Contents

- [What a JavaScript mod is](#what-a-javascript-mod-is)
- [Quick start](#quick-start)
- [Modules and imports](#modules-and-imports)
- [Everything a script can use](#everything-a-script-can-use)
- [Reading and changing the game](#reading-and-changing-the-game)
- [Errors and promises](#errors-and-promises)
- [TypeScript](#typescript)
- [Examples](#examples)
- [What is not available](#what-is-not-available)
- [Limits](#limits)
- [Lua and JavaScript side by side](#lua-and-javascript-side-by-side)
- [Common mistakes](#common-mistakes)
- [See also](#see-also)

## What a JavaScript mod is

A folder in `mods` with a `mod.json` that points to a `.js` (or `.mjs`) entry file:

```json
{ "id": "my-js-mod", "name": "My JavaScript mod", "version": "1.0", "api": "1.1", "touches_gameplay": true, "script": "scripts/main.js" }
```

The engine is **QuickJS-ng**, which supports modern JavaScript (ES2023). The script runs once when the mod starts and registers handlers, exactly as in Lua. As in Lua, set `touches_gameplay` when the script changes anything in the game.

## Quick start

`mods\hello-js\mod.json` as above (with `"id": "hello-js"`), and `mods\hello-js\scripts\main.js`:

```js
log.write('hello-js is on');

on('on_step', () => {
  if (uc.player(1).present && uc.game.time % 10 === 0) {
    log.write(`mission time: ${uc.game.time}`);
  }
});
```

Tick the mod in the **Mods** tab, watch `debug\mods.log`, and use **Reload scripts** after every edit.

## Modules and imports

The entry file and everything it imports are **ES modules**. Variables you declare with `const` or `let` at the top level stay **local to the file**.

```js
// scripts/main.js
import { clamp } from './lib/math.js';

on('on_step', () => {
  const p = uc.player(1);
  if (p.present) p.magazine = clamp(p.magazine, 1, 15);
});
```

```js
// scripts/lib/math.js
export const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
```

Rules for `import` (and `import()`):

- Only **relative** paths: `./file.js`, `../shared/other.js`.
- The file must stay **inside your mod folder**.
- Not allowed: `https:`, `file:`, drive letters, absolute paths, bare names such as `import 'lodash'`.

## Everything a script can use

The same globals as in Lua: `on`, `uc`, `hook`, `settings`, `ui`, `log`.

| Name | What |
|---|---|
| `on(event, fn)` | Register a handler. Events are listed in the [Lua guide](lua.md#events) and the [Game API guide](game-api.md#events). |
| `uc` | `uc.player(1)`, `uc.weapon(0)`, `uc.enemy(id)`, `uc.enemies`, `uc.game`, `uc.camera`, `uc.input`, `uc.mem`, `uc.symbols`, `uc.guest`. |
| `settings.get(key)`, `settings.set(key, value)` | Declared settings ([Settings and UI](settings-and-ui.md)). |
| `ui.draw(fn)`, `ui.*` | Your own settings window. |
| `hook.pre`, `hook.post`, `hook.replace`, `hook.list` | Hooks ([Hooks](hooks.md)). |
| `log.write(...)` | Writes to `debug\mods.log` and the console. |

Calls take **positional arguments**: `uc.game.give_weapon(1, 2, 7)`. Objects and arrays pass through JSON, so `NaN` and `Infinity` become `null`.

## Reading and changing the game

```js
const p = uc.player(1);         // a handle with accessor properties
p.damage = 0;                   // write
const m = p.magazine;           // read

for (const e of uc.enemies.list()) {
  log.write(`enemy ${e.id} hp ${e.hp}`);
}

on('on_weapon_fired', (event) => log.write(`slot ${event.slot}`));
```

Everything in the Lua guide about **waiting for the player**, **remembering original values**, **timing with steps** and **writes needing `touches_gameplay`** applies unchanged. The member list is in the [Game API guide](game-api.md).

## Errors and promises

- Errors from the API are thrown as exceptions: `try { ... } catch (e) { log.write(e.message); }`.
- An uncaught exception in a handler counts as a failure; after three the mod is switched off until it is unticked and ticked again.
- **Promises** work, and their callbacks run right after the handler that created them. There are **no timers**: no `setTimeout`, no `setInterval`. To wait, count steps in `on_step`.

```js
let steps = 0;
on('on_step', () => {
  steps += 1;
  if (steps % 300 === 0) log.write('ten seconds passed');   // 30 steps per second
});
```

## TypeScript

The file [`uc.d.ts`](../../uc.d.ts) describes the whole API for TypeScript and for editors with JavaScript completion. The port runs **JavaScript only**: write `.ts`, compile to `.js`, and ship the `.js`.

A minimal setup (needs Node.js and TypeScript on your computer):

```json
{
  "compilerOptions": { "target": "ES2022", "module": "ES2022", "outDir": "mods/my-js-mod/scripts", "strict": true },
  "include": ["src", "uc.d.ts"]
}
```

Copy `uc.d.ts` next to `tsconfig.json`, put your code in `src\main.ts`, and run `tsc`. The compiled files land in the mod's `scripts` folder. Imports between your files must end in `.js` in the TypeScript source too (`import { x } from './lib.js'`).

## Examples

**Heal a little for every kill** (`touches_gameplay: true`, `api` 1.1):

```js
on('on_enemy_killed', () => {
  const p = uc.player(1);
  if (p.present) p.health = Math.min(1, p.health + 0.05);
});
```

**Damage multiplier with a setting** (declare a number setting `scale` in `mod.json`; see [Settings and UI](settings-and-ui.md)):

```js
let base = null;
const apply = () => {
  if (!base) {
    if (uc.weapon(0).damage <= 0) return;           // the game has not filled its weapon table yet
    base = Array.from({ length: 20 }, (_, id) => uc.weapon(id).damage);
  }
  const scale = settings.get('scale');
  base.forEach((d, id) => { uc.weapon(id).damage = d * scale; });
};
on('on_step', apply);
on('on_settings_changed', apply);
```

**A window with live numbers** (read only):

```js
ui.draw(() => {
  const p = uc.player(1);
  if (!p.present) { ui.text('not in a mission'); return; }
  ui.text(`damage meter: ${p.damage.toFixed(1)} of 50`);
  if (ui.button('Heal now')) p.damage = 0;   // needs touches_gameplay
});
```

More in the [mods-examples](../../../../mods-examples) folder (`god_mode` is a JavaScript mod).

## What is not available

No `std`, `os`, `require`, `process`, `fetch`, `XMLHttpRequest`, `setTimeout`, `setInterval`, `Deno` or `Bun`. A script cannot read or write files, start programs or use the network.

## Limits

Same as Lua: 1 ms per game step for all `on_step` handlers of all mods together, 4 ms per drawing call, 2 s to load, 64 MiB memory, three failures switch the mod off. The time interruption **cannot be caught** by `try/catch`. Built-in array methods refuse array-likes longer than 16 777 216 elements.

Stack traces name the file; the line of the failing frame inside a function is approximate.

## Lua and JavaScript side by side

| Task | Lua | JavaScript |
|---|---|---|
| Register an event | `on('on_step', function() ... end)` | `on('on_step', () => { ... });` |
| Property write | `uc.player(1).magazine = 15` | `uc.player(1).magazine = 15;` |
| List enemies | `for _, e in ipairs(uc.enemies.list()) do ... end` | `for (const e of uc.enemies.list()) { ... }` |
| Log | `log.write('x ' .. n)` | ``log.write(`x ${n}`)`` |
| Catch an error | `local ok, err = pcall(f)` | `try { f(); } catch (e) { ... }` |
| Other file | `require('lib.util')` | `import { x } from './lib/util.js'` |
| Bit test | `held & 0x400 ~= 0` | `(held & 0x400) !== 0` |
| Not equal | `~=` | `!==` |
| Arrays start at | 1 (in your own tables) | 0 |

## Common mistakes

| Mistake | Fix |
|---|---|
| `Cannot use import statement` or a variable of another file is missing | The entry file must be an ES module: use `import`, not `require`. |
| `setTimeout is not defined` | There are no timers. Count steps in `on_step`. |
| An `import` fails | The path must be relative, inside the mod, and end in `.js`. |
| The same as in Lua: reading a player in `on_load`, forgetting `touches_gameplay`, slow handlers | See the [Lua guide](lua.md#common-mistakes). |

## See also

- [Lua](lua.md) - the same ideas, with more explanation.
- [Game API](game-api.md) - every member.
- [Settings and UI](settings-and-ui.md), [Hooks](hooks.md)
- [JavaScript reference](../../javascript.md)

Previous: [Lua](lua.md) · Next: [Settings and UI](settings-and-ui.md)

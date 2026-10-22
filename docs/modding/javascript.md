# JavaScript mods

Set `"script": "scripts/main.js"` in `mod.json`. The engine is QuickJS-ng (ES2023). Entry files and everything they import are **ES modules**, so top-level `const` and `let` stay local to the file.

```js
import { clamp } from './lib/math.js';

log.write('hello');

on('on_step', () => {
  uc.player(1).magazine = clamp(uc.player(1).magazine, 1, 15);
});
```

TypeScript users get editor completion from [`uc.d.ts`](uc.d.ts) (add it to the project; write `.ts`, compile to `.js`, ship the `.js`).

## What a script can use

The same globals as in Lua ([lua.md](lua.md)): `on`, `uc`, `hook`, `settings`, `ui`, `log`. API calls take positional arguments; objects and arrays are converted through JSON (so `NaN` and `Infinity` become `null`).

```js
const p = uc.player(1);        // a handle with accessor properties
p.damage = 0;
for (const e of uc.enemies.list()) log.write(e.hp);
on('on_weapon_fired', (event) => log.write('slot ' + event.slot));
```

Errors from the API are thrown as exceptions (`try { ... } catch (e) { e.message }`). Promises work and their callbacks run right after the handler that created them; there are no timers.

## Imports

- Only relative specifiers: `./file.js`, `../shared/other.js`.
- The resolved file must stay inside the mod folder; `https:`, `file:`, drive letters, absolute paths and bare names (`import 'lodash'`) are refused.
- `import()` works the same way.

## Not available

`std`, `os`, `require`, `process`, `fetch`, `XMLHttpRequest`, `setTimeout`/`setInterval`, `Deno` and `Bun` do not exist. A script cannot read or write files, start programs or use the network.

## Limits

Array-likes longer than 16 777 216 elements are refused by the built-in array methods (a RangeError), so they cannot run for ever. Same as Lua: 1 ms per step for all `on_step` handlers together, 4 ms per drawing call, 2 s to load, 64 MiB of memory; the interruption cannot be caught by `try/catch`. Three failures switch the mod off until it is switched off and on again. Stack traces name the file; the line of the failing frame inside a function is approximate.

# Моды на JavaScript

[English](../en/javascript.md) · **Русский**

Вы здесь: [Руководство по моддингу](README.md) › **JavaScript**

Как писать моды на JavaScript (или TypeScript). Мод на JavaScript умеет всё, что умеет мод на Lua; выбирайте язык, который знаете лучше. Это руководство показывает различия и то, как то же самое делается на JavaScript. Идеи, стоящие за API, читайте в [руководстве по Lua](lua.md): они одинаковы.

## Содержание

- [Что такое мод на JavaScript](#что-такое-мод-на-javascript)
- [Быстрый старт](#быстрый-старт)
- [Модули и импорт](#модули-и-импорт)
- [Всё, что доступно скрипту](#всё-что-доступно-скрипту)
- [Чтение и изменение игры](#чтение-и-изменение-игры)
- [Ошибки и промисы](#ошибки-и-промисы)
- [TypeScript](#typescript)
- [Примеры](#примеры)
- [Что недоступно](#что-недоступно)
- [Лимиты](#лимиты)
- [Lua и JavaScript рядом](#lua-и-javascript-рядом)
- [Частые ошибки](#частые-ошибки)
- [См. также](#см-также)

## Что такое мод на JavaScript

Папка в `mods` с файлом `mod.json`, указывающим на основной файл `.js` (или `.mjs`):

```json
{ "id": "my-js-mod", "name": "My JavaScript mod", "version": "1.0", "api": "1.1", "touches_gameplay": true, "script": "scripts/main.js" }
```

Движок - **QuickJS-ng**, поддерживающий современный JavaScript (ES2023). Скрипт выполняется один раз при старте мода и регистрирует обработчики, точно как в Lua. Как и в Lua, указывайте `touches_gameplay`, когда скрипт что-либо меняет в игре.

## Быстрый старт

`mods\hello-js\mod.json` как выше (с `"id": "hello-js"`) и `mods\hello-js\scripts\main.js`:

```js
log.write('hello-js is on');

on('on_step', () => {
  if (uc.player(1).present && uc.game.time % 10 === 0) {
    log.write(`mission time: ${uc.game.time}`);
  }
});
```

Поставьте флажок у мода на вкладке **Mods**, следите за `debug\mods.log` и после каждой правки используйте **Reload scripts**.

## Модули и импорт

Основной файл и всё, что он импортирует, - **модули ES**. Переменные, объявленные через `const` или `let` на верхнем уровне, остаются **локальными для файла**.

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

Правила для `import` (и `import()`):

- Только **относительные** пути: `./file.js`, `../shared/other.js`.
- Файл должен лежать **внутри папки вашего мода**.
- Нельзя: `https:`, `file:`, буквы дисков, абсолютные пути, голые имена вроде `import 'lodash'`.

## Всё, что доступно скрипту

Те же глобальные имена, что и в Lua: `on`, `uc`, `hook`, `settings`, `ui`, `log`.

| Имя | Что это |
|---|---|
| `on(event, fn)` | Зарегистрировать обработчик. События перечислены в [руководстве по Lua](lua.md#события) и в руководстве [Игровой API](game-api.md#события). |
| `uc` | `uc.player(1)`, `uc.weapon(0)`, `uc.enemy(id)`, `uc.enemies`, `uc.game`, `uc.camera`, `uc.input`, `uc.mem`, `uc.symbols`, `uc.guest`. |
| `settings.get(key)`, `settings.set(key, value)` | Объявленные настройки ([Настройки и интерфейс](settings-and-ui.md)). |
| `ui.draw(fn)`, `ui.*` | Собственное окно настроек. |
| `hook.pre`, `hook.post`, `hook.replace`, `hook.list` | Хуки ([Хуки](hooks.md)). |
| `log.write(...)` | Пишет в `debug\mods.log` и в консоль. |

Вызовы принимают **позиционные аргументы**: `uc.game.give_weapon(1, 2, 7)`. Объекты и массивы передаются через JSON, поэтому `NaN` и `Infinity` становятся `null`.

## Чтение и изменение игры

```js
const p = uc.player(1);         // дескриптор со свойствами-аксессорами
p.damage = 0;                   // запись
const m = p.magazine;           // чтение

for (const e of uc.enemies.list()) {
  log.write(`enemy ${e.id} hp ${e.hp}`);
}

on('on_weapon_fired', (event) => log.write(`slot ${event.slot}`));
```

Всё, что в руководстве по Lua сказано про **ожидание игрока**, **запоминание исходных значений**, **время по шагам** и **необходимость `touches_gameplay` для записи**, действует без изменений. Список членов - в руководстве [Игровой API](game-api.md).

## Ошибки и промисы

- Ошибки API бросаются как исключения: `try { ... } catch (e) { log.write(e.message); }`.
- Неперехваченное исключение в обработчике считается сбоем; после трёх мод выключается, пока флажок не снимут и не поставят заново.
- **Промисы** работают, а их колбэки выполняются сразу после обработчика, который их создал. **Таймеров нет**: ни `setTimeout`, ни `setInterval`. Чтобы подождать, считайте шаги в `on_step`.

```js
let steps = 0;
on('on_step', () => {
  steps += 1;
  if (steps % 300 === 0) log.write('ten seconds passed');   // 30 шагов в секунду
});
```

## TypeScript

Файл [`uc.d.ts`](../../uc.d.ts) описывает весь API для TypeScript и для редакторов с автодополнением JavaScript. Порт выполняет **только JavaScript**: пишите `.ts`, компилируйте в `.js` и поставляйте `.js`.

Минимальная настройка (нужны Node.js и TypeScript на вашем компьютере):

```json
{
  "compilerOptions": { "target": "ES2022", "module": "ES2022", "outDir": "mods/my-js-mod/scripts", "strict": true },
  "include": ["src", "uc.d.ts"]
}
```

Скопируйте `uc.d.ts` рядом с `tsconfig.json`, поместите код в `src\main.ts` и запустите `tsc`. Скомпилированные файлы попадут в папку `scripts` мода. Импорты между вашими файлами должны оканчиваться на `.js` и в исходнике TypeScript (`import { x } from './lib.js'`).

## Примеры

**Немного лечить за каждое убийство** (`touches_gameplay: true`, `api` 1.1):

```js
on('on_enemy_killed', () => {
  const p = uc.player(1);
  if (p.present) p.health = Math.min(1, p.health + 0.05);
});
```

**Множитель урона с настройкой** (объявите числовую настройку `scale` в `mod.json`; см. [Настройки и интерфейс](settings-and-ui.md)):

```js
let base = null;
const apply = () => {
  if (!base) {
    if (uc.weapon(0).damage <= 0) return;           // игра ещё не заполнила таблицу оружия
    base = Array.from({ length: 20 }, (_, id) => uc.weapon(id).damage);
  }
  const scale = settings.get('scale');
  base.forEach((d, id) => { uc.weapon(id).damage = d * scale; });
};
on('on_step', apply);
on('on_settings_changed', apply);
```

**Окно с живыми числами** (только чтение):

```js
ui.draw(() => {
  const p = uc.player(1);
  if (!p.present) { ui.text('not in a mission'); return; }
  ui.text(`damage meter: ${p.damage.toFixed(1)} of 50`);
  if (ui.button('Heal now')) p.damage = 0;   // нужен touches_gameplay
});
```

Больше примеров - в папке [mods-examples](../../../../mods-examples) (`god_mode` - мод на JavaScript).

## Что недоступно

Нет `std`, `os`, `require`, `process`, `fetch`, `XMLHttpRequest`, `setTimeout`, `setInterval`, `Deno` и `Bun`. Скрипт не может читать или писать файлы, запускать программы или обращаться к сети.

## Лимиты

Те же, что в Lua: 1 мс на шаг игры на все обработчики `on_step` всех модов вместе, 4 мс на вызов рисования, 2 с на загрузку, 64 МиБ памяти, три сбоя выключают мод. Прерывание по времени **нельзя перехватить** через `try/catch`. Встроенные методы массивов отклоняют массивоподобные объекты длиннее 16 777 216 элементов.

Стеки вызовов называют файл; номер строки сбойного кадра внутри функции приблизителен.

## Lua и JavaScript рядом

| Задача | Lua | JavaScript |
|---|---|---|
| Зарегистрировать событие | `on('on_step', function() ... end)` | `on('on_step', () => { ... });` |
| Запись свойства | `uc.player(1).magazine = 15` | `uc.player(1).magazine = 15;` |
| Список врагов | `for _, e in ipairs(uc.enemies.list()) do ... end` | `for (const e of uc.enemies.list()) { ... }` |
| Лог | `log.write('x ' .. n)` | ``log.write(`x ${n}`)`` |
| Перехват ошибки | `local ok, err = pcall(f)` | `try { f(); } catch (e) { ... }` |
| Другой файл | `require('lib.util')` | `import { x } from './lib/util.js'` |
| Проверка бита | `held & 0x400 ~= 0` | `(held & 0x400) !== 0` |
| Не равно | `~=` | `!==` |
| Массивы начинаются с | 1 (в ваших собственных таблицах) | 0 |

## Частые ошибки

| Ошибка | Решение |
|---|---|
| `Cannot use import statement` или не хватает переменной из другого файла | Основной файл должен быть модулем ES: используйте `import`, а не `require`. |
| `setTimeout is not defined` | Таймеров нет. Считайте шаги в `on_step`. |
| `import` не срабатывает | Путь должен быть относительным, внутри мода и оканчиваться на `.js`. |
| То же, что в Lua: чтение игрока в `on_load`, забытый `touches_gameplay`, медленные обработчики | См. [руководство по Lua](lua.md#частые-ошибки). |

## См. также

- [Lua](lua.md) - те же идеи с более подробными объяснениями.
- [Игровой API](game-api.md) - каждый член.
- [Настройки и интерфейс](settings-and-ui.md), [Хуки](hooks.md)
- [Справочник по JavaScript](../../javascript.md)

Назад: [Lua](lua.md) · Дальше: [Настройки и интерфейс](settings-and-ui.md)

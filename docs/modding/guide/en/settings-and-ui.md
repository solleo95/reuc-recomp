# Settings and UI

[Русский](../ru/settings-and-ui.md) · **English**

You are here: [Modding guide](README.md) › **Settings and UI**

How to let players adjust your mod: sliders, checkboxes, drop-downs and text boxes that the game draws for you, and your own window with buttons and live numbers.

## Contents

- [The gear button and the settings window](#the-gear-button-and-the-settings-window)
- [Declared settings - no code](#declared-settings---no-code)
- [Setting types](#setting-types)
- [Translating labels](#translating-labels)
- [Using settings in a script](#using-settings-in-a-script)
- [Where the values are saved](#where-the-values-are-saved)
- [Your own window](#your-own-window)
- [Widgets](#widgets)
- [Keeping widget values](#keeping-widget-values)
- [A complete example](#a-complete-example)
- [Limits](#limits)
- [Troubleshooting](#troubleshooting)
- [See also](#see-also)

## The gear button and the settings window

A mod that declares settings or draws something gets a **gear button** in the **Mods** tab, left of its load-order arrows. The button opens the mod's **settings window**, a window titled with the mod's name. A mod without settings and without drawing has no gear. Mods do not get tabs of their own.

The window shows **declared settings** first (drawn by the game from your `mod.json`), then whatever your **drawing function** adds. You can use either or both.

## Declared settings - no code

List the settings in `mod.json`. The game draws the widgets, remembers the values and restores them at the next start.

```json
{
  "id": "damage-scale",
  "name": "Damage scale",
  "version": "1.0",
  "api": "1.0",
  "touches_gameplay": true,
  "script": "scripts/main.lua",
  "settings": [
    { "key": "scale", "type": "number", "default": 2.0, "min": 0.5, "max": 10, "step": 0.5,
      "label": { "en": "Damage multiplier", "ru": "Множитель урона" },
      "tooltip": { "en": "Applied to every weapon." } },
    { "key": "infinite", "type": "bool", "default": false, "label": { "en": "Infinite ammo" } }
  ]
}
```

Each setting needs a `key` (its name in your script), a `type`, a `default` and a `label` with at least an English text.

## Setting types

| Type | Widget | Default | Notes |
|---|---|---|---|
| `bool` | checkbox | `true` or `false` | |
| `int` | slider (a drag box without a range) | whole number | Optional `min`, `max`, `step`; values snap to the step. |
| `number` | slider (a drag box without a range) | any number | Same options. |
| `choice` | drop-down | one of the choices | `choices` is a list of strings. |
| `text` | text box | a string | At most 1024 bytes. |

Other optional fields: `tooltip` (text shown on hover) and `restart_required: true` (marks a setting that only takes effect after a restart).

What the game enforces, even on hand-edited files: a stored value of the wrong type or an unknown choice falls back to the default; numbers outside the range are clamped. **Your script never sees an invalid value.**

## Translating labels

`label` and `tooltip` are objects with a language code as the key. `en` is required for labels; other languages are optional, and the game falls back to English.

```json
"label": { "en": "Damage multiplier", "ru": "Множитель урона", "de": "Schadensfaktor" }
```

The mod's own name can be translated with `names` in `mod.json`: `"names": { "ru": "Множитель урона" }`.

## Using settings in a script

```lua
local scale = settings.get('scale')          -- read
settings.set('scale', 3.0)                   -- write (clamped to the range, saved)

on('on_settings_changed', function(e)        -- a setting changed in the menu
  log.write('changed: ' .. e.key)
end)
```

```js
const scale = settings.get('scale');
settings.set('scale', 3.0);
on('on_settings_changed', (e) => log.write(`changed: ${e.key}`));
```

- `settings.get` returns `nil`/`null` for an unknown key; `settings.set` returns `false` and changes nothing for an unknown key or a wrong type.
- `on_settings_changed` arrives on the game thread at the start of the **next game step**, so a value changed in the menu reaches your script about one step later. While a slider is being dragged the value is saved and announced **once, when the drag ends**.
- Apply your settings once at start **and** in `on_settings_changed`:

```lua
local function apply() uc.weapon(0).damage = 10 * settings.get('scale') end
apply()
on('on_settings_changed', apply)
```

## Where the values are saved

In `config.json` next to `uc.exe`, under `mods.<your id>.settings`. The player can reset a mod's settings by deleting that part. Settings are saved per mod id, so **do not change the `id` of a published mod**: players would lose their values.

## Your own window

Register **one function** with `ui.draw`. The game calls it every frame while the settings window is open, after the declared settings:

```lua
ui.draw(function()
  ui.text('Player 1')
  if ui.button('Heal') then uc.player(1).damage = 0 end
  ui.same_line()
  ui.tooltip('Sets the damage meter to zero')
end)
```

Widgets only work **inside** the drawing function. The function has a budget of 4 ms per frame; an error or an overrun counts as a failure ([limits](lua.md#limits)). Drawing runs on the menu thread, so it is the one place where the game API's **calls into the game's code** (`uc.guest.call`, `uc.game.select_weapon`) do not work; normal reads and writes do.

A DLL draws with Dear ImGui directly ([Native DLL](native-dll.md#drawing-with-imgui)).

## Widgets

| Widget | Meaning |
|---|---|
| `ui.text(text)` | Plain text. |
| `ui.button(label)` | A button. Returns `true` on the frame it is clicked. |
| `ui.checkbox(label, value)` | A checkbox. Returns the new state. |
| `ui.slider_int(label, value, min, max)` | An integer slider. Returns the new value. |
| `ui.slider_float(label, value, min, max)` | A decimal slider. Returns the new value. |
| `ui.combo(label, selected, items)` | A drop-down; `selected` is a 0-based index, `items` a list of strings. Returns the new index. |
| `ui.input_text(label, value)` | A text box. Returns the new text. |
| `ui.color_edit(label, rgba)` | A colour picker, four numbers 0 to 1. Returns the new four numbers. |
| `ui.separator()` | A horizontal line. |
| `ui.same_line()` | The next widget goes on the same line. |
| `ui.collapsing_header(label)` | A foldable header. Returns `true` while open (draw its content only then). |
| `ui.tooltip(text)` | A tooltip for the widget just drawn. |

Widgets are identified by their label **within your mod**, so give different widgets different labels.

## Keeping widget values

The drawing function runs every frame; **the widgets do not remember values themselves**. You hand the current value in and take the new value out, and keep it in a variable of your own:

```lua
local boost, mode = 1.0, 0

ui.draw(function()
  boost = ui.slider_float('Boost', boost, 0.5, 3.0)
  mode  = ui.combo('Mode', mode, { 'Slow', 'Normal', 'Fast' })
  ui.text('Mode ' .. mode .. ', boost ' .. boost)
end)
```

Variables are lost when the mod restarts. If a value should be remembered, make it a **declared setting** instead and write it with `settings.set`.

## A complete example

A mod with one declared setting, a switch driven by it, and a window that shows live information.

`mod.json`

```json
{
  "id": "immortal",
  "name": "Immortal",
  "version": "1.0",
  "api": "1.0",
  "touches_gameplay": true,
  "script": "scripts/main.lua",
  "settings": [
    { "key": "player1", "type": "bool", "default": true, "label": { "en": "Player 1 cannot be hurt" } },
    { "key": "player2", "type": "bool", "default": false, "label": { "en": "Player 2 cannot be hurt" } }
  ]
}
```

`scripts/main.lua`

```lua
local function apply()
  uc.player(1).invulnerable = settings.get('player1')
  uc.player(2).invulnerable = settings.get('player2')
end
apply()
on('on_settings_changed', apply)

ui.draw(function()
  local p = uc.player(1)
  if not p.present then ui.text('not in a mission') return end
  ui.text(string.format('Damage meter of player 1: %.1f of 50', p.damage))
  if ui.button('Heal now') then p.damage = 0 end
end)
```

## Limits

- 4 ms per drawing call; 1024 bytes for a `text` setting.
- The window is a modal window: while it is open, the player cannot use the rest of the menu.
- Widgets work only inside the drawing function.

## Troubleshooting

| Problem | Likely cause and fix |
|---|---|
| There is no gear button | The mod declares no settings and registers no `ui.draw`; or the mod is off. |
| The value does not change in the game | You read the setting once at start; also apply it in `on_settings_changed`. |
| A slider moves but the script gets the value late | By design: `on_settings_changed` arrives at the next game step. |
| The window flickers or a widget "does not stick" | Keep the value in a variable and hand it back every frame ([Keeping widget values](#keeping-widget-values)). |
| The settings are gone after an update | The `id` or the `key` changed. Keep both stable between versions. |

## See also

- [Lua](lua.md), [JavaScript](javascript.md) - writing the script.
- [Native DLL](native-dll.md) - drawing with Dear ImGui.
- [Game API](game-api.md) - what you can show and change.
- [mod.json reference](../../manifest.md) - every field of a declared setting.
- [UI reference](../../api/ui.md)

Previous: [JavaScript](javascript.md) · Next: [Game API](game-api.md)

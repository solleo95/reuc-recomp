# Settings and the mod's tab

Every enabled mod that declares settings or draws something gets its own tab in the Insert menu, after the built-in tabs. Two ways to fill it, usable together in one mod.

## Declared settings (no code)

List them in `mod.json`. The port draws the widgets, saves the values with its own settings (`config.json`, `mods.<id>.settings`) and restores them next start.

```json
"settings": [
  { "key": "damage_scale", "type": "number", "default": 1.0, "min": 0.5, "max": 10, "step": 0.5,
    "label": { "en": "Damage multiplier", "ru": "Множитель урона" },
    "tooltip": { "en": "Applied to every hit." } },
  { "key": "mode", "type": "choice", "default": "fast", "choices": ["slow", "fast"], "label": { "en": "Mode" } },
  { "key": "infinite", "type": "bool", "default": true, "label": { "en": "Infinite ammo" } },
  { "key": "count", "type": "int", "default": 3, "min": 0, "max": 10, "label": { "en": "Count" } },
  { "key": "name", "type": "text", "default": "", "label": { "en": "Name" } }
]
```

| Type | Widget | Notes |
|---|---|---|
| `bool` | checkbox | |
| `int` | slider (drag box without a range) | `min`, `max`, `step` optional; values snap to the step |
| `number` | slider (drag box without a range) | same |
| `choice` | drop-down | `choices` is a list of strings; the default must be one of them |
| `text` | text box | at most 1024 bytes |

`label.en` is required, other languages are optional (the port falls back to `en`). `restart_required: true` marks a setting that only takes effect after a restart.

Rules the port enforces, also on hand-edited config files: a stored value of the wrong type or an unknown choice falls back to the default; numbers outside the range are clamped. A script never sees an invalid value.

Reading and changing them from code:

```lua
local scale = settings.get('damage_scale')      -- Lua
settings.set('damage_scale', 2.0)
on('on_settings_changed', function(p) log.write(p.key) end)
```

`on_settings_changed` is delivered on the game thread at the start of the next game step (a value changed in the menu reaches your script about one step later). While a slider is being dragged the value is saved and announced once, when the drag ends.

```js
const scale = settings.get('damage_scale');     // JavaScript
settings.set('damage_scale', 2.0);
on('on_settings_changed', (p) => log.write(p.key));
```

## Free drawing

Register one function with `ui.draw`; it is called every frame the tab is visible, after the declared settings.

```lua
ui.draw(function()
  ui.text('Player 1')
  if ui.button('Heal') then uc.player(1).damage = 0 end
  ui.same_line()
  ui.tooltip('Sets the damage meter to zero')
end)
```

The widgets are listed in the [ui reference](api/ui.md). Widgets only work inside the drawing function. The function has a 4 ms budget per frame; exceeding it or raising an error counts as a failure (see safety.md).

A DLL draws with Dear ImGui directly (see native-dll.md).

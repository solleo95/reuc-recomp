# Mods in the launchers (Windows and Steam Deck)

## Intent

The in-game menus have a mod manager. The owner wants the same manager in both launchers, so a player can turn mods on, order them and set them up before the game starts. The result is a full equivalent of the in-game manager, with one limit (below).

## Goals

- A **Mods** tab in the Windows launcher and a fourth tab **Mods** in the Deck launcher (Game, Graphics, Controls, Mods).
- Same functions as the in-game manager: enable/disable, load order (up/down), the native-code switch with its warning, the DLL hash confirmation of a native mod, the list of file conflicts, the state of each mod (enabled, disabled, incompatible, missing dependency, dependency off, error) with its messages, author, version, description, and per mod a settings window with the mod's declared settings (checkbox, integer, number, choice, text).
- State is stored where the game already reads it: the `"mods"` section of `config.json`. The game needs no change: it starts with what the launcher saved.
- Mods are read from `mods/` next to the game executable (the install folder), the same folder the game uses (folders and `.zip` files).

## Non-goals

- Anything that needs a running mod: widgets and tabs that a mod draws itself (`has_ui`), live status ("stopped", "waiting"), audio-conversion progress, "Reload scripts". They stay in the game. Scripts, native DLLs and JS are never loaded by a launcher.
- Installing, downloading or deleting mods; opening the mods folder.
- A mod-specific language beyond what the manager has today (English and Russian; other languages show English, as in the game).

## Anti-goals

- The launcher must never change a mod's files, run its code, or lose the settings of mods it does not know (the section keeps every id it was given; a mod that is not installed keeps its stored settings).
- A launcher save must not drop the `"mods"` section, and a mods save must not drop the launcher's other settings.

## Design

### Headless model (`modding/standalone.h/.cpp`, no ImGui, no host)

`StandaloneMods`: `discover(mods_dir, section, game_id)` builds a `ModRegistry` and keeps, per mod, a `ModSettings` made from the manifest's declarations and `section[id]["settings"]` (created on first use). `to_section()` gives the section to store: `registry.to_config()` plus the settings of every mod that was edited (the same merge `ModMenuUi::commit` does; that merge becomes one shared function so the two cannot differ). `dirty()` says a save is due. Pure logic, unit-tested without a window.

Compatibility with the game version: the launcher does not know the disc's game id. An empty game id skips the "made for another game version" check in `ModRegistry::discover` (today an empty id would flag every mod that lists a game). The game still applies the check when it starts.

### Windows launcher

A Mods tab draws `ModMenuUi::manager_tab` (the in-game widgets, so the two cannot drift). `ModMenuUi` gets a stand-alone mode: with no host it edits settings through `StandaloneMods` instead of `host->edit_settings` and `commit()` saves them through `save_`. The tab's save writes the section into the launcher's config document (`document.properties["mods"]`), so the launcher's later saves keep it.

### Steam Deck launcher

Rows (the launcher's own row/focus model, no ImGui nav) over `StandaloneMods`:
- a toggle row "Allow native code" (turning it on opens the warning modal);
- per mod one row: title, version, state line, wrapped description and messages; its parts are chosen with the D-pad like the Bindings row: enable (A), settings (gear), up, down; a "Confirm" part appears for a native mod whose DLL hash is not yet confirmed (modal with the hash, as in the game);
- a conflicts note at the end;
- a settings modal per mod with rows for its declared settings (toggle, slider for integer/number, choice list, text via the text-entry modal and the system keyboard).
Labels are translated (launcher translations, every language); the mod's own texts follow the manager's rule (English/Russian, else English).

### Saving

Each change saves at once (as the launcher's other settings do) with the same atomic config write. If `config.json` fails to parse, the tab shows the error instead of the list.

## Tests

- Unit: `StandaloneMods` round trip (enable, order, native flag, hash, settings clamp/validation, unknown mod keeps its settings, empty game id), shared merge function.
- `ModMenuUi` stand-alone mode: existing menu_ui tests still pass; a new test clicks a declared setting with no host and checks the saved section.
- Deck: `tab_pages` has the Mods tab; rows test for the mod row sub-focus; snapshots of the tab, a settings modal and the native warning (read by eye).
- Windows launcher smoke run (windowed 1280x720 harness) opens the Mods tab.

## Calls I made (marked as mine)

- Empty game id skips the game-version check in the launcher.
- Text settings on the Deck go through a text-entry modal using SDL text input (Steam keyboard); unverified on hardware.
- No mods-folder button and no installer.
- The Windows tab reuses the in-game widgets rather than a new layout.

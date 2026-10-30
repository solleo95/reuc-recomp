# Steam Deck in-game menu and the settings registry

Date: 2026-10-30. Branch: feature/linux-steamdeck. Decisions were taken by the assistant on the owner's "decide yourself, we will correct later" (marked *call*).

## Intent

The Deck needs its own in-game menu (View button): pauses the game, shows only settings that apply to the Deck, is driven by touch, D-pad, sticks and the standard buttons, and looks good. Behind it a single settings registry says, for every setting, in which of four places it shows: desktop menu, Deck menu, desktop launcher, Deck launcher. A new setting is added in one place.

## Goals

- `launcher/settings_registry.*` (shared by the game and the launcher): one table of items. Each item: stable id, label key, page, kind (toggle, choice, slider, colour, custom), surface mask, accessors on `Options`, optional state (visible / enabled / why) from the options and the machine's capabilities.
- Deck menu (`game/deck_menu.*`) renders every item with the Deck bit generically (no per-item code). A new simple setting appears there by adding a table row.
- Desktop menu and Windows launcher ask the registry whether to show an item (`shown(id, surface)`). *call*: their hand-written widgets, ids and layouts stay (tests and the launcher harness depend on them); only the visibility moves to the table. Complex items (upscaling, frame generation, DLSS 5, bindings editors, Wii Remote, mods, reticle preview) are `custom` rows: listed in the table with their surfaces, drawn by code of the surface that has them.
- Deck launcher: the bit exists in the table; no Deck launcher program is written now (*call*).
- Deck menu pauses the game and audio while open (not in network play, which cannot pause); View opens and closes it, B closes it.

## Non-goals / anti-goals

- No rewrite of the desktop menu or the launcher widgets.
- No Deck glyph artwork yet (button hints are plain text letters); no mods page on the Deck yet; no custom key-binding editor on the Deck yet (gamepad bindings stay with the shared Controllers window, reachable later).
- Nothing on the Deck that cannot work: no DLSS/FSR/frame generation/HDR/ray tracing/DLSS 5/Wii Remote/graphics API/window mode rows.

## Deck menu behaviour

- Layout: dimmed screen, one panel. Left column: pages (Graphics, Effects, Controls, Gyro and sticks, Reticle). Right: rows of the page, big touch targets (>= 56 px at 720p), a footer with button hints.
- Pad: D-pad up/down moves the focus, left/right changes the value (hold repeats), A opens a list / toggles, B or View closes (B closes an open list first), L1/R1 (and L4/R4 not used) switch pages, right stick scrolls, left stick acts as the D-pad.
- Touch (arrives as a cursor + left button): tap a row to focus it, tap a switch to flip it, tap a choice to open its list, drag a slider; drag the list to scroll.
- Every change is applied at once through `LiveSettings` (as the desktop menu does).
- Choice values and slider steps come from the registry, stepping is a pure function (`settings::step`) tested without a window.

## Items on the Deck (calls)

Graphics: render scale, FPS limit (30..90), HUD position, upscaling (Off / SMAA). Effects: anisotropy, per-pixel lighting, ambient occlusion, motion blur, contact shadows, bloom, bloom strength. Controls: touch smoothing, reload zone and size, prompt style. Gyro and sticks: gyro mode, sensitivity X/Y, invert X/Y, aim stick speed, aim dead zone, camera dead zone. Reticle: size, hide wake / dot / marks / arc, wake length, dot size, partner opacity, colours (presets).

## Builder notes

- Registry has no ImGui dependency; the generic drawing and navigation live in the game.
- Strings: existing keys where one exists; new keys (`deck_*`) go to `launcher/translations.inc` with all 13 languages.
- Tests: registry (every id unique, every label key translated, surface rules, stepping/clamping, round trips of accessors), Deck menu navigation, `host_input` (existing).
- Deploy: full `build.bat` for Windows; `scripts/linux/build.ps1 -Target uc` and `deck.py stage` for the Deck.

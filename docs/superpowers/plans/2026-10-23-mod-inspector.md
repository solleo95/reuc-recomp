# Mod inspector Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline, as for the earlier milestones). Steps use checkbox syntax.

**Goal:** A mod author switches on one example mod and sees, live, what the game exposes to mods: both players, the mission, the camera, the pad, the nearest enemies, a log of game events, and any memory address or symbol they type in; one button writes a snapshot to `debug\mods.log` for bug reports and notes.

**Architecture:** An example mod `mods-examples/inspector/` in JavaScript, read-only (no `touches_gameplay`, API 1.1), whose settings window (`ui.draw`) shows collapsible sections. Pure helpers live in `scripts/lib/inspect.js` (formatting, distance sorting, the event ring, address parsing); `scripts/main.js` reads `uc.*` and draws. The mod needs no change in the port: it is the first user of the whole read side of the API, which makes it a check of that API too. A `log_every` setting (0 = off) writes the snapshot every N game seconds; the harness uses it for a live check.

**Spec:** the owner's plan item "tools (inspector, free camera, randomizer)" (2026-10-22); no design document: the choices below are mine and are listed for the owner to check.

## My calls (the owner did not specify the inspector)

- It is an **example mod**, not a built-in window: it uses only the public API, so authors can copy and change it, and it needs no port changes. Built-in menu tabs stay for the port's own settings.
- **Read-only.** Memory watches read, never write; no `touches_gameplay` flag, so it can be on while playing normally.
- Sections (collapsing headers, all closed on first open except Players): Players, Mission and camera, Pad, Enemies, Events, Memory watch, Snapshot.
- Settings: `enemies_shown` (number, 1 to 30, default 8), `events_kept` (number, 5 to 100, default 30), `log_every` (number, 0 to 600 game seconds, default 0).
- Units shown the way mod authors write them: positions in meters (the game's unit is 10 cm), health as a percentage and the raw damage meter next to it.

## Global Constraints

- JavaScript (ES modules, as the platform runs them): no timers other than the platform's `setTimeout` (not needed), no `touches_gameplay`, no writes to the game; a failing read of one section (no scene, no player) is shown as text in that section and never stops the others or throws out of `ui.draw`.
- Per-step cost is tiny: the step handler only logs events; everything else is read in `ui.draw` or on the `log_every` tick (an interval of whole game seconds); the enemy list is read once per draw.
- Event ring: the newest `events_kept` lines, oldest dropped; lines read `[mm:ss] name key=value ...` with the mission clock `uc.game.time` (seconds) when a mission runs.
- Edit source only with Edit/Write; tests: `ctest --test-dir build --output-on-failure -R mod_examples`; the live check is `python -m uctest run inspector_live`; full `build.bat` deploy at the end.

## Review Focus

1. Not in a mission (no players, no scene): every section says so, no exception, no stale numbers.
2. A garbage address in the memory watch (empty, `zz`, `0x`, a huge or negative number, an unmapped address, a symbol that does not exist): a one-line message, never a throw out of `ui.draw`.
3. Many enemies (30) and enemies with the same distance: stable order, limit respected, `enemies_shown` changes take effect at once.
4. A burst of events (hundreds in one step): the ring stays at `events_kept`, the step stays within its budget.
5. Changing `events_kept` down while the ring is longer: it is trimmed at once.
6. The snapshot line is one line of valid JSON (so it can be pasted into a bug report and parsed) even with `null`s for missing parts.

---

### Task 1: the inspector mod

**Files:** Create `mods-examples/inspector/mod.json`, `mods-examples/inspector/scripts/main.js`, `mods-examples/inspector/scripts/lib/inspect.js`; Modify `tests/test_mod_examples.cpp`

**Interfaces:**
- Produces (`lib/inspect.js`, all pure; positions are `[x, y, z]` in game units):
  - `export function clockText(seconds)` -> `"mm:ss"` (`"--:--"` for null)
  - `export function playerLine(slot, p)` -> one line for a player snapshot `{health, damage, weapon, magazine, reserve, grenades, hits, critical_hits, objects_destroyed}` (`reserve` null = infinite)
  - `export function enemyLine(e, camera)` -> `"#id hp 20.0  12.3 m  (x, y, z)"`
  - `export function nearest(enemies, camera, limit)` -> the `limit` enemies nearest to `camera` (`null` camera: in id order), ties by id
  - `export function eventLine(seconds, name, data)` -> `"[mm:ss] name key=value ..."`
  - `export class EventRing { constructor(limit); push(line); setLimit(n); lines(); clear(); }`
  - `export function parseAddress(text, lookup)` -> `{address}` or `{error}`; `text` is decimal, `0x` hex, or a symbol name resolved with `lookup(name)` (returns an address or null)
  - `export const WATCH_TYPES` = `["u8", "u16", "u32", "s32", "f32"]` and `export function readWatch(mem, address, type)` -> `{text}` with decimal and hex (or the float), calling `mem.read_<type>`; errors as `{error}`
  - `export function snapshot(state)` -> a one-line JSON string
- Consumes: nothing from other tasks.

- [ ] Tests first (`tests/test_mod_examples.cpp`, new `inspector()` on the example rig; the rig's `RecordingUi` gains `bool open_headers` for `collapsing_header` and maps for typed `input_text` and chosen `combo` values): `inspector_in_a_mission` (headers open: texts hold `Player 1` and `Player 2` lines with the magazines the rig wrote (15 and 8) and no `not in a mission`), `inspector_outside_a_mission` (player table zeroed: Players says `not in a mission`, draw still returns ok), `memory_watch_reads_what_is_typed` (typed `0x80100200` with a u32 written there shows its decimal and hex; typed `zz`, an empty text, `0x`, `-5`, `0xFFFFFFFF` and an unknown symbol each give a one-line message and `draw_ui(...).ok`), `events_are_logged_and_trimmed` (the rig lowers player 1's magazine between steps so the platform emits `on_weapon_fired`; the Events section then lists a `weapon_fired` line; with `events_kept` 5 and more than 5 events only 5 lines show), `snapshot_is_one_line_of_json` (pressing the Snapshot button, the rig's button override returns true for it, writes one `[inspector] {json}` line to `debug/mods.log` that parses as JSON with `players`, `game`, `camera`, `enemies` keys, `null` where nothing exists), `log_every_writes_snapshots` (`log_every` 1: after 31 steps one more snapshot line appears), `settings_are_declared` (the three keys with their ranges). Run: FAIL (no such example).
- [ ] Implement the mod and its library as specified; run: PASS. Commit "Add the inspector example mod".

### Task 2: live check, docs, deploy

**Files:** Modify `scripts/uctest/scenarios.py` (scenario `inspector_live`, mod settings for it), `scripts/uctest/client.py` (`_install_mods` takes the stored settings of a mod), `scripts/uctest/__main__.py` (passes them); Create `docs/modding/inspector.md`; Modify `docs/modding/guide/en/README.md`, `docs/modding/guide/ru/README.md` (example mods table), `docs/modding/recipes.md` if it lists examples

- [ ] Failing check first: `inspector_live` starts the train chapter with the inspector (setting `log_every` 1), plays 20 game seconds and requires in `debug\mods.log` at least 10 `[inspector] {...}` lines that parse as JSON, `game.time` rising between the first and last, a camera `position` of three numbers, player 1 present with `health` between 0 and 1, and an `enemies` array (it may be empty). Expected before the mod exists: FAIL (mod not found).
- [ ] Run it with the mod: PASS. Then one capture of the mod's window in the Insert menu (the mod's gear opens its window; the scenario takes the capture) looked at by eye: sections readable, nothing clipped, in 1280x720.
- [ ] Docs: `docs/modding/inspector.md` (what each section shows, the settings, the snapshot line format, how to read the log, how to copy the mod as a starting point), a row in both guides' example tables. The docs link test passes.
- [ ] Full `ctest`, full `build.bat` deploy, one Opus review of the whole change, fix findings (each with a failing test first), commit "Inspector example mod (live check, docs)".

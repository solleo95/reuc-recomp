# Mod randomizer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (inline, as for the earlier milestones). Steps use checkbox syntax.

**Goal:** One example mod, `randomizer`, that shuffles the game from a seed: weapon stats, zombie hit points and (optionally) the weapon each player starts a mission with. The same seed always gives the same game; a button picks a new seed.

**Architecture:** An example mod `mods-examples/randomizer/` in JavaScript, API 1.1, `touches_gameplay`. Pure helpers in `scripts/lib/random.js` (seeded PRNG, log-uniform factors, the weapon plan, the enemy hit-point rule, the start-weapon pool) are used by `scripts/main.js`, which reads the game's own values first, writes the planned ones and shows what it did in its window. No port change: it uses the public API (`uc.weapon`, `uc.enemy`, `uc.enemies`, `uc.game.give_weapon/select_weapon`).

**Spec:** the owner's plan item "tools (inspector, free camera, randomizer)"; no design document, the choices below are mine and are listed for the owner to check. Facts from the game: the weapon table of the train chapter (read live 2026-10-23): 20 records; id 6 has capacity 0 and rate 0 (not a gun), all others have capacity 1 to 50; `fire_mode` values differ per weapon (auto 0, single 1, 3 on id 3) and are left alone.

## My calls (the owner did not specify the randomizer)

- It is an **example mod**, like the inspector: copyable, uses only the public API, switchable live.
- **Deterministic:** every random draw comes from a PRNG seeded by `seed` and the thing being changed (kind and id), so a change does not depend on the order of events (a zombie gets the same hit points whenever it is seen).
- **Strength** (percent, default 50): a factor is log-uniform in `[1/(1+2s), 1+2s]` with `s = strength / 100` (50: half to double, 100: a third to triple, 0: no change).
- Weapons: for every record `damage`, `fire_rate`, `capacity` and `reload_time` get a factor, each only when the game's value is above 0 (id 6 stays as it is); capacity is rounded and at least 1; limits of the API are respected (damage at most 10000, rate 0.05 to 60, capacity at most 999, reload at most 1000).
- Enemies: a zombie's hit points are multiplied once, when first seen (spawn event, or the periodic scan for zombies that were there at the start of the scene), at least 1.
- Start weapon (off by default: it changes progression): when a player appears (a mission starts), `give_weapon` + `select_weapon` with a weapon drawn from the table's real guns (capacity and rate above 0, taken from the game's own values, not the randomized ones).
- A "New seed" button in the mod's window; the window lists what was changed.

## Global Constraints

- JavaScript ES modules; `touches_gameplay: true`; API 1.1; no timers; reads of a missing part (no mission, no scene) never throw out of `ui.draw` or a handler.
- The game's own values are read before the first write (the table is ready when `uc.weapon(0).damage > 0`) and kept as the base; turning `weapons` off writes the base back; stopping the mod restores the game's values (the platform does).
- Per-step cost: the step handler does O(1) work except one enemy scan every 6th step (at most 30 zombies); events cost O(1).
- Edit source only with Edit/Write; tests: `ctest --test-dir build --output-on-failure -R mod_examples`; the live check is `python -m uctest run randomizer_live`; full `build.bat` deploy at the end.

## Review Focus

1. Seed 0 and seed values at the ends of the range, strength 0 and 100: no NaN, no value outside the API's limits, `strength 0` changes nothing at all.
2. Toggling `weapons`, `enemies` and `start_weapon` while a mission runs; changing the seed or the strength re-plans from the game's base values, never from already randomized ones (no compounding).
3. A zombie seen twice (event and scan), scene changes (ids reused, `seen` reset), dead zombies (hp 0) left alone.
4. Start weapon when player 2 is absent, when the pool is empty (table not ready), and not repeated every step while the player stays.
5. The window outside a mission and before the table is ready: plain text, no throw.
6. Same seed in two runs gives byte-equal planned values (determinism), different seeds differ.

---

### Task 1: the randomizer mod

**Files:** Create `mods-examples/randomizer/mod.json`, `mods-examples/randomizer/scripts/main.js`, `mods-examples/randomizer/scripts/lib/random.js`; Modify `tests/test_mod_examples.cpp` (example count 8, new tests)

**Interfaces:**
- Produces (`lib/random.js`, pure):
  - `export function rng(seed, kind, id)` -> a function returning numbers in `[0, 1)`; `kind` is a small integer (0 weapon, 1 enemy, 2 start weapon) and `id` an integer
  - `export function factor(draw, strength)` -> a number in `[1/(1+2s), 1+2s]` (`s = strength/100`), exactly 1 for strength 0
  - `export function planWeapon(seed, strength, id, base)` -> `{damage, fire_rate, capacity, reload_time}` for a base record `{damage, fire_rate, capacity, reload_time}`; fields whose base is not above 0 are returned unchanged; limits as above
  - `export function enemyHp(seed, strength, id, hp)` -> hit points, at least 1; `hp <= 0` returns `hp`
  - `export function startPool(base)` -> ids of records with capacity and fire_rate above 0
  - `export function startWeapon(seed, slot, pool)` -> an id from the pool (`null` for an empty pool)
- Consumes: nothing from other tasks.

- [ ] Tests first (`tests/test_mod_examples.cpp`, `randomizer()` tests on the example rig; the rig's weapon table: damage `10 * (i + 1)`, capacity 15, reload 1.0, mode 1, fire rate 0 (so rate stays as it is)): `randomizer_weapons_follow_the_seed` (seed 7, strength 50: after a step every damage lies in `[orig/2, orig*2]`, at least 10 of 20 differ, capacities are integers from 8 to 30, reloads in `[0.5, 2]`; a second world with the same seed has identical values, seed 8 differs), `strength_zero_changes_nothing`, `strength_100_stays_within_a_third_to_triple_and_the_limits` (also seed 0 and 999999), `switching_weapons_off_restores_the_game_values` and back on re-plans from the base (the values equal the first plan, no compounding), `changing_strength_replans_from_the_base`, `window_shows_the_seed_and_new_seed_changes_it` (the text `Seed 7`; with the button pressed the next draw shows another seed and the weapons follow it), `outside_a_mission_the_window_is_plain_text` (players gone, table not ready: draw ok), `randomizer_settings_are_declared` (keys `seed` int 1..999999 default 1, `strength` int 0..100 step 5 default 50, `weapons` bool true, `enemies` bool true, `start_weapon` bool false; `touches_gameplay` true; api 1.1). Run: FAIL (no such example).
- [ ] Implement the mod and its library as specified; run: PASS. Commit "Add the randomizer example mod".

### Task 2: live check, docs, deploy

**Files:** Modify `scripts/uctest/scenarios.py` (scenario `randomizer_live`, settings for it); Modify `docs/modding/recipes.md`, the guides' `javascript.md` example mentions (EN/RU)

- [ ] Failing check first: `randomizer_live` starts the train chapter with the mod (seed 1, strength 100, `start_weapon` on) and requires, read from the guest: the weapon table differs from the game's (handgun record 0: damage 10, capacity 15, reload 1.0 originally) in at least 3 of 20 records and every changed damage lies within a third to triple of the original table (the originals are the values read in the control run `randomizer_plain`... see below); the zombies of the first scene (`frame["enemies"]`, ids 160 to 163 with hit points 20, 10, 30, 20 in the unmodified game) have hit points that differ from those in at least one case and lie within a third to triple of them; player 1 holds weapon id (read at `player + 0x4C8`) from the pool, not necessarily 0. Expected before the mod exists: FAIL (mod not found).
- [ ] Run it with the mod: PASS. Look at one capture of the game (a weapon other than the handgun in the HUD when the start weapon is on).
- [ ] Docs: a recipe in `recipes.md` (what it changes, settings, determinism, how to share a seed), a mention in both `javascript.md` guides. Link test passes.
- [ ] Full `ctest`, full `build.bat` deploy, one Opus review, fixes with failing tests first, commit "Randomizer example mod (live check, docs)".

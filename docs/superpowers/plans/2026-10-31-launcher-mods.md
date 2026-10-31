# Mods in the launchers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Mods tab in the Windows launcher and in the Steam Deck launcher that manages mods before the game starts, with the in-game manager's functions.

**Architecture:** A headless `StandaloneMods` (registry + per-mod `ModSettings` + section writer) in `uc_mods`. The Windows launcher draws the existing `ModMenuUi::manager_tab`, which gets a host-less mode over `StandaloneMods`. The Deck launcher draws its own rows over the same `StandaloneMods`. Both save the `"mods"` section into `config.json` through the launcher's config document.

**Tech Stack:** C++20, Dear ImGui, nlohmann_json, the repo's `check.h` tests, Linux cross build (`scripts/linux/build.ps1`), Deck snapshots (`scripts/linux/deck_snapshot.py`).

**Spec:** `docs/superpowers/specs/2026-10-31-launcher-mods-design.md`

## Global Constraints

- Edit source only with Edit/Write (no heredoc/sed edits). Match the surrounding comment density and naming.
- No change to the game's behavior: `GameMods`, `ModHost`, `ModMenuUi` in-game paths stay as they are; `test_mod_menu`, `test_mod_registry`, `test_mod_settings`, `test_deck_pages`, `test_settings_registry` keep passing.
- Launchers never load mod code. Mod-drawn UI, status, reload stay in the game only.
- Windows: new tab beside the existing four (`page` index 5; the hidden Sound page keeps index 4). Deck: `Tab::mods` fourth; L1/R1 order Game, Graphics, Controls, Mods, then the Language select.
- Every new launcher text key exists in every language (`launcher/translations.inc`, checked by the locale test).
- Tests/snapshots run windowed 1280x720; never fullscreen. Commit only when the owner asks.
- At the end: full `build.bat` (deploys to the game folder) and a Deck deploy of the launcher files like earlier rounds.

## Review Focus

- No `mods/` folder, or no `"mods"` section in `config.json`: the tab shows "no mods", nothing is created or changed on disk except what the user changes.
- Settings stored for a mod that is not installed (or temporarily broken) survive a launcher save.
- A broken `mod.json`, duplicate ids and a `.zip` mod show their error line and cannot crash the tab; `.cache` extraction is the same as the game's.
- Saving other launcher settings after a mods change keeps `"mods"`, and a mods change keeps the launcher's other keys (`game`, `graphics`, `launcher`, ...).
- A native mod: enabling needs "Allow native code" + the hash confirmation; turning native code off or changing the DLL clears the confirmed state exactly as in the game (`ModRegistry` already decides; the UI only mirrors it).

---

### Task 1: `StandaloneMods` and the registry's empty game id

**Files:**
- Create: `modding/standalone.h`, `modding/standalone.cpp`, `tests/test_mod_standalone.cpp`
- Modify: `modding/registry.cpp:115` (skip the game-version check when `game_version_` is empty), `modding/CMakeLists.txt` (add `standalone.cpp` to `uc_mods`), `tests/CMakeLists.txt` (test target like `test_mod_registry`), `modding/menu_ui.cpp` (use the shared merge in `commit()`)

**Interfaces:**
- Produces:
  - `namespace uc::mods { nlohmann::ordered_json merge_mod_settings(nlohmann::ordered_json section, const std::map<std::string, nlohmann::ordered_json>& settings); }` — sets `section[id]["settings"] = settings[id]` for each entry (creating the object when missing); the code now inlined in `ModMenuUi::commit` (lines 355-362).
  - `class StandaloneMods { public: StandaloneMods(std::filesystem::path mods_dir, nlohmann::ordered_json section, std::string game_id = {}); ModRegistry& registry(); ModSettings* settings(std::string_view id); void mark_dirty(); bool take_dirty(); nlohmann::ordered_json section(); std::string dll_hash(std::string_view id); }`
    - `settings(id)`: built on first call from the entry's `manifest.settings` and `section[id]["settings"]`; null for an unknown id or a broken entry; the same object on later calls.
    - `section()`: `merge_mod_settings(registry().to_config(), saved)` where `saved` holds `to_json()` of every mod whose `ModSettings` is `dirty()` or was saved before; mods never opened keep their stored settings untouched (the registry leaves per-mod objects alone, per `registry.h`).
    - `dll_hash(id)`: sha256 hex of the entry's native DLL (confined to the mod root), cached, empty when none; use the same helper `ModMenuUi` uses for `hash_cache_`.
    - `mods_dir` that does not exist: an empty registry.
- Consumes: `ModRegistry::discover(mods_dir, section, ApiVersion{1, 1}, game_id)` (same api version `GameMods` passes), `ModSettings(decls, stored)`.

- [ ] **Step 1: Write `tests/test_mod_standalone.cpp`** (fixtures as in `test_mod_registry.cpp`: `fresh`, `put`, `mod`). Tests:
  - `empty_folder_and_missing_section`: `StandaloneMods(fresh("sa_none"), {})` → `registry().entries().empty()`, `section()` equals `registry().to_config()`.
  - `enable_order_native_round_trip`: three mods a,b,c; `set_enabled("a",true)`, `move("c",-1)`, `set_allow_native(true)`; `section()` re-fed to a new `StandaloneMods` gives the same `wanted`, order and `allow_native`.
  - `declared_setting_edit_saves_and_validates`: mod with an integer setting `n` (min 0, max 10, default 5); `settings("a")->set("n", 99)` clamps to 10, `mark_dirty()`, `section()["a"]["settings"]["n"] == 10`; a new instance from that section reads 10.
  - `unknown_mod_settings_survive`: section holds `"gone": {"settings": {"x": 1}}` for an uninstalled mod; after enabling "a" and `section()`, `"gone"` is unchanged.
  - `empty_game_id_skips_version_check`: mod with `"game":["X"]`: with game id `""` its state is not `incompatible`; with game id `"Y"` it is.
  - `merge_creates_missing_objects`: `merge_mod_settings({}, {{"m", {{"k",1}}}})["m"]["settings"]["k"] == 1`.
- [ ] **Step 2: Run to verify it fails** — `cmake --build build --target test_mod_standalone`; Expected: FAIL to compile (`standalone.h` missing).
- [ ] **Step 3: Implement** `standalone.h/.cpp`, the one-line guard in `registry.cpp`, and replace the inlined merge in `ModMenuUi::commit` with `merge_mod_settings`.
- [ ] **Step 4: Run** `ctest --test-dir build -R "test_mod_(standalone|registry|settings|menu)" --output-on-failure`; Expected: all PASS.
- [ ] **Step 5: Review check** — grep callers of `ModRegistry::discover` (only `GameMods` and the new class) to confirm the empty-id guard changes nothing in the game (game id is never empty there).

### Task 2: `ModMenuUi` host-less mode

**Files:**
- Modify: `modding/menu_ui.h`, `modding/menu_ui.cpp`, `tests/test_mod_menu.cpp`

**Interfaces:**
- Produces: `void ModMenuUi::set_standalone(StandaloneMods* mods)` — when set and `host_()` is null: `draw_mod_window` draws the declared settings from `mods->settings(id)` (no script UI), `commit()` marks a save due when any `settings(id)->dirty()` (then `clear_dirty()`, record into `saved_settings_`) and calls `save_(merge_mod_settings(registry.to_config(), saved_settings_))`. With a host the behavior is unchanged.
- Consumes: Task 1 `StandaloneMods`, `merge_mod_settings`.

- [ ] **Step 1: Write the failing test** in `tests/test_mod_menu.cpp` (follow `declared_setting_click_and_commit`): `standalone_declared_setting_click_and_commit` — a headless menu with `GetHost` returning null and a `StandaloneMods` over a temp mods folder with one mod that declares a boolean `on`; open its settings window, `click("setting/alpha/on")`, call `commit()` after the 100 ms edit pause; assert the `save_` callback received a section whose `["alpha"]["settings"]["on"]` flipped, and the gear button exists for a mod with declared settings but is absent for one without.
- [ ] **Step 2: Run** `ctest --test-dir build -R test_mod_menu --output-on-failure`; Expected: FAIL (`set_standalone` undefined).
- [ ] **Step 3: Implement** the mode (small branches in `draw_mod_window` and `commit`; the `manager_tab` body is untouched).
- [ ] **Step 4: Run** the same command; Expected: PASS, including the existing in-game cases.

### Task 3: Windows launcher Mods tab

**Files:**
- Modify: `launcher/main.cpp` (nav list near line 518, page body near line 601), `launcher/CMakeLists.txt` (link `uc_mod_menu` into `uc_launcher`), `launcher/translations.inc` (`mods` tab label, `mods_hint`, page title)
- Test: the launcher smoke path in `launcher/main.cpp` (`track()` ids) and an entry in the existing launcher smoke test if one drives pages (grep `smoke` in `tests/CMakeLists.txt`).

**Interfaces:**
- Consumes: Tasks 1-2. Mods folder = the folder holding `uc.exe` (use the variable `main.cpp` already uses to locate/launch the game executable).
- Produces: nav entries become an explicit list of page indices `{0,1,2,3,5}` (the Sound page 4 stays hidden); `sections[]` and `hints[]` gain `"mods"` / `"mods_hint"` at index 5; the page draws `menu.manager_tab(unit)` inside the scrolling `"settings"` child; no game-folder row under it (like the Controls pages: `controls_page` also true for 5, so only Multiplayer and Play remain).
- Wiring: `StandaloneMods mods_state(install_dir / "mods", document.properties.value("mods", ordered_json::object()), "")`; `ModMenuUi mods_menu([&]{ return &mods_state.registry(); }, []() -> ModHost* { return nullptr; }, active_language_code)`; `set_standalone(&mods_state)`; `set_save` writes `auto next = document; next.properties["mods"] = section; uc::save_config(config_path, next); document = std::move(next);` (same pattern as `save` at line 303); call `mods_menu.commit()` once per frame after drawing. The menu's language is `"ru"` or `"en"` from the launcher language (other languages: `"en"`).
- Navigation icon for the new tab: reuse the Controllers icon index mapping already in `draw_navigation_icon` (add a branch only if no icon fits; check the icon function first).

- [ ] **Step 1: Failing check** — extend the launcher smoke sequence (or add a snapshot-style smoke step if the launcher has one) to select page 5 and assert `track("mods")` registers; run it; Expected: FAIL (no such page).
- [ ] **Step 2: Implement** the wiring above plus the translation keys in all languages.
- [ ] **Step 3: Verify** with the launcher smoke run (windowed 1280x720) and, with a temp folder holding `examples` from `mods-examples/`, look at a screenshot of the tab: list, gear, order arrows, native switch. Expected: matches the in-game manager's rows.
- [ ] **Step 4: Check the save path by hand** — toggle a mod, close, read `config.json`: `"mods"` holds the section and `game`, `graphics`, `launcher` keys are intact; then change a launcher setting and confirm `"mods"` is still there.

### Task 4: Deck Mods tab (rows, modals, translations)

**Files:**
- Create: `launcher/deck/mods_page.h`, `launcher/deck/mods_page.cpp`, `tests/test_deck_mods.cpp`
- Modify: `launcher/deck/app.h` (`enum class Tab { game, graphics, controls, mods }`, `kTabCount = 4`, `StandaloneMods`, `ModsUi` state), `launcher/deck/main_deck.cpp` (`kTabLabels/Titles/Hints`, sidebar tab drawing, `handle_main_keys` last-tab logic uses `kTabCount` already, `draw_tab` dispatch, modals next to the quit modal, snapshot script, `draw_sidebar` height budget `3 * 70 * unit` → `kTabCount * 70 * unit`), `launcher/deck/rows.h/.cpp` (a mod row), `launcher/CMakeLists.txt` (`uc_deck_mods` library linking `uc_deck_pages uc_mods`; `uc_launcher` Linux target links it), `launcher/translations.inc`, `tests/CMakeLists.txt`, `tests/linux/CMakeLists.txt` if Deck tests run there

**Interfaces:**
- Produces in `mods_page.h`:
  - `struct ModsUi { std::string settings_id, confirm_id; bool native_warning = false; int part = 0; }` — one instance in `App`.
  - `bool draw_mods(RowCtx& rows, StandaloneMods& mods, ModsUi& ui, std::string_view language)` — the tab body; returns true when something changed (the caller saves `mods.section()` into the document and the config). Draws, in order: caption/intro, toggle row "Allow native code" (turning it on sets `ui.native_warning`), one mod row per entry, conflicts note, "no mods" note when empty.
  - `bool draw_mod_settings(RowCtx& rows, StandaloneMods& mods, ModsUi& ui, std::string_view language)` — the settings modal body: rows for each declared setting of `ui.settings_id`: boolean → toggle row; integer/number → slider row (step from `step` or 1 / (max-min)/100); choice → choice row (opens the existing list modal); text → entry row that opens a text-entry modal (`SDL_StartTextInput`, ImGui `InputText`, A confirms, B cancels); a Close row.
  - Mod row parts (like the binding row's `RowCtx::sub`, 0..3): enable (A toggles; disabled for a broken manifest), gear (A opens settings; hidden when the mod declares no settings), up (A moves up), down (A moves down); left/right moves the part; a native mod whose hash is unconfirmed shows a Confirm part (A opens the hash modal, A confirms → `set_native_hash(id, dll_hash(id))`).
  - Text of a mod: `localized(names, name)` and `descriptions` per the manager's rule (language "ru" else English).
- Consumes: Task 1; `RowCtx`, `registry_row`-style helpers in `rows.h`, `FocusList` (see `launcher/deck/focus.h`), the list/choice modal and `button_row`/`note`/`caption` already used by Controls.

- [ ] **Step 1: Write `tests/test_deck_mods.cpp`** with a real `FocusList`/`RowCtx` and a temp mods folder (pattern of `test_deck_pages.cpp`): (a) `tab_count`: `kTabCount == 4`, `Tab::mods` is last; (b) `enable_with_a`: focus the first mod row, feed `accept`, `draw_mods` returns true and `mods.registry().wanted("a")`; (c) `parts`: `right` moves to gear, `accept` sets `ui.settings_id == "a"`; a mod without settings has no gear part (right goes to up); (d) `order`: part up + accept moves the mod up and returns true; first mod's up is a no-op, no change reported; (e) `native_needs_warning`: toggling "Allow native code" on only sets `ui.native_warning` and leaves `allow_native()` false until the modal's confirm; (f) `settings_rows`: `draw_mod_settings` for a mod with boolean/integer/choice/text declarations registers four enabled rows plus Close, slider `right` changes the integer by its step and clamps at max; (g) `empty`: no mods → one note row, no focusable mod rows.
- [ ] **Step 2: Run** `ctest --test-dir build -R test_deck_mods`; Expected: FAIL (missing header/target).
- [ ] **Step 3: Implement** `mods_page.*`, the row, the `App`/`main_deck.cpp` wiring (tab, modals: native warning with the existing warning text translated, hash confirm, settings, text entry), saving through the same `save_config(app, ...)` path the Controls tab uses but writing `document.properties["mods"]`, and the translation keys (every language).
- [ ] **Step 4: Run** `ctest --test-dir build -R "test_deck_(mods|pages|focus|nav_keys)|test_settings_registry"`; Expected: PASS. Then Linux cross build `pwsh scripts/linux/build.ps1 -Target uc_launcher` and run the Linux tests that exist for the Deck on the Deck as before.
- [ ] **Step 5: Snapshots** — add `shot("mods", ...)`, `shot("mod-settings", ...)`, `shot("mod-native-warning", ...)` to the snapshot script (temp mods folder with `mods-examples/*`; do not touch `~/uc-run/mods`), run `python scripts/linux/deck_snapshot.py`, read the PNGs; Expected: rows legible at the current scale, one selection highlight only, B on the tab opens the quit modal.

### Task 5: Docs, full build, deploy, review

**Files:**
- Modify: `docs/steamdeck-install.md` (Mods section: where mods go on the Deck, the tab), memory note `steamdeck-launcher.md` (done at the end of the work)

- [ ] **Step 1:** `build.bat` from PowerShell without `--no-install` (tests, stage, deploy to the game folder). Clean test leftovers from the staged folder first. Expected: all tests pass; report path and time.
- [ ] **Step 2:** Deploy the launcher files to the Deck's `~/uc-run` as in earlier rounds (package, tar over ssh, chmod, md5 check); leave `uc`, `config.json`, saves and `~/uc-run/mods` untouched; delete temp files.
- [ ] **Step 3:** Final whole-branch review on **Opus** (`model: "opus"`), then fix Critical/Important findings with a failing test first.
- [ ] **Step 4:** Tell the owner what is verified (Windows smoke screenshot, Deck snapshots, tests) and what is not (real gamepad/touch, Steam keyboard for text settings, a real game start with launcher-saved mods).

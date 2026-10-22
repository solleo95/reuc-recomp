# Mod platform design

Date: 2026-10-22. Status: draft for owner approval.

## Intent

Players download mods from the internet and install them into the port. The port already replaces textures from a `textures/` folder. This spec turns that into a general mod platform: a mod is a package that can replace files, patch data, run scripts (Lua, JavaScript) or native code (DLL), add tabs with settings to the in-game Insert menu, and read and change game state through a documented, versioned game API.

The first milestone proves the platform with gameplay mods the owner named: infinite ammo, invulnerability, pistol automatic fire while the fire button is held, and weapon parameter changes (damage, fire rate, magazine size, reload speed).

This spec covers the platform and the game API for the player and weapons only. Later specs (own brainstorm each): the rich game API (enemies, items, missions, HUD, audio and so on, grown by reverse engineering), PBR texture materials, asset mods (audio, video, text, meshes), tools (inspector, free camera, randomizer).

## Goals

1. A mod is a folder (or a zip of that folder) with a `mod.json` manifest. Installing means copying it into `mods/` next to the exe.
2. Mod manager in the Insert menu: list, enable/disable, load order, status (loaded, error, incompatible API version, conflicts), "restart required" markers.
3. File overlay: files of an enabled mod replace game disc files by path. Later mods in the load order win. Conflicts between mods are listed in the manager.
4. Texture replacement keeps working and becomes per-mod (`textures/` inside the mod folder, the existing `tex1_<WxH>_<xxh64>_<fmt>.dds` naming).
5. Data patches: a JSON file maps API field names to values (`"weapon.pistol.damage": 2.0`), applied whenever the object appears. No code needed.
6. Three script/code runtimes with identical power over the same API: Lua 5.4, JavaScript (QuickJS-ng), native DLL (C ABI).
7. One machine-readable API schema is the single source. A generator produces from it: bindings for Lua and JS, the C header for DLLs, the API reference docs, `uc.d.ts` for JS/TypeScript editors, and Lua stub files for lua-language-server. Every schema element has a status (`implemented`, `planned`, `unverified`) and a `since` API version.
8. Mods add tabs to the Insert menu. Two ways, usable together in one tab:
   - declarative: the mod lists settings in `mod.json` (bool, int, float, choice, text), the port draws them;
   - free drawing: the mod draws with a UI API (Lua and JS get wrapper functions, DLL gets the ImGui context).
9. Mod settings are saved with the port's normal settings, in the same config file, one section per mod, and restored on the next start.
10. The Insert menu tab bar scrolls (arrows) when the tabs do not fit the width. This applies to the whole menu, with or without mods.
11. Documentation under `docs/modding/`: getting started, manifest reference, settings and UI, Lua guide, JS guide, DLL guide, generated API reference, recipes for each example mod, safety notes, API changelog.
12. Example mods in the repo (`mods-examples/`), each documented: infinite ammo (Lua), god mode (JS), pistol autofire (Lua), damage multiplier with declarative settings only, hello-world native DLL with an ImGui tab.

## Non-goals

- Anything about speedruns, demo recording or replay sharing (owner dropped these).
- Netplay or co-op changes, an online mod browser or downloader, Workshop integration.
- Hot-swapping file overlays or DLLs while the game runs (scripts may reload; the rest needs a restart).
- Patching arbitrary binary game archives.
- Rich game API beyond player and weapons, PBR, audio/video/mesh replacement, inspector tools: separate specs.

## Anti-goals (failure even if it technically works)

- A faulty script or DLL callback takes the game down without a message, or freezes the game.
- Mods change or corrupt the owner's real saves unnoticed.
- The API reference drifts from the code.
- Measurable cost for players who run no mods (no hooks active, no runtime started).
- A downloaded Lua/JS mod can read or write files outside its folder, start processes or use the network.

## Constraints

- Windows, C++ with CMake; the project already depends on `nlohmann::json`, so manifest, settings and data patches use JSON (not TOML).
- The Insert menu is ImGui (`game/settings_menu.cpp`, `ImGui::BeginTabBar`). Settings are stored through `game/options_config.cpp` (ordered JSON).
- Guest memory is mapped 1:1 into the host process and big-endian helpers exist (`runtime/guest_mem.h`, `runtime/ppc_rt.h`). Game functions are statically recompiled and called directly, so a hook needs a hook point emitted at translation time.
- File overlay hook point: DVD path lookup in `runtime/hle/dvd.cpp` (`DVDConvertPathToEntrynum`, `DVDOpen`) and `DvdFs` (`runtime/dvdfs.h`).
- The game runs at a nominal step rate with decoupled step/render timing (see `docs/spikes/F5-step-rate-dependence.md`); script events must be tied to the game step, not to render frames.
- Testing never runs fullscreen and never touches real saves (`AGENTS.md`).
- Known game state is thin: about 15 documented offsets (`docs/spikes/G22-autoplay-memory.md`), no struct headers. Weapon parameters (damage, rate, reload, capacity) are not found yet; locating them is part of this spec's game API milestone.

## Decisions made with the owner

- Mods come from strangers on the internet: Lua and JS run in a sandbox, DLLs are allowed.
- DLL mods are allowed, with full native power and no sandbox.
- Mods can add Insert-menu tabs; both declarative settings and free drawing are supported; settings persist like normal settings.
- Menu tab bar scrolls when tabs overflow.
- JavaScript is supported next to Lua, via QuickJS-ng.
- The game API is to be as rich as possible and thoroughly documented; this spec starts with player and weapons.
- Mod platform first, then game API content, PBR and others as separate specs.

## Design

### Package layout

```
mods/<id>/mod.json
mods/<id>/files/            overlay of disc files, same relative paths as the disc root
mods/<id>/textures/         texture replacements
mods/<id>/data/*.json       data patches
mods/<id>/scripts/          Lua and JS sources
mods/<id>/native/*.dll      native mod
```

`mod.json`: `id` (unique, lowercase), `name`, `version`, `author`, `description`, `api` (required API version range), `game` (supported game versions), `depends` (mod ids and ranges), `touches_gameplay` (bool), `scripts` (entry file, language by extension), `native` (dll file), `settings` (declarative list), plus localized names/labels keyed by language code.

All paths inside a mod are normalised and confined to the mod folder (no `..`, no absolute paths, zip entries are checked against zip-slip). Overlay paths match the disc case-insensitively.

### Loading and order

The enabled mods and their order are saved in the port config. Order decides overlay and data-patch conflicts (later wins). Changes to overlay files, textures and DLLs apply on the next start; script mods can be reloaded from the manager. The manager shows "restart required" where it applies.

Texture lookup must let a later mod's texture win over an earlier one and over the base `textures/` folder. (The existing index keeps the first entry for a key; the builder adjusts this.)

### Runtimes

- Lua 5.4: standard base, string, table, math, utf8 libraries only. No `io`, `os`, `package`, `debug`, `dofile`, `loadfile`, no `load` of external files. `require` resolves only inside the mod folder.
- QuickJS-ng: no `std` or `os` modules; only the globals the port provides; modules (`import`) resolve only inside the mod folder.
- Native DLL: C ABI header `uc_mod.h`, exports `uc_mod_init(const UcApi*)`, `uc_mod_shutdown()`. `UcApi` is a versioned function table plus the ImGui context and allocator pointers. The DLL must be built with the ImGui version recorded in the header; a mismatch refuses the load with a message.
- Safety for script runtimes: an instruction/time budget per callback (QuickJS interrupt handler, Lua count hook). A callback that exceeds it or throws is reported (log and manager); after repeated failures the mod is disabled for the session. Script errors never crash the game.
- Native DLLs: before the first load of a DLL the manager shows a warning that it contains native code that can do anything on the computer, and asks for confirmation. The DLL hash is stored; a changed hash asks again. A crash inside a DLL cannot be isolated; this is stated in the docs.

### Events and threading

Events: `on_load`, `on_unload`, `on_step` (once per game step at the nominal rate), `on_settings_changed`, plus game events the API defines (for this spec: damage taken, weapon fired, reload started/finished). Script and DLL callbacks run on the thread that runs the guest code, at a defined point of the step. Menu drawing callbacks run on the UI thread; shared state between the two is marshalled by the platform. The builder determines the actual threads in the code.

### API schema and generator

`api/uc_api.json` (name and location the builder may change) describes domains, types, functions, properties (with access: read, write, read-write), events, constants, status, `since`, docs text, and an example. A Python generator (the repo already has Python tooling) emits bindings, the C header, `docs/modding/api/*.md`, `uc.d.ts`, Lua stubs. CI/test fails if generated files are out of date. The API version is semver: minor adds, major breaks. A mod declaring an incompatible range is not loaded and the manager says why.

### Low-level API

- `mem`: read/write u8/u16/u32/s8/s16/s32/f32/f64/bytes at guest addresses or symbol names (big-endian handled by the platform). Writes outside mapped guest memory are rejected with an error, not a crash.
- `symbols`: lookup of names known to the project (`project/symbols.txt`, game-named entries).
- `hook`: pre/post/replace callbacks on functions. **My call:** only on a curated set of hookable functions marked in the schema (extended per release), because a hook point on every one of ~16k recompiled functions costs speed for everyone and the project goal is performance. When no mod uses a hook point the cost must be zero or negligible (the builder picks the mechanism and measures with the deterministic bench).

### High-level game API v1 (player and weapons)

Domains and fields target what the examples need, each with a status:
- `player`: health/damage state, invulnerability toggle, position, magazine and reserve ammo, grenades, current weapon.
- `weapon`: per weapon type: damage, fire rate, magazine capacity, reload time, fire mode (semi/auto).
- `input`: read the fire button; a mod can mark "held fire fires repeatedly" for a weapon via the weapon fire mode, not by faking input.
- `camera`: view matrix (read only).
- `enemies`: list with position and health (read only, zombies verified).
- `events` as above.

Known starting points: actor list `804421C4`, player magazine `+4D4`, damage in scene `+6C00`, camera `8044B4E0`, enemy hp `+3A4` (zombies), weapon update `GameWeaponUpdate 8006C268`. Weapon damage, rate, reload and capacity tables are unknown: finding them is a task of this milestone. If one of them proves infeasible, that field stays `planned` and its example mod is reduced; this is reported to the owner.

### Mod manager and menu

A built-in "Mods" tab after the built-in tabs. Each enabled mod with UI gets its own tab after it, in load order. Tab bar uses `ImGuiTabBarFlags_FittingPolicyScroll`.

Declarative setting: `key`, `type` (`bool`, `int`, `float`, `choice`, `text`), `label` (localizable), `default`, `min`/`max`/`step` for numbers, `choices` for choice, `tooltip`, `restart_required`. The port renders it and saves it under the config section `mods.<id>.settings.<key>`. Scripts read it via `settings.get(key)`; `on_settings_changed` fires on change. Invalid stored values fall back to defaults with a log line. Removing a mod leaves its section in the config untouched.

Free drawing UI API (Lua/JS wrappers): text, button, checkbox, slider (int/float), combo, input text, separator, same-line, collapsing header, tooltip, color edit. The DLL uses ImGui directly.

### Saves

**My call:** mods whose `touches_gameplay` is true switch the game to a separate "modded" save directory, created by copying the vanilla saves on first use. Cosmetic-only mods (textures, files, UI) keep the vanilla saves. The harness already copies saves, so tests are unaffected.

### Testing

- Unit tests: manifest parsing and validation, load order and overlay precedence, path confinement and zip-slip, sandbox escapes (`io`, `os`, `package`, `debug`, JS modules), settings declaration/persistence/invalid values, budget and error handling, generator output (golden files and a staleness check).
- Menu tests next to the existing `test_settings_menu` ones: scroll policy, mod tabs, persistence.
- Harness scenario that loads the example mods and checks state through the test server (`peek`, `state`), at the forced 1280x720 window.
- Performance: deterministic bench with and without the hook mechanism and with no mods enabled; no regression beyond noise.

### Calls made after the builder check (my calls, owner may change)

- **Hooks ship in the normal `uc.exe`.** The curated hook points are always emitted: each is one predictable branch on a global pointer that is null when no mod hooks it. No second "mod-capable" binary. Growing the curated set means retranslation in a normal release build. Verify with the deterministic bench that the cost is within noise.
- **Milestone acceptance.** The platform ships regardless of the reverse-engineering progress; the reverse engineering of weapon data runs as a parallel track. Must-have fields: infinite ammo and god mode (offsets already known, `--god` does the same). Pistol autofire and the weapon stat fields (damage, fire rate, capacity, reload time) are best-effort: each field is time-boxed to about two reverse-engineering approaches; when it does not work it stays `planned` in the schema and its example mod is dropped or reduced, and this is reported to the owner.
- **ImGui for DLLs.** The port links ImGui 1.92.9b statically (`uc_imgui`, `launcher/CMakeLists.txt`). A DLL builds its own copy of that exact ImGui version and receives the context and the allocator functions through `UcApi` (`SetCurrentContext`, `SetAllocatorFunctions`). `uc_mod.h` records the ImGui version; a mismatch refuses the load.
- **Saves.** The switch is global: any enabled mod with `touches_gameplay` moves the whole game to the "modded" save directory. The modded directory is copied from the vanilla saves once, on first use, and then diverges; it is never refreshed automatically. Disabling all gameplay mods returns to the vanilla saves, and the modded directory stays on disk for the next time. The manager warns before the first switch. The redirect happens at startup, so it follows the same "restart required" rule.
- **Script budgets.** Defaults: `on_step` callbacks of all scripts together get 1 ms per step, a UI callback 4 ms; a mod that exceeds a budget or throws 3 times in a session is disabled for the session. The values are config constants.
- **Native mods switch.** A global "Allow native (DLL) mods" setting, default off, in the manager. With it off the loader runs the sandboxed runtimes only. Turning it on shows the warning once; each DLL still needs its own confirmation tied to its hash. A zip with both scripts and a DLL installs the same way as any mod; its DLL stays inactive until confirmed.
- **File overlay scope.** The overlay replaces files at disc-path level. If the game keeps assets inside packed archives that it parses itself, such an archive can be replaced only as a whole file; editing inside archives is not part of this spec. The `files` documentation says so, and the builder reports which archives the game uses.

### Left to the builder

Choice of exact C++ binding technique, schema file location and format details, the hook mechanism inside the translator, thread handling, zip loading details, exact menu look, error-message wording, localisation keys.

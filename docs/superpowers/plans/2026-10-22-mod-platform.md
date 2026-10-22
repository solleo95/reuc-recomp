# Mod Platform Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Players install mods (file overlay, texture packs, data patches, Lua / JavaScript / native DLL code, Insert-menu tabs with persistent settings) that read and change game state through one documented, versioned game API; the first API covers the player and weapons.

**Architecture:** A new static library `uc_mods` (directory `modding/`) holds manifest parsing, the registry, overlay, settings, the API registry, the three runtimes and the host that fires events. The game wires it in `uc_main.cpp` (overlay before the DVD session, textures per mod, host events from the game step and from the menu). One API schema (`modding/api/uc_api.json`) is the single source for the docs, `uc.d.ts`, Lua stubs, the hookable-function list and a consistency test against the C++ registrations. Runtimes bind the API generically through the registry (`path` strings), not per-member generated glue.

**Tech Stack:** C++20 (MSVC/CMake, Windows), nlohmann::json (already a dependency), ImGui 1.92.9b (already linked statically as `uc_imgui`), Lua 5.4, QuickJS-ng, miniz (zip), Windows CNG for SHA-256, Python 3 for the generator.

**Spec:** `docs/superpowers/specs/2026-10-22-mod-platform-design.md` (read it first; "Calls made after the builder check" are binding).

## Global Constraints

- Windows only. Config is JSON (`ConfigDocument.properties`, ordered). Manifest, data patches and settings are JSON, never TOML.
- No commits unless the owner asks (global rule); each task ends with a verification run instead of a commit. Stage explicit paths if asked (`git add tests` also stages the owner's `tests/test_uctest_autoplay.py`).
- Edit source only with Edit/Write (never heredoc/sed/python patches); after any edit containing `\n`-style escapes, check the changed line before the first build.
- Build: `build.bat --no-install`. Tests: `ctest --test-dir build --output-on-failure [-R name]`. Tests use `check.h` `CHECK(...)`, one executable per test, registered with `add_executable` + `add_test` in `tests/CMakeLists.txt`.
- Never run any game, test, bench or demo fullscreen; harness forces 1280x720 (`docs/testing/test-server.md`). Never run the game against real saves. Performance claims only from `python -m uctest bench <A> <B> --demo fight2` (deterministic), never one FPS run.
- Third-party code via `FetchContent` with `URL` + `URL_HASH SHA256=` and a local `build/deps/<archive>` override, exactly like imgui in `launcher/CMakeLists.txt:13-21`. Pin release tags; compute the SHA256 on first download and record it. Licence files go to `licenses/<name>` (see `install(FILES ... licenses/imgui)`) and `THIRD_PARTY_NOTICES.md`.
- `mod.json` ids: regex `^[a-z0-9][a-z0-9_-]{0,47}$`; reserved ids (config keys): `enabled`, `allow_native`, `native_hash`.
- API version `1.0` (semver; a mod's `"api": "1.0"` means minimum 1.0, same major).
- Budgets (config constants): all `on_step` callbacks together 1 ms per step; one UI callback 4 ms; a mod that exceeds a budget or throws 3 times in a session is disabled for the session. Memory cap per script runtime: 64 MiB.
- Mods paths: normalised and confined to the mod folder; overlay paths match the disc case-insensitively.
- No cost without mods: no runtime started, hook branches only at curated functions and null-checked.
- Windows paths may be non-ASCII: read files through `std::filesystem::path` (wide) and pass contents to Lua/QuickJS as buffers, never as narrow path strings.

## Review Focus

1. One broken `mod.json` (invalid JSON, wrong types, duplicate id) must not stop the other mods; it shows as an error entry with the reason (Task 2, 3, 5).
2. Dependency problems: missing dependency, dependency cycle, dependency disabled: the dependent is not loaded and says why; no crash, no infinite loop (Task 5).
3. A hand-edited config with wrong types or out-of-range values for a mod setting falls back to the default (clamped for ranges) and logs one line (Task 7).
4. A runaway script (endless loop, huge allocation, deep recursion) is stopped by the budget or memory cap and disables only that mod; the game keeps stepping (Task 10, 11, 13).
5. The game folder under a non-ASCII path (e.g. Cyrillic user name) loads mods and DLLs (Task 3, 10, 11, 12).

---

## File Structure

New directory `modding/` (library `uc_mods`, `modding/CMakeLists.txt`, added in the root `CMakeLists.txt` after `add_subdirectory(renderer)` and before `add_subdirectory(tests)`):

| File | Responsibility |
|---|---|
| `modding/manifest.{h,cpp}` | `ModManifest`, `parse_manifest` |
| `modding/mod_paths.{h,cpp}` | path confinement, zip extraction (miniz) |
| `modding/registry.{h,cpp}` | discovery, enable/order, state, conflicts, config section |
| `modding/overlay.{h,cpp}` | texture directory order for mods (DvdFs overlay lives in `runtime/dvdfs.*`) |
| `modding/settings.{h,cpp}` | `ModSettings` declarative values, validation, persistence JSON |
| `modding/api_registry.{h,cpp}` | `ApiRegistry`, `Member`, value model (`nlohmann::json`) |
| `modding/game_api.{h,cpp}` | guest memory access with bounds checks, player/weapon members |
| `modding/runtime.h` | `ModRuntime` interface, `Budget` |
| `modding/runtime_lua.{h,cpp}`, `runtime_js.{h,cpp}`, `runtime_native.{h,cpp}` | the three runtimes |
| `modding/uc_mod.h` | public C header for DLL authors (hand-written) |
| `modding/host.{h,cpp}` | `ModHost`: lifecycle, events, budgets, disable policy |
| `modding/hooks.h` | `hook_enter<A>` used by `UC_ENTER`; generated `modding/generated/hookable.inc` |
| `modding/menu_bridge.h`, `modding/menu_ui.{h,cpp}` | `ModMenu` interface and the ImGui implementation (manager tab, mod tabs) |
| `modding/profile.{h,cpp}` | modded save-root resolution |
| `modding/api/uc_api.json` | API schema |
| `scripts/gen_mod_api.py` | generator + `--check` |
| `docs/modding/**`, `mods-examples/**` | docs and example mods |
| `tests/test_mod_*.cpp` | one test per unit |

Modified: `runtime/dvdfs.{h,cpp}` / `runtime/hle/dvd.cpp` (overlay), `renderer/texture_replacement.{h,cpp}` (override order), `game/settings_menu.{h,cpp}` (scroll, mod tabs), `game/live_settings.{h,cpp}` (mods section), `game/uc_main.cpp` (wiring), `game/entry_probe.h` (hook point), `game/game_pacing.cpp` (step event), `game/CMakeLists.txt`, `tests/CMakeLists.txt`.

---

### Task 1: Scrolling tab bar

**Files:**
- Modify: `game/settings_menu.cpp:64`, `game/settings_menu.h`
- Test: `tests/test_settings_menu.cpp`

**Interfaces:**
- Produces: `inline constexpr int kMenuTabBarFlags` in `game/settings_menu.h` (= `ImGuiTabBarFlags_FittingPolicyScroll`); the menu's tab bar uses it.

- [ ] **Step 1: Write the failing test** in `tests/test_settings_menu.cpp` `exercise()`: after the existing `tabs->Tabs.Size==4` check add `CHECK(tabs->Flags & ImGuiTabBarFlags_FittingPolicyScroll);`. Add a second check by pushing the window width down: shrink `ImGui::GetIO().DisplaySize.x` so the four tabs cannot fit and assert `tabs->WidthAllTabs > tabs->BarRect.GetWidth()` while every tab stays reachable (`tabs->ScrollingTargetDistToVisibility` computed via `ImGui::TabBarScrollToTab`, or simply that `tabs->ScrollingAnim` / scroll buttons exist). Use whichever internal fields compile with the vendored `imgui_internal.h`.
- [ ] **Step 2: Run** `ctest --test-dir build -R test_settings_menu --output-on-failure`. Expected: FAIL on the flag check.
- [ ] **Step 3: Implement** `ImGui::BeginTabBar("tabs", kMenuTabBarFlags)` at `game/settings_menu.cpp:64`.
- [ ] **Step 4: Run** the same test. Expected: PASS (existing `Tabs[3].Offset+Width<=BarRect width` check stays valid at the normal width).

---

### Task 2: Manifest parsing

**Files:**
- Create: `modding/CMakeLists.txt`, `modding/manifest.h`, `modding/manifest.cpp`
- Modify: root `CMakeLists.txt` (add `add_subdirectory(modding)`), `tests/CMakeLists.txt`
- Test: `tests/test_mod_manifest.cpp`

**Interfaces:**
- Produces (namespace `uc::mods`):
```cpp
enum class SettingType { boolean, integer, number, choice, text };
struct SettingDecl { std::string key; SettingType type; nlohmann::json default_value;
  std::optional<double> min, max, step; std::vector<std::string> choices;
  std::map<std::string,std::string> label, tooltip;   // language code -> text; "en" required for label
  bool restart_required=false; };
struct ModDependency { std::string id; std::string min_version; };
struct ApiVersion { int major=0, minor=0; };
struct ModManifest { std::string id, name, version, author, description; ApiVersion api;
  std::vector<std::string> game; std::vector<ModDependency> depends; bool touches_gameplay=false;
  std::string script, native; std::vector<SettingDecl> settings;
  std::map<std::string,std::string> names; };   // localized display names
struct ManifestResult { std::optional<ModManifest> manifest; std::vector<std::string> errors; };
ManifestResult parse_manifest(std::string_view json_text);
```
- `uc_mods` target links `nlohmann_json::nlohmann_json`, `uc_runtime`.

- [ ] **Step 1: Write failing tests** (`test_mod_manifest.cpp`): `valid_minimal` (id/name/version/api only → manifest set, no errors); `bad_id` (`"Bad Id"`, reserved `"enabled"`, 49 chars → errors, no manifest); `invalid_json` (garbage text → one error, no throw); `api_format` (`"api":"1.0"` → `{1,0}`, `"1"` and `"x"` → error); `settings_types` (bool/int/number/choice/text each parse; default of wrong type → error naming the key; `min>max` → error; choice default not in `choices` → error; missing `label.en` → error; duplicate key → error); `script_and_native_paths` (`"script":"scripts/main.lua"` ok; `"../x.lua"`, absolute and `C:\` paths → error); `script_extension` (only `.lua`, `.js`, `.mjs`; others error).
- [ ] **Step 2: Run** `ctest -R test_mod_manifest`. Expected: FAIL (does not build).
- [ ] **Step 3: Implement** `parse_manifest` in `modding/manifest.cpp`. All errors are collected (no early return), messages name the offending field. Never throws.
- [ ] **Step 4: Run** the test. Expected: PASS.

---

### Task 3: Path confinement and discovery

**Files:**
- Create: `modding/mod_paths.h`, `modding/mod_paths.cpp`
- Test: `tests/test_mod_paths.cpp`

**Interfaces:**
- Produces:
```cpp
std::optional<std::filesystem::path> confine(const std::filesystem::path& root, std::string_view relative);   // nullopt for "..", absolute, drive, UNC, reparse escapes
struct DiscoveredMod { std::filesystem::path root; std::string manifest_text; std::string folder_name; };
std::vector<DiscoveredMod> find_mod_folders(const std::filesystem::path& mods_dir);   // direct children holding mod.json; missing dir -> empty
```

- [ ] **Step 1: Write failing tests**: `confine_ok` (`scripts/main.lua` under a temp root → path inside root); `confine_rejects` (`../a`, `a/../../b`, `C:\x`, `\\server\share\x`, `/x`, a name with `:` stream syntax → nullopt); `confine_symlink` (a directory junction inside the root pointing outside → nullopt; skip with a message if the junction cannot be created); `find_folders` (temp dir with `a/mod.json`, `b/` without manifest, a file `c` → only `a`; unreadable manifest skipped with the folder name still reported by a separate `errors` out-list is not needed: unreadable manifest yields `manifest_text` empty and Task 5 reports it); `non_ascii_root` (root named with Cyrillic letters works for both functions).
- [ ] **Step 2: Run** `ctest -R test_mod_paths`. Expected: FAIL.
- [ ] **Step 3: Implement** with `std::filesystem::weakly_canonical` on both sides and a prefix check on path components (not string prefix).
- [ ] **Step 4: Run.** Expected: PASS.

---

### Task 4: Zip packages

**Files:**
- Modify: `modding/CMakeLists.txt` (FetchContent miniz 3.0.2), `modding/mod_paths.{h,cpp}`
- Test: `tests/test_mod_paths.cpp` (extend)

**Interfaces:**
- Produces: `bool extract_zip_mod(const std::filesystem::path& zip, const std::filesystem::path& cache_dir, std::string& error)`; extracts to `cache_dir/<zip stem>-<8 hex of zip size+mtime hash>/`, skips if already extracted; returns the folder via `std::filesystem::path extracted_mod_root(...)` not needed: `find_mod_folders` also scans `*.zip` in `mods_dir`, extracts them under `<mods_dir>/.cache/` and reports the extracted root. A zip with a single top-level folder containing `mod.json` is unwrapped.

- [ ] **Step 1: Write failing tests** (build zips in the test with miniz's writer): `zip_extract_ok`; `zip_slip` (entry `../evil.txt` and `..\evil.txt` and absolute entry → extraction refused, nothing written outside the cache, error text names the entry); `zip_single_folder_unwrapped`; `zip_too_big` (declared uncompressed size > 512 MiB or > 20000 entries → refused: zip-bomb guard); `zip_reextract_cached` (second call does no work).
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** using miniz; every entry name goes through `confine`.
- [ ] **Step 4: Run.** Expected: PASS.

---

### Task 5: Registry (discovery, enable/order, state, conflicts, config)

**Files:**
- Create: `modding/registry.h`, `modding/registry.cpp`
- Test: `tests/test_mod_registry.cpp`

**Interfaces:**
- Consumes: `parse_manifest`, `find_mod_folders` (Tasks 2-4).
- Produces:
```cpp
enum class ModState { disabled, enabled, error, incompatible, missing_dependency, dependency_disabled };
struct ModEntry { ModManifest manifest; std::filesystem::path root; ModState state; std::vector<std::string> messages; std::string folder_name; };
struct Conflict { std::string path; std::vector<std::string> mod_ids; };   // overlay path lower-case -> mods in load order
class ModRegistry {
 public:
  static ModRegistry discover(const std::filesystem::path& mods_dir, const nlohmann::ordered_json& mods_section, ApiVersion host_api, std::string_view game_version);
  const std::vector<ModEntry>& entries() const;           // load order: saved order first, new mods appended alphabetically
  std::vector<const ModEntry*> active() const;            // state == enabled, dependency order within load order
  bool set_enabled(std::string_view id, bool on);
  bool move(std::string_view id, int delta);              // reorders the saved order; false at the ends
  std::vector<Conflict> conflicts() const;                // files/ and textures/ paths provided by 2+ active mods
  nlohmann::ordered_json to_config() const;               // {"enabled":[ids in order],"allow_native":bool,"native_hash":{id:hex}} merged over the input section, other keys (per-mod sections) untouched
  bool allow_native() const; void set_allow_native(bool);
};
```
Config shape: `mods: { enabled:[...], allow_native:false, native_hash:{<id>:"<sha256 hex>"}, <id>:{settings:{...}} }`.

- [ ] **Step 1: Write failing tests**: `discover_mixed` (one valid, one with broken JSON, one duplicate id → valid enabled-if-listed, broken has `error` with the parse message, the duplicate second occurrence `error` "duplicate id"); `enabled_list_respected` (only ids in `enabled` are `enabled`; unknown ids in the list ignored); `order_follows_saved_list`; `incompatible_api` (mod needs `2.0`, host `1.3` → `incompatible`, and `1.5` required on host `1.3` → `incompatible`); `incompatible_game` (non-empty `game` list without the host's version); `dependency_missing` / `dependency_disabled` / `dependency_cycle` (a<->b both `error` "cycle", no hang) / `dependency_min_version`; `dependency_order` (b depends on a, saved order b,a → `active()` returns a,b); `conflicts` (two mods both provide `files/DATA/x.bin` in different case → one Conflict with both ids in load order; textures likewise); `to_config_roundtrip` (set_enabled/move/set_allow_native then `to_config()` then `discover` again gives the same state; per-mod `settings` sections survive); `reserved_keys_not_mods` (a folder whose manifest id is `enabled` is an error).
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** Dependency resolution by topological sort over active candidates; compare versions as dotted integers.
- [ ] **Step 4: Run.** Expected: PASS.

---

### Task 6: File overlay and texture order

**Files:**
- Modify: `runtime/dvdfs.h`, `runtime/dvdfs.cpp` (find where `open_root`/`walk` are defined with Grep), `renderer/texture_replacement.h`, `renderer/texture_replacement.cpp:60` (`load_index`)
- Create: `modding/overlay.h`, `modding/overlay.cpp`
- Test: `tests/test_dvdfs_overlay.cpp`, extend `tests/test_texture_replacement.cpp`

**Interfaces:**
- Produces: `size_t DvdFs::overlay(const std::filesystem::path& files_root)` — for every file under `files_root`: if `index_` (lower-case relative path) has it, replace that entry's `full` and `size`; otherwise append a new entry (and missing parent directory entries). Returns files replaced+added. Entry numbers of existing entries never change. Call only before `dvd_hle_init`.
- `size_t TextureReplacements::load_index(const std::filesystem::path& dir, bool replace_existing = false)`.
- `size_t apply_mod_overlays(DvdFs&, const std::vector<const ModEntry*>& active)` and `size_t load_mod_textures(gx::TextureReplacements&, const std::vector<const ModEntry*>& active)` in `modding/overlay.h`: iterate active mods in load order; later mods win.

- [ ] **Step 1: Write failing tests**: `overlay_replaces_file` (temp disc with `DATA/a.bin` 3 bytes; mod `files/data/A.BIN` 5 bytes → `entry_of("DATA/a.bin")` unchanged number, `length` 5, `read` returns the mod bytes); `overlay_adds_file_and_dirs` (new `DATA/new/b.bin` reachable, parents are directories); `overlay_order` (two overlays, second wins); `overlay_ignores_escapes` (a symlink/junction out of `files/` skipped); `texture_later_wins` (same key in base dir and two mod dirs → `find` returns the last mod's path); `texture_default_first_wins` (old behaviour kept when `replace_existing=false`).
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** as described in Interfaces.
- [ ] **Step 4: Run** the new tests and `test_texture_replacement`. Expected: PASS.

---

### Task 7: Declarative settings store

**Files:**
- Create: `modding/settings.h`, `modding/settings.cpp`
- Test: `tests/test_mod_settings.cpp`

**Interfaces:**
- Consumes: `ModManifest::settings`.
- Produces:
```cpp
class ModSettings {
 public:
  ModSettings(const std::vector<SettingDecl>& decls, const nlohmann::json& stored);   // invalid/missing -> default, one warning per bad key
  const nlohmann::json& get(std::string_view key) const;                              // null json for an unknown key
  bool set(std::string_view key, const nlohmann::json& value);                         // validates type; numbers clamped to min/max and snapped to step; false + no change on type/choice error
  nlohmann::json to_json() const;                                                      // all keys with current values
  const std::vector<std::string>& warnings() const;
  bool dirty() const; void clear_dirty();
};
```

- [ ] **Step 1: Write failing tests**: `defaults_when_empty`; `stored_values_used`; `wrong_type_falls_back` (stored string for a bool key → default and 1 warning naming the key); `clamp_on_load` (stored 500 for max 100 → 100); `int_snaps_to_step`; `choice_must_be_listed` (stored unknown → default; `set` unknown → false); `unknown_stored_keys_ignored_but_not_dropped` (to_json keeps declared keys only, documented); `set_marks_dirty`; `text_length_cap` (strings over 1024 bytes rejected).
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run.** Expected: PASS.

---

### Task 8: API schema and generator

**Files:**
- Create: `modding/api/uc_api.json`, `scripts/gen_mod_api.py`, `modding/generated/hookable.inc`, `docs/modding/api/*.md` (generated), `docs/modding/uc.d.ts` (generated), `docs/modding/lua/uc.lua` (generated stubs)
- Modify: `tests/CMakeLists.txt` (`add_test(NAME test_mod_api_generated COMMAND python ${CMAKE_SOURCE_DIR}/scripts/gen_mod_api.py --check)`)

**Interfaces:**
- Schema (`uc_api.json`): `{ "version":"1.0", "domains":[{ "name":"player", "doc":"...", "scope":"slot", "members":[{ "name":"magazine", "kind":"property|function|event", "type":"int|float|bool|string|vec3|array<...>|...", "access":"r|w|rw", "status":"implemented|planned|unverified", "since":"1.0", "doc":"...", "example":{"lua":"...","js":"..."}, "args":[{"name","type","doc"}] }] }], "hookable":[{"name":"GameWeaponUpdate","address":"8006C268","doc":"..."}] }`. Domain `scope:"slot"` means members take a player slot (1 or 2), `scope:"id"` an entity id, absent means global. Event members carry `"payload"` fields.
- Generated outputs: per-domain Markdown with a status column, `uc.d.ts` (global `uc` namespace; player access as `uc.player(slot)`), Lua stub file (`---@class` annotations for lua-language-server), `modding/generated/hookable.inc` (`X(0x8006C268u)` lines).
- `python scripts/gen_mod_api.py` writes; `--check` exits 1 when any generated file differs.

- [ ] **Step 1: Write the schema** with domains `player`, `weapon`, `input`, `camera`, `enemies`, `events`, `mem`, `symbols`, `hook`, `settings`, `ui`, `log`, and every member from the spec's "High-level game API v1" and "Low-level API" sections, honest status per member (known offsets `implemented` once Task 14 lands; unknown weapon fields `planned`). Include one lua and one js example per implemented member.
- [ ] **Step 2: Write the generator** (stdlib only). Failing check first: run `python scripts/gen_mod_api.py --check` before the outputs exist. Expected: exit 1.
- [ ] **Step 3: Run** `python scripts/gen_mod_api.py`, then `--check`. Expected: exit 0.
- [ ] **Step 4: Schema validation inside the generator**: unique names per domain, known `kind/access/status`, `since <= version`, every `implemented` member has an example, `hookable` addresses are 8-hex-digit and unique. Add a test script case `scripts/test_gen_mod_api.py` (feeds a broken schema from a temp file via `--schema`, expects exit 1 and the message) and register it with `add_test`.

---

### Task 9: API registry and guest memory access

**Files:**
- Create: `modding/api_registry.h`, `modding/api_registry.cpp`, `modding/game_api.h`, `modding/game_api.cpp`
- Test: `tests/test_mod_api.cpp`

**Interfaces:**
- Produces:
```cpp
using Value = nlohmann::json;
struct Member { std::string path;                                   // "player.magazine", "mem.read_u32"
  std::function<Value(const Value& args)> get;                      // property read (args: {"slot":1} or {"id":n} or {})
  std::function<void(const Value& args, const Value& v)> set;       // property write
  std::function<Value(const Value& args)> call; };                  // function
class ApiRegistry { public: void add(Member); const Member* find(std::string_view path) const; std::vector<std::string> paths() const; };
class GuestMemory {                                                  // wraps the guest base pointer
 public:
  explicit GuestMemory(uint8_t* base);
  std::optional<uint32_t> read_u32(uint32_t addr) const;             // big-endian guest value; nullopt when [addr,addr+4) is outside MEM1 (0x80000000..0x81800000) or MEM2 (0x90000000..0x94000000), including a range that straddles the end
  bool write_u32(uint32_t addr, uint32_t v); // + u8/u16/s8/s16/s32/f32/f64/bytes variants with the same bounds rule
};
void register_low_level_api(ApiRegistry&, GuestMemory&, const SymbolTable&);   // mem.*, symbols.lookup
```
`SymbolTable`: reuse `uc::symbols_load` data (see `game/uc_main.cpp:274`; find its header with Grep and use that lookup, no new loader).
- Consistency: `register_*` functions register exactly the members the schema marks `implemented`, nothing else.

- [ ] **Step 1: Write failing tests**: `read_write_big_endian` (buffer, `write_u32(0x80000010,0x11223344)` → bytes `11 22 33 44`, `read_u32` back); `bounds` (below 0x80000000, `0x817FFFFD` read_u32 → nullopt, `0x817FFFFC` ok, `0x93FFFFFD` nullopt, wrap-around `0xFFFFFFFE` nullopt, unaligned address OK); `float_roundtrip` (`write_f32`/`read_f32`); `api_mem_read_rejects` (calling `mem.read_u32` with an out-of-range address throws `ApiError` with a message, never crashes); `symbols_lookup`; `schema_consistency` (loads `modding/api/uc_api.json` path passed as `UC_API_SCHEMA` definition; every `implemented` member path is registered after all `register_*` calls and no extra path is registered).
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** `ApiError` is `std::runtime_error` caught at runtime boundaries.
- [ ] **Step 4: Run.** Expected: PASS (`schema_consistency` only checks members implemented so far; Task 14 re-runs it).

---

### Task 10: Lua runtime

**Files:**
- Create: `modding/runtime.h`, `modding/runtime_lua.h`, `modding/runtime_lua.cpp`
- Modify: `modding/CMakeLists.txt` (FetchContent Lua 5.4.x, build as static C library)
- Test: `tests/test_mod_lua.cpp`

**Interfaces:**
- Produces (`runtime.h`):
```cpp
struct Budget { std::chrono::microseconds limit; };
struct CallResult { bool ok; std::string error; bool budget_exceeded=false; };
class ModRuntime {
 public:
  virtual ~ModRuntime() = default;
  virtual CallResult load(const std::filesystem::path& mod_root, const std::string& entry_relative) = 0;
  virtual CallResult fire(std::string_view event, const Value& payload, Budget) = 0;   // on_load, on_unload, on_step, on_settings_changed, game events
  virtual CallResult draw_ui(Budget) = 0;                                              // mod tab free-draw callback (Task 15), no-op when the mod has none
  virtual std::string name() const = 0;
};
std::unique_ptr<ModRuntime> make_lua_runtime(ModContext&);
```
`ModContext` (in `runtime.h`): `{ ApiRegistry& api; ModSettings& settings; std::string mod_id; std::function<void(std::string_view)> log; UiApi& ui; }` where `UiApi` is the abstract widget interface from Task 15 (declare the header here with the full method list: `text, button, checkbox, slider_int, slider_float, combo, input_text, separator, same_line, collapsing_header, tooltip, color_edit`; Task 15 implements it over ImGui; tests use a recording fake).
- Lua environment: libs `base` (minus `dofile`, `loadfile`, `load`, `loadstring`, `collectgarbage` except `"count"`), `string`, `table`, `math`, `utf8`; `require` loads only `<mod>/scripts/**.lua` through `confine`; globals `uc` (domains from the registry; `uc.player(slot)` returns a proxy with property get/set through `Member`s), `settings.get(key)`, `log(...)`, `on(event, fn)`, `ui` (UiApi). Entry file read through `std::filesystem::path` into a buffer, loaded with `luaL_loadbufferx` in text mode.
- Budget: `lua_sethook` count hook (every 10 000 instructions) checking a deadline; memory cap 64 MiB through the allocator passed to `lua_newstate`.

- [ ] **Step 1: Write failing tests**: `sandbox_missing_libs` (`io`, `os`, `package`, `debug`, `dofile`, `loadfile`, `load`, `string.dump` all nil/absent; script that touches them errors without effect); `require_confined` (`require("lib.util")` inside the mod works; `require("../other")` and `require("C:/x")` fail); `non_ascii_mod_root`; `events` (script registers `on("on_step", fn)`; `fire("on_step", {})` runs it exactly once; unknown event is ignored); `api_read_write` (fake registry member `player.magazine`; `uc.player(1).magazine = 7` calls `set` with `{"slot":1}`; reading returns the getter's value); `api_error_is_lua_error` (member throws `ApiError` → Lua error caught by `pcall`, and uncaught → `CallResult.ok=false` with message); `settings_get`; `budget_endless_loop` (`while true do end` in `on_step` with a 5 ms budget → returns within 50 ms, `budget_exceeded=true`); `memory_cap` (script allocating a growing table → error, process alive); `syntax_error_reported` (load fails with `ok=false` and line number in error); `ui_calls_forwarded` (fake UiApi records `button("x")` result handling).
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** with the raw Lua C API (no sol2). One `lua_State` per mod. All entry points take the mod's mutex (see Task 13).
- [ ] **Step 4: Run.** Expected: PASS.

---

### Task 11: JavaScript runtime (QuickJS-ng)

**Files:**
- Create: `modding/runtime_js.h`, `modding/runtime_js.cpp`
- Modify: `modding/CMakeLists.txt` (FetchContent quickjs-ng, pinned release tag)
- Test: `tests/test_mod_js.cpp`

**Interfaces:**
- Produces: `std::unique_ptr<ModRuntime> make_js_runtime(ModContext&);` — same globals as Lua (`uc`, `settings.get`, `log`, `on`, `ui`) with JS conventions; no `std`/`os` modules (do not call `js_init_module_std/os`); ES modules resolve only inside the mod folder via a custom module loader using `confine`; `eval`/`Function` allowed (they cannot reach anything outside the globals); `JS_SetMemoryLimit(64 MiB)` and `JS_SetInterruptHandler` deadline check.

- [ ] **Step 1: Write failing tests** mirroring Task 10: `no_std_os` (`typeof std`, `os`, `require`, `process` all `undefined`); `import_confined` (relative import inside the mod works; `../x.js`, absolute, URL imports rejected); `non_ascii_mod_root`; `events`; `api_read_write` (`uc.player(1).magazine = 7`); `api_error_is_exception`; `settings_get`; `budget_endless_loop` (`for(;;){}`), `memory_cap` (growing array), `syntax_error_reported`, `ui_calls_forwarded`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.** Values cross as `Value` (nlohmann) converted via JSON for objects, native for scalars.
- [ ] **Step 4: Run.** Expected: PASS.

---

### Task 12: Native DLL runtime

**Files:**
- Create: `modding/uc_mod.h`, `modding/runtime_native.h`, `modding/runtime_native.cpp`, `modding/sha256.h`, `modding/sha256.cpp` (Windows CNG)
- Modify: `tests/CMakeLists.txt` (a test DLL target `test_mod_dll` as `MODULE`)
- Test: `tests/test_mod_native.cpp`, `tests/test_mod_dll.cpp` (the DLL source)

**Interfaces:**
- `uc_mod.h` (C, `extern "C"`): `#define UC_MOD_API_MAJOR 1`, `#define UC_MOD_API_MINOR 0`, `#define UC_MOD_IMGUI_VERSION "1.92.9b"`; `typedef struct UcApi { uint32_t struct_size, api_major, api_minor; const char* imgui_version; void* imgui_context; void* (*imgui_alloc)(size_t, void*); void (*imgui_free)(void*, void*); void* imgui_alloc_user; const char* mod_id; void (*log)(const char*); int (*mem_read)(uint32_t addr, void* dst, uint32_t len); int (*mem_write)(uint32_t addr, const void* src, uint32_t len); const char* (*call_json)(const char* path, const char* args_json, const char* value_json); const char* (*settings_get_json)(const char* key); void (*on)(const char* event, void (*cb)(const char* payload_json, void* user), void* user); void (*set_ui)(void (*draw)(void* user), void* user); } UcApi;` Exports required of a mod: `int uc_mod_init(const UcApi*)` (0 = ok) and `void uc_mod_shutdown(void)`. Strings returned by the host are valid until the next call from the same thread.
- `std::unique_ptr<ModRuntime> make_native_runtime(ModContext&, const NativePolicy&)`, `struct NativePolicy { bool allow_native; std::function<bool(const std::string& mod_id, const std::string& sha256_hex)> confirmed; }`. `load` refuses with a clear error when `allow_native` is false, when `confirmed(id, hash)` is false, or when `api_major` differs.
- `std::string sha256_file_hex(const std::filesystem::path&)`.

- [ ] **Step 1: Write failing tests**: `sha256_known` (empty file → `e3b0c442...b855`, `"abc"` → `ba7816bf...15ad`); `refuses_when_not_allowed`; `refuses_when_unconfirmed` (hash mismatch counts as unconfirmed); `loads_test_dll` (the DLL calls `api->log`, registers an `on_step` callback that increments a counter through `mem_write` on a test buffer; `fire("on_step")` runs it); `api_major_mismatch_refused` (struct field set by the host differs from the DLL's compiled constant → init returns non-zero and load reports it); `imgui_version_mismatch_refused`; `missing_exports_reported`; `shutdown_called_once`; `non_ascii_dll_path`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** with `LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`; `FreeLibrary` after `uc_mod_shutdown`. DLL callbacks are wrapped in SEH (`__try`) so a hardware exception becomes `CallResult{ok=false}` where the OS allows; document that this cannot make a corrupt DLL safe.
- [ ] **Step 4: Run.** Expected: PASS.

---

### Task 13: Host, events, hooks and wiring

**Files:**
- Create: `modding/host.h`, `modding/host.cpp`, `modding/hooks.h`
- Modify: `game/entry_probe.h` (`UC_ENTER`), `game/game_pacing.cpp:80` (`hle_GameFrameWait`), `game/uc_main.cpp`, `game/live_settings.{h,cpp}`, `game/CMakeLists.txt`
- Test: `tests/test_mod_host.cpp`, `tests/test_mod_hooks.cpp`

**Interfaces:**
- Produces:
```cpp
class ModHost {
 public:
  ModHost(ModRegistry&, ApiRegistry&, GuestMemory&, ModHostConfig);     // ModHostConfig: Budgets, NativePolicy, log sink
  void start();                                      // creates a runtime per active mod by file extension of manifest.script (.lua/.js/.mjs) and/or manifest.native; fires on_load
  void step();                                       // once per game step on the guest thread: fires on_step to all mods within the 1 ms total budget
  void settings_changed(std::string_view mod_id);    // fires on_settings_changed
  void fire_game_event(std::string_view event, const Value& payload);
  void shutdown();                                   // on_unload, destroy runtimes
  struct ModStatus { std::string id; bool running; bool disabled_for_session; std::string last_error; uint32_t failures; };
  std::vector<ModStatus> status() const;
  void reload_scripts();                             // scripts only; overlay/textures/DLL need a restart
};
```
  Per-mod `std::mutex` held around every callback into that mod's runtime (guest thread and UI thread both call in). Failure policy: 3 failures (exception or budget) → `disabled_for_session`, no more callbacks, status shows the last error. A native DLL stays loaded but gets no more callbacks.
- `modding/hooks.h`: `inline std::atomic<HookFn*> g_hook_slots[kHookCount]`; `template<uint32_t A> inline void hook_enter(ppc::Ctx& c) { if constexpr (is_hookable(A)) { if (auto* h = g_hook_slots[hook_index(A)].load(std::memory_order_relaxed)) (*h)(c); } }` with `is_hookable`/`hook_index` built `constexpr` from `modding/generated/hookable.inc`. `UC_ENTER(a)` gains `uc::mods::hook_enter<a>(c);`. API member `hook.pre(name, fn)` installs a slot, calling the script/DLL callback with the registers needed by the schema (`args` r3..r10 as ints, `f1..f8` as floats). `hook.post`/`hook.replace` need translator support: Step 5 below investigates; if it needs more than a small translator change, ship `pre` only and mark `post`/`replace` `planned` in the schema (report this to the owner).
- Step event: in `hle_GameFrameWait` call `mods::host_step_if_installed()` exactly once per elapsed game step of the main clock (`object==kClock`); under pause (`game_pacing_paused()`) no step events.
- `LiveSettings`: add `const nlohmann::ordered_json& mods_section() const` and `void apply_mods(const nlohmann::ordered_json& section)` (sets `document_.properties["mods"]` and saves like `save()`; a refused save reports through `error()` as the other setters do).

- [ ] **Step 1: Write failing tests** (`test_mod_host.cpp`, using fake `ModRuntime` objects injected through a factory seam `ModHostConfig::runtime_factory`): `start_creates_runtime_per_mod_and_fires_on_load`; `step_fires_each_mod_once`; `step_total_budget` (two fakes each sleeping 0.8 ms → second still runs but the host records the overrun and skips later mods next step is NOT required: assert only that `step()` never runs a callback after the 1 ms total is spent in that step and reports `budget_exceeded` on the mod that crossed it); `three_failures_disable` (fake throwing → after 3 steps status `disabled_for_session`, a 4th step makes no call; other mods unaffected); `reload_scripts_restarts_script_mods_only`; `concurrent_calls_serialised` (guest-thread `step()` and a UI-thread `draw_ui` from two threads never overlap inside one mod: fake asserts with an atomic flag); `no_mods_no_cost` (`ModHost` with zero active mods: `step()` returns immediately, no runtime created).
  `test_mod_hooks.cpp`: `hook_slot_null_by_default`; `hook_installed_called_with_context` (instantiate `hook_enter<0x8006C268u>` with a fake `Ctx`, install a slot, assert one call with the register values); `non_hookable_compiles_to_nothing` (`static_assert(!is_hookable(0x80000000u))`, `static_assert(is_hookable(0x8006C268u))`); `hook_index_stable` (every address in `hookable.inc` maps to a unique index below `kHookCount`).
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** `ModHost`, `hooks.h`, the `LiveSettings` additions and the `UC_ENTER` change.
- [ ] **Step 4: Run** the new tests plus `ctest -R "test_live_settings|test_options"`. Expected: PASS.
- [ ] **Step 5: Translator investigation (timeboxed to one session):** check whether a post/replace hook can be added through the existing wrapper mechanism (`project/uc.cfg` `entry`/`hle` rows, `tools/pacing_hooks.py`, `common/translator/emit.cpp`) without emitting code in functions that are not hookable. Record the result in `docs/modding/hooks.md`. If feasible and small, implement `hook.post`/`hook.replace` for the curated list with a test in `test_mod_hooks.cpp`; otherwise leave them `planned` in `uc_api.json` and re-run the generator.
- [ ] **Step 6: Wire `game/uc_main.cpp`:** after `open_root` (`uc_main.cpp:294`) discover mods from `exe_dir()/"mods"` with `ModRegistry::discover(..., live config "mods" section, ApiVersion{1,0}, game_version)`, call `apply_mod_overlays` before `DvdSession` is created (`:321`), call `load_mod_textures` right after the base `load_index` (`:441`), build `ApiRegistry`/`GuestMemory` from `boot.mem`, create and `start()` the `ModHost` once the guest is ready, install `ModHost::step` for Step event, and `shutdown()` on exit. Skip everything (no registry scan beyond a directory existence check, no host) when `mods/` is absent or has no active mods. Tests and harness runs pass `--no-mods` (new option in `game/options.*`, default off; the harness sets it by default and the mod scenarios of Task 17 turn it on).
- [ ] **Step 7: Bench**: stage `uc_before.exe` from `main` and the new `uc.exe` next to `dist` data, run `python -m uctest bench <before> <after> --demo fight2` once with no `mods/` folder. Expected: difference within the ~5% screening threshold; if above, run `--runs 3`. Record the result in the PR/notes.

---

### Task 14: Game API v1 (player and weapons) and weapon-data reverse engineering

**Files:**
- Create: `modding/game_api_player.cpp` (members), `docs/spikes/M1-weapon-data.md`
- Modify: `modding/api/uc_api.json` (statuses), generated docs
- Test: `tests/test_mod_game_api.cpp`; scenario in Task 17

**Interfaces:**
- `void register_player_weapon_api(ApiRegistry&, GuestMemory&)`.
- Players: object pointers come from the actor list `804421C4` (200 x `(id, ptr)`), players ids 2 and 3 (slot 1 -> id 2, slot 2 -> id 3); returns `ApiError("player not present")` when the entry is empty. Offsets: magazine `player+0x4D4`; damage meter `scene+0x6C00` (P2 `+0x6C14`, scene base per `docs/spikes/G22-autoplay-memory.md`); HUD life `804B7960`; camera `8044B4E0` (3x4 float view matrix, read-only); enemies: list from the actor list where object class word at `+0x10` is `803F2B60` (zombies), position `+0xF48` (3 floats, 10 cm units), hp `+0x3A4` float. Width and endianness of each field must be confirmed with the harness `peek` before the member is marked `implemented`.
- Members (all in `uc_api.json`): `player(slot).magazine` rw, `.grenades` rw (via the code behind `--god`: `docs/testing/test-server.md:68`, find with Grep in `game/test_state.cpp`/`game/test_protocol.cpp`), `.damage` rw (scene damage meter), `.invulnerable` rw (when true the API zeroes the damage meter every step, like `--god`), `.infinite_ammo` rw (keeps magazine at its capacity every step), `.position` r (if found), `camera.view` r, `enemies.list()` r (id, class, x, y, z, hp), `events.on_weapon_fired`, `events.on_damage_taken`, `events.on_reload` (via pre-hooks on `GameWeaponUpdate 8006C268` and `800676A0`; confirm which function carries each event).
- Weapon fields `weapon(name).damage|fire_rate|capacity|reload_time|fire_mode`: locate the data (spike `M1-weapon-data.md`). Each field gets at most two approaches (trace writes at `player+0x4D4` with `game/write_trap.cpp`, scripted Dolphin from `docs/spikes/G20-dolphin-reference.md`, diff of memory before/after weapon switch, published Gecko codes for game id RBUE08 as a lead). Found fields become `implemented` with an address/offset table in the spike; unfound ones stay `planned`.

- [ ] **Step 1: Write failing tests** with a synthetic guest buffer: `player_magazine_roundtrip`; `player_missing_slot_error`; `invulnerable_zeroes_damage_each_step` (step the API tick function `void game_api_step(GuestMemory&)` twice after setting the meter to 20 → 0); `infinite_ammo_refills`; `enemies_list_parses_actor_table` (two zombie objects + one non-zombie in a fake actor list → two entries with hp/position); `camera_view_read`; `schema_consistency` from Task 9 passes with the new members.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Verify offsets live** using the harness (skill `.claude/skills/uc-test/SKILL.md`): `python -m uctest run train_gameplay --speed 8` plus `peek` calls; record confirmed width/type of each field in `docs/spikes/M1-weapon-data.md`. One or two runs per question, prefer offline evidence.
- [ ] **Step 4: Implement** `register_player_weapon_api` and `game_api_step`; `ModHost::step` calls `game_api_step` before mod `on_step` callbacks.
- [ ] **Step 5: Weapon-data RE** per Interfaces; update statuses; regenerate docs (`python scripts/gen_mod_api.py`).
- [ ] **Step 6: Run** `ctest -R "test_mod_game_api|test_mod_api|test_mod_api_generated"`. Expected: PASS.

---

### Task 14a: Data patches

**Files:**
- Create: `modding/data_patch.h`, `modding/data_patch.cpp`
- Modify: `modding/host.cpp`
- Test: `tests/test_mod_data_patch.cpp`

**Interfaces:**
- Consumes: `ApiRegistry`, `ModEntry`.
- Produces: `struct PatchResult { size_t applied=0; std::vector<std::string> errors; }; PatchResult apply_data_patches(const ModEntry&, const ApiRegistry&, const GuestMemory&)`. Each `<mod>/data/*.json` (files in name order) is a flat object `{"weapon.pistol.damage": 2.0, "player.1.magazine": 99}`: the key is `domain.<entity>.<member>` (entity = weapon name or player slot), resolved to a writable `Member`. Unknown paths, `planned` members and read-only members are reported as errors and skipped; the rest still applies. `ModHost::start()` applies the patches of every active mod in load order (later wins) after `on_load`, and again on the `on_mission_start` event once that event exists.

- [ ] **Step 1: Write failing tests**: `applies_flat_values`; `unknown_path_reported_others_applied`; `readonly_member_rejected`; `planned_member_skipped_with_message`; `later_mod_wins`; `invalid_json_file_reported_not_thrown`; `non_object_root_rejected`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** `apply_data_patches` and call it from `ModHost::start()`.
- [ ] **Step 4: Run** `ctest -R test_mod_data_patch`. Expected: PASS.

---

### Task 15: Menu: manager tab, mod tabs, free drawing

**Files:**
- Create: `modding/menu_bridge.h`, `modding/menu_ui.h`, `modding/menu_ui.cpp`
- Modify: `game/settings_menu.{h,cpp}`, `game/uc_main.cpp`, `launcher/translations.inc` (new strings via the existing `tr()` keys)
- Test: `tests/test_mod_menu.cpp`

**Interfaces:**
- `menu_bridge.h` (no ImGui types): 
```cpp
struct ModTab { std::string id, title; };
class ModMenu { public: virtual ~ModMenu() = default;
  virtual void manager_tab(float unit) = 0;
  virtual std::vector<ModTab> tabs() = 0;                 // active mods that have settings or a UI callback, load order
  virtual void draw_tab(const std::string& id, float unit) = 0;
  virtual void commit() = 0; };                           // main loop: persist dirty settings, fire on_settings_changed
```
- `SettingsMenu::set_mod_menu(ModMenu*)`. After the four built-in tabs: a tab "Mods" (`manager_tab`), then one tab per `tabs()` entry. Tab bar uses `kMenuTabBarFlags` (Task 1). `SettingsMenu::commit()` calls `mod_menu_->commit()`.
- Manager tab content: global "Allow native (DLL) mods" checkbox (default off; first enable shows the warning text from the spec); a row per mod: state badge (enabled / disabled / error / incompatible / missing dependency), version, author, enable checkbox, up/down buttons, message lines, a "native code" marker, conflicts list (`ModRegistry::conflicts`), "Restart required" label whenever overlay/texture/DLL/enable state differs from what was loaded, a "Reload scripts" button; DLL confirm popup (mod name, hash, text "native code can do anything on this computer"; Confirm stores `native_hash[id]`).
- Mod tab: declarative settings rendered in manifest order (bool -> checkbox, int/number with min/max -> slider, number without range -> drag input, choice -> combo, text -> input), labels/tooltips by current language with `en` fallback; then, if the mod has a UI callback, a separator and `ModRuntime::draw_ui(Budget{4 ms})`. A change calls `ModSettings::set`, which marks dirty; `commit()` writes `mods.<id>.settings` through `LiveSettings::apply_mods` and calls `ModHost::settings_changed`.
- `UiApi` implementation over ImGui (Task 10 interface): widgets prefix ids with the mod id so two mods never share an ImGui id; text input buffers are per-call-site strings kept by the implementation.

- [ ] **Step 1: Write failing tests** (`test_mod_menu.cpp`, headless ImGui context: `ImGui::CreateContext`, `io.DisplaySize`, built font atlas, `NewFrame/Render` per frame; no renderer): `tabs_appear_after_builtins` (3 fake mods -> tab order Main, Effects, DLSS 5, Controls, Mods, mod1..3); `tab_bar_scrolls_with_many_mods` (12 mod tabs at 746 px width -> `WidthAllTabs > BarRect width` and a tab beyond the width becomes visible after `ImGui::TabBarScrollToTab`); `declarative_widgets` (a setting of each type renders; simulated click on a bool flips it and sets dirty); `slider_clamps`; `commit_saves_section` (fake LiveSettings target receives `mods.<id>.settings` with the changed key only after `commit()`, not during `build`); `commit_fires_settings_changed_once`; `language_fallback` (missing `ru` label -> `en`); `free_draw_called_within_budget` (fake runtime counts `draw_ui` calls; a runtime that exceeds 4 ms adds a failure to the host status); `ids_unique_between_mods` (two mods both draw `button("OK")`; only the clicked mod's callback sees `true`); `manager_toggle_marks_restart_required`; `native_confirmation_flow` (checkbox off -> DLL mod shows locked; on -> confirm popup; confirm stores the hash).
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest -R "test_mod_menu|test_settings_menu"`. Expected: PASS.
- [ ] **Step 5: Check in the real game window** via the harness (never with system input): the Task 17 scenario opens the menu with the `host_key` command, takes a capture of the Mods tab and of an example mod tab, and the capture is looked at once for layout problems (1280x720 only).

---

### Task 16: Modded save profile

**Files:**
- Create: `modding/profile.h`, `modding/profile.cpp`
- Modify: `game/uc_main.cpp:319`
- Test: `tests/test_mod_profile.cpp`

**Interfaces:**
- Produces: `std::filesystem::path resolve_save_root(const std::filesystem::path& exe_dir, bool explicit_root_given, const std::vector<const ModEntry*>& active, std::string& warning)` — returns `exe_dir/"saves"` when `explicit_root_given` (harness) or no active mod has `touches_gameplay`; otherwise `exe_dir/"saves-modded"`, created by recursively copying `saves` once when the folder does not exist (partial copy cleaned up on failure and the vanilla root returned with a `warning`; never writes into `saves`).

- [ ] **Step 1: Write failing tests**: `vanilla_without_gameplay_mods`; `explicit_root_wins`; `modded_copy_created_once` (second call does not re-copy; a file changed in `saves-modded` survives); `vanilla_untouched` (hash of `saves` before == after); `copy_failure_falls_back` (unwritable target -> vanilla root + warning; no partial `saves-modded`); `missing_vanilla_creates_empty_modded`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement**, then use it at `game/uc_main.cpp:319` (`SaveFs saves(...)`), printing the warning to stderr.
- [ ] **Step 4: Run.** Expected: PASS.

---

### Task 17: Example mods, harness scenario, documentation

**Files:**
- Create: `mods-examples/infinite_ammo/` (Lua), `mods-examples/god_mode/` (JS), `mods-examples/pistol_autofire/` (Lua), `mods-examples/damage_multiplier/` (settings only), `mods-examples/hello_native/` (C++ DLL with an ImGui tab, own `CMakeLists.txt`, built by the root build into `dist/mods-examples/hello_native/native/`), `scripts/uctest/` scenario `mods_smoke` in `scripts/uctest/scenarios.py`, `docs/modding/*.md`
- Modify: `scripts/uctest/scenarios.py`, `build.bat` staging (copy `mods-examples` into `dist`)
- Test: harness scenario; `tests/test_mod_examples.cpp` (loads every example manifest through `parse_manifest` and runs the Lua/JS entries against a fake API)

**Interfaces / content:**
- `pistol_autofire` and `damage_multiplier` ship only when their API fields are `implemented` (Task 14); otherwise they are removed from `mods-examples` and listed as pending in `docs/modding/recipes.md`.
- Docs (all link the generated API reference, examples compile-checked by `test_mod_examples`): `docs/modding/getting-started.md`, `manifest.md` (every field, example), `settings-and-ui.md`, `lua.md`, `javascript.md`, `native-dll.md` (how to build against `uc_mod.h`, ImGui version, allocator handoff), `hooks.md`, `files-and-textures.md` (overlay semantics, archive limitation, texture naming, load order), `saves.md`, `safety.md` (sandbox, native warning, budgets, what a mod can never do), `recipes.md` (one per example), `api/` (generated), `api-changelog.md`.
- Harness scenario `mods_smoke` (uses `--game-arg` to enable mods; window stays 1280x720): enables `infinite_ammo` + `god_mode` from a temp copy of `mods-examples`, runs `train_gameplay`, fires, and asserts through `peek` that the magazine does not drop below its start value and the damage meter stays 0; opens the menu (`host_key`) and captures the Mods tab.

- [ ] **Step 1: Write failing tests**: `test_mod_examples` — each example folder has a valid manifest; `infinite_ammo` against a fake API: after `fire("on_step")` the fake `player.magazine` equals its initial value after being decremented; `god_mode` likewise for `damage`; `damage_multiplier` has only declarative settings and a data patch file that parses.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Write** the example mods and docs. Every doc page states which API fields are `planned`.
- [ ] **Step 4: Run** `ctest --test-dir build --output-on-failure` (all). Expected: PASS.
- [ ] **Step 5: Run** `cd scripts && python -m uctest run mods_smoke --speed 8` once. Expected: scenario passes, capture shows the Mods tab. Fix and rerun once if it fails; do not loop.
- [ ] **Step 6: Final check** with `superpowers:verification-before-completion`: full build `build.bat --no-install`, full `ctest`, `python scripts/gen_mod_api.py --check`, and the bench from Task 13 once more against `main`.

---

## Self-review notes

- Spec coverage: goals 1-12 map to Tasks 2-5 (package, manager data), 6 (overlay, textures), 7 (settings), 8 (schema/docs/d.ts/stubs), 10-12 (runtimes), 13 (host, events, hooks, no-cost), 14 (player/weapons API), 15 (menu tabs, scroll in Task 1, persistence via `LiveSettings::apply_mods`), 16 (saves), 17 (docs, examples). Data patches (goal 5) are Task 14a.
- Open risks reported to the owner: hook `post`/`replace` may stay `planned` (Task 13 step 5); weapon stat fields may stay `planned` (Task 14 step 5); `pistol_autofire`/`damage_multiplier` depend on that.

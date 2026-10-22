# Asset mods, milestone 2: text Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (the owner chose inline execution). Steps use checkbox syntax.

**Goal:** A mod puts `text/<lang>.json` (or `text/all.json`) next to `mod.json` and the game shows its strings instead of the originals.

**Architecture:** `modding/text/txtres.*` reads and patches the game's `mes_<lang>.txtres` (changed records are repointed to strings appended at the end of the file; an unchanged file stays byte-identical). `modding/text/text_mods.*` applies the active mods' JSON in load order to the base file of each language and writes the result to `mods/.cache/_text/<key>/Data/message/` (a new folder per content version, so a file the game holds open is never rewritten). `GameMods` puts that root last in the overlay list and shows problems in the Mods tab. Patching is synchronous (a quarter of a megabyte per language), so the text is in place before the game opens the file.

**Spec:** `docs/superpowers/specs/2026-10-22-asset-mods-design.md` ("Text (milestone 2)"), with the spike results below.

## Spike result (2026-10-23, done; files of the owner's install)

`mes_<lang>.txtres` (languages `us en fr ge it jp sp du`; `csvtbl` files are out of scope: whole-file replacement already works), all big-endian:

- Header: `TRES`, u32 `0x00010000`, u32 record count (2634), u32 table offset (`0x10`).
- Table: one u32 per record, the file offset of the record (ascending; here `0x2938 + 20 * i`).
- Record, 20 bytes: u32 id (ascending, unique), u32 voice-name pointer (0 or the offset of a `VO_xxxxxxx` string), u32 line count, u32 text pointer, u32 0.
- Strings: UTF-8, NUL-terminated, a record's lines are consecutive; pointers are absolute file offsets; the pool is not dense (strings are shared, gaps exist, the Russian `us` file starts its pool at `0x12028` after a leftover German block), so a loader tolerates arbitrary pointers.
- Control codes: none besides printf-style `%s` `%c` `%d` (45, 40, 9 uses in `en`) which must stay as they are.
- The owner's `mes_us` is a Russian localization drawn with the font's own code points (`Handgun` is `&ucmovem` there). The JSON carries whatever UTF-8 the file holds; `tools/dump_text.py` shows it raw. Record 500 (`Samurai Edge 2`) is the handgun's name in the HUD at the start of the train chapter.

## Global Constraints

- JSON: `{ "<id>": "text" }` with decimal id keys; `\n` in the text separates lines (the line count of the record follows the text); `all.json` applies to every language file, then `<lang>.json` of the same mod; later mods win per id.
- A string may not hold NUL, may not exceed 2048 bytes, and a record may have at most 32 lines.
- Unknown ids, malformed JSON and bad values are reported per mod ("text: <file>: ...") and skipped; the rest of the mod's text still applies.
- Never modify the disc files or a generated file in place; cache key = FNV-1a of the produced bytes; folders of older versions are deleted at the next start.
- Edit source only with Edit/Write; tests: `ctest --test-dir build --output-on-failure -R <name>`; full `build.bat` deploy at the end.

## Review Focus

1. A string whose line count differs from the original (more or fewer lines).
2. Two mods changing the same id, and `all.json` plus `<lang>.json` in one mod.
3. A damaged or truncated base file (pointers outside the file, missing NUL, absurd count): refused with a message, never a crash or out-of-bounds read.
4. A mod's own `files/Data/message/mes_us.txtres` as the base for the patches.
5. Toggling a text mod while the game runs: the already open file keeps its version, the next open sees the new one.
6. JSON with unicode escapes, emoji, and strings holding `%s`/`%d`.

---

### Task 1: TRES reader and patcher

**Files:** Create `modding/text/txtres.h`, `modding/text/txtres.cpp`; Test `tests/test_txtres.cpp` (+ `modding/CMakeLists.txt`, `tests/CMakeLists.txt`)

**Interfaces:**
- Produces:
  - `struct TextRecord { uint32_t id; std::vector<std::string> lines; };`
  - `std::optional<std::vector<TextRecord>> read_txtres(std::span<const uint8_t> file, std::string& error);` validates everything it reads (magic, version, count at most 100000, table and records inside the file, every pointer inside the file, every line NUL-terminated inside the file, ids strictly ascending)
  - `using TextChanges = std::map<uint32_t, std::vector<std::string>>;`
  - `struct TextPatch { std::vector<uint8_t> file; std::vector<uint32_t> unknown_ids; };`
  - `std::optional<TextPatch> patch_txtres(std::span<const uint8_t> file, const TextChanges& changes, std::string& error);` validates like `read_txtres`; ids not in the file go to `unknown_ids`; the rest append their lines at the end of the file and update the record's count and pointer; no changes: the same bytes.

- [ ] Tests first (`tests/test_txtres.cpp`, fixture built by a helper `make_txtres(records)` that lays out header, table, records and a pool with a leftover block, a shared string and a voice-name pointer like the real files): `reads_records_and_voice_names`, `patch_without_changes_is_identical`, `patch_changes_text_and_line_count` (1 line to 3 lines and 2 lines to 1; neighbours untouched; voice pointer and field 5 kept), `unknown_ids_are_reported`, `shared_strings_are_not_disturbed` (two records pointing at one string, one patched), `rejects_damaged_files` (bad magic, version, count 100001, table past the end, pointer past the end, string without NUL, descending ids, empty), `patched_file_reads_back_with_read_txtres`. Run: FAIL (missing header).
- [ ] Implement; run: PASS. Cross-check on `mes_en.txtres`, `mes_us.txtres` and `mes_jp.txtres` of the install with a throwaway program (all three read, patch-without-changes identical, one patched record reads back). Commit "Add TRES reader and patcher".

### Task 2: text mods (JSON, load order, generated root)

**Files:** Create `modding/text/text_mods.h`, `modding/text/text_mods.cpp`; Test `tests/test_text_mods.cpp`

**Interfaces:**
- Consumes: `read_txtres`/`patch_txtres` (Task 1), `ModEntry`
- Produces:
  ```cpp
  inline constexpr const char* kTextLanguages[] = {"us", "en", "fr", "ge", "it", "jp", "sp", "du"};
  struct TextBuild { std::filesystem::path root; std::map<std::string, std::vector<std::string>> problems; };   // root empty: nothing to overlay; problems by mod id
  using TextBase = std::function<std::optional<std::vector<uint8_t>>(const std::string& lang)>;   // the unpatched file of a language (disc or a mod's files/)
  TextBuild build_text_mods(const std::filesystem::path& cache_root, const std::vector<const ModEntry*>& mods, const TextBase& base);
  void prune_text_cache(const std::filesystem::path& cache_root, const std::filesystem::path& keep);   // deletes <cache>/_text/<other keys>
  ```
  `root` is `<cache_root>/_text/<key>`; the file is `<root>/Data/message/mes_<lang>.txtres`; only languages that have changes are written; an existing folder of the same key is reused.

- [ ] Tests first: `single_mod_patches_the_language_file`, `all_json_then_language_json_then_later_mod`, `line_breaks_split_into_lines`, `unknown_id_bad_json_and_bad_values_are_problems_not_fatal` (problems name the file and id; other ids still apply), `limits` (NUL, 2049 bytes, 33 lines), `same_input_reuses_the_folder_and_changed_input_makes_a_new_one`, `no_text_means_empty_root`, `missing_base_is_a_problem`, `prune_keeps_only_the_given_key`. FAIL, implement, PASS, commit "Add text mods".

### Task 3: GameMods integration, tool, harness check, docs

**Files:** Modify `game/game_mods.h`, `game/game_mods.cpp` (disc originals read before the first overlay; `refresh_files` builds the text root; `overlay_roots` appends it; `audio_status` becomes `mod_status` and appends the text problems), `modding/menu_ui.cpp` (problem colour for lines starting `text:`), `game/uc_main.cpp`; Create `tools/dump_text.py` (+ install rule in `tools/texture_extract/CMakeLists.txt`'s sibling, a new `tools/CMakeLists.txt` entry), `tests/test_dump_text.py`, `scripts/uctest/mods/text_marker/` (test-only mod: `all.json` sets record 500 to a marker), scenario `text_replace` in `scripts/uctest/scenarios.py`, `docs/modding/text.md`, link from `files-and-textures.md`, `docs/modding/manifest.md`.

- [ ] Failing check first: scenario `text_replace` reaches the train chapter with the marker mod and fails unless the marker string is found in guest memory (MEM1, then MEM2, by `peek`); a control run without the mod (`text_replace_plain`) must not find it. Expect FAIL before the integration.
- [ ] Implement the integration; PASS both scenarios; also look at one capture of the HUD (the handgun's name shows the marker).
- [ ] `tools/dump_text.py <txtres>` prints `{ "<id>": "line1\nline2" }` as JSON (UTF-8, ids ascending); test with a synthetic file.
- [ ] Docs, full `ctest`, full `build.bat` deploy, commit "Text replacement for mods (milestone 2)".

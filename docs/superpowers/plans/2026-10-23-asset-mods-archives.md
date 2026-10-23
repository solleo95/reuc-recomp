# Asset mods, milestone 3: archive members Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (the owner chose inline execution for the earlier milestones; same here). Steps use checkbox syntax.

**Goal:** A mod puts a file at `files/<disc path>.arc!/<path inside>` and the game sees that one member of the archive replaced (or added), without the mod shipping the whole archive.

**Architecture:** `modding/arc/u8.*` reads, edits and writes U8 archives (an unchanged archive stays byte-identical). `modding/arc/arc_mods.*` finds the members of the active mods, applies them in load order to the base archive of each target (the disc's file, reset by any later mod that ships the whole archive) and writes the results to `mods/.cache/_arc/<key>/Data/...` (a new folder per content version). `GameMods` puts that root last in the overlay list, shows problems and growth warnings in the Mods tab. `DvdFs` skips the `*.arc!` folders when it scans a mod's `files/`. `tools/arc_tool.py` unpacks, packs and lists archives for authors.

**Tech Stack:** C++20 (uc_mods library), Python 3 (tool), the uctest harness.

**Spec:** `docs/superpowers/specs/2026-10-22-asset-mods-design.md` ("Archive members (milestone 3)"), with the spike `docs/spikes/M6-arc-members.md`.

## Global Constraints

- Member path: `mods/<id>/files/<disc path>.arc!/<path inside the archive>`; the folder name ends in `.arc!` (case-insensitive), the rest is matched case-insensitively against the archive's names; separators `/`.
- Load order decides: for each target archive the base is the disc file; walking the active mods in order, a mod that ships the whole archive (`files/<disc path>.arc`) replaces the state so far, then the mod's members apply. The last mod wins per member.
- Never change the disc or a generated file in place; cache key = FNV-1a of the produced bytes; older folders deleted at start (`prune_arc_cache`), same pattern as `_text`.
- Limits (the port's): archive at most 256 MiB, at most 100000 nodes, a member at most 64 MiB, path inside at most 64 levels; a damaged archive (member past the end of the file, name offset outside the pool, directory ranges that do not nest) is refused with a message, never read out of bounds.
- Growth warning (spike M6: the game has a few hundred KB to 1 MB of heap headroom per archive): a rebuilt archive more than 256 KiB (262144 bytes) bigger than its base gets the problem line `arc: <disc path>: N KiB bigger than the original; the game may run out of memory loading it` (still applied).
- Problem lines start with `arc:` (the Mods tab shows them as problems); at most 20 per mod, then a note.
- Edit source only with Edit/Write; tests: `ctest --test-dir build --output-on-failure -R <name>`; full `build.bat` deploy at the end; a second `uctest` run is the live check.

## Review Focus

1. A member for an archive the game does not have, and for a path inside that does not exist (replaced vs added: a new member is added, a new folder too).
2. Two mods replacing the same member; one mod shipping a whole archive and a later mod a member of it; the reverse order.
3. A damaged base archive (bad magic, count huge, member past the end, name offset outside the pool, cyclic or overlapping directory ranges): refused, no crash.
4. A member that is a directory in the archive or a name that is a file where a folder is needed (`a.arc!/x/y` when `x` is a file).
5. Names: case differences, non-ASCII names, a member path with `..` or a drive letter (refused).
6. Toggling the mod while the game runs: the open archive keeps its version (new folder per key), the next open sees the new one.

---

### Task 1: U8 model (parse, edit, write)

**Files:** Create `modding/arc/u8.h`, `modding/arc/u8.cpp`; Modify `modding/CMakeLists.txt` (add `arc/u8.cpp`, `arc/arc_mods.cpp` to `uc_mods`); Test `tests/test_u8.cpp` (+ `tests/CMakeLists.txt`)

**Interfaces:**
- Produces:
  ```cpp
  namespace uc::mods {
  class U8Archive {
  public:
      static std::optional<U8Archive> parse(std::span<const uint8_t> file, std::string& error);   // validates everything it reads (limits above)
      std::vector<uint8_t> write() const;                                                    // unchanged archive: the original bytes
      bool replace(std::string_view path, std::vector<uint8_t> data, std::string& error);    // path inside, case-insensitive; error if it names a directory or a folder is a file
      bool add_or_replace(std::string_view path, std::vector<uint8_t> data, std::string& error);   // creates missing folders and the file; true also when it replaced
      bool contains(std::string_view path) const;
      std::vector<std::string> files() const;                                                // every file path, in node order
  };
  }
  ```
  Behavior: nodes keep their order; an added node goes at the end of its directory's range (node indexes, parent and end fields of later directories are fixed up); the name pool keeps its bytes and new names are appended; members are written in their original offset order (new ones last, in the order added), each 0x20 aligned, zero padded; the data area starts where it did if the tables did not grow, else at the next 0x20 multiple after the tables.

- [ ] Tests first (`tests/test_u8.cpp`, fixture `make_u8(files)` that builds a small archive by hand with two folders, a nested folder and 5 members): `parse_lists_files`, `write_without_changes_is_identical`, `replace_grows_and_shrinks_a_member_and_moves_the_later_ones` (re-parse and compare every other member), `replace_is_case_insensitive`, `add_creates_folders_and_file` (new file in an existing folder, new file in a new nested folder; node ranges re-parse), `add_or_replace_reports_both`, `rejects_directory_as_file_and_file_as_folder`, `rejects_damaged_archives` (bad magic, count 100001, member past the end, name offset past the pool, directory end index smaller than its own index, root not a directory, empty, truncated header), `padding_and_alignment` (every offset a multiple of 0x20), `round_trips_the_install_when_UC_GAME_DIR_is_set` (optional: every `.arc` of `<UC_GAME_DIR>/DATA/files` parses and writes identically; skipped when unset; the two `zzz_*.arc` files that overrun their end are expected to be refused). Run: FAIL (missing header).
- [ ] Implement `U8Archive`; run: PASS. Run the optional test once against the owner's install (614 archives, 612 identical, 2 refused). Commit "Add U8 archive model".

### Task 2: archive mods (members, load order, generated root)

**Files:** Create `modding/arc/arc_mods.h`, `modding/arc/arc_mods.cpp`; Test `tests/test_arc_mods.cpp`

**Interfaces:**
- Consumes: `U8Archive` (Task 1), `ModEntry`
- Produces:
  ```cpp
  struct ArcBuild {
      std::filesystem::path root;                                       // <cache>/_arc/<key>; empty: nothing to overlay
      std::map<std::string, std::vector<std::string>> problems;         // by mod id: "arc: <what>" lines
      std::vector<std::string> conflicts;                               // "archive member <arc>!/<path> is set by A and B (the last one wins)"
  };
  // The archive file of the disc by path relative to DATA/files (e.g. "Data/chara/EZ01.arc"); nullopt: the game has none.
  using ArcBase = std::function<std::optional<std::vector<uint8_t>>(const std::string& disc_path)>;
  ArcBuild build_arc_mods(const std::filesystem::path& cache_root, const std::vector<const ModEntry*>& mods, const ArcBase& base);
  void prune_arc_cache(const std::filesystem::path& cache_root, const std::filesystem::path& keep);
  ```
  The scan of a mod: every regular file under `<mod>/files` whose relative path has a component ending in `.arc!` (case-insensitive) splits into the archive path (up to and including `.arc`) and the member path (after the `!/`); symlinks skipped; a nested `.arc!` inside the member part is refused ("archives inside archives are not supported"); a path with `..`, a drive letter or an empty component is refused. Output files are `<root>/<archive path>`; only archives with at least one applied member are written; a folder of the same key is reused if every file is there at its length.

- [ ] Tests first (`tests/test_arc_mods.cpp`, fixture helper writes mods and a `make_u8` archive as the base): `member_replaces_inside_the_disc_archive`, `new_member_is_added`, `later_mod_wins_and_conflict_is_listed`, `whole_archive_of_a_later_mod_resets_earlier_members`, `members_of_a_later_mod_apply_on_an_earlier_whole_archive`, `unknown_archive_is_a_problem_with_the_path`, `damaged_base_is_a_problem_and_no_file`, `bad_paths_are_problems_not_fatal` (`..`, empty component, file used as a folder, nested `.arc!`), `growth_over_256_kib_warns_but_applies`, `same_input_reuses_the_folder_and_changed_input_makes_a_new_one`, `no_members_means_empty_root`, `prune_keeps_only_the_given_key`, `member_names_with_non_ascii_letters` (UTF-8 file name). FAIL, implement, PASS. Commit "Add archive member mods".

### Task 3: the DVD tree ignores member folders

**Files:** Modify `runtime/dvdfs.h`, `runtime/dvdfs.cpp`; Test `tests/test_dvd.cpp`

**Interfaces:**
- Produces: `std::filesystem::path DvdFs::disc_root() const` (the folder `open_root` was given; empty before); `scan()` skips every directory whose name ends in `.arc!` (case-insensitive) and everything below it.

- [ ] Tests first in `tests/test_dvd.cpp`: `overlay_skips_arc_member_folders` (an overlay holding `Data/a.arc!/x.bin` and `Data/b.bin` adds only `Data/b.bin`; `entry_of("Data/a.arc!")` is -1), `disc_root_is_the_opened_folder`. FAIL, implement, PASS. Commit "DVD overlay ignores archive member folders".

### Task 4: GameMods integration

**Files:** Modify `game/game_mods.h`, `game/game_mods.cpp` (`refresh_files` builds the archive root after the text root with an `ArcBase` that reads `dvd_->disc_root() / disc_path` from the disc, size at most 256 MiB, never through the overlay; `overlay_roots` appends `arc_root_` last; `mod_status` appends `arc:` problems; conflicts printed to stderr like the text ones; `prune_arc_cache` at start), `modding/menu_ui.cpp:174` (problem colour for `arc:`), `tests/test_game_mods.cpp`

**Interfaces:**
- Consumes: `build_arc_mods`, `prune_arc_cache` (Task 2), `DvdFs::disc_root` (Task 3).

- [ ] Test first in `tests/test_game_mods.cpp`: a temporary disc folder with `Data/x/A.arc` (built with `make_u8`) and a mod `m` with `files/Data/x/A.arc!/inner/new.bin`; after `GameMods` construction `dvd.entry_of("Data/x/A.arc")` has the rebuilt length and reading it parses with a member `inner/new.bin`; `dvd.entry_of("Data/x/A.arc!/inner/new.bin")` is -1; `mod_status("m")` is empty; with a member for a missing archive `mod_status` starts with `arc:`. FAIL (archive unchanged), implement, PASS. Commit "Archive member mods in the game".

### Task 5: tool, live check, docs, deploy

**Files:** Create `tools/arc_tool.py` (`list <arc>`, `unpack <arc> <dir>`, `pack <dir> <arc>`; pack orders nodes by case-insensitive name, folders first as the game's archives do not require a particular order), `tests/test_arc_tool.py`, `scripts/uctest/mods/arc_member/` (test-only mod; its member is generated by `tools/spikes/u8_grow_mod.py`-style code in the scenario setup, see below), scenario `arc_member` in `scripts/uctest/scenarios.py`; Modify `tools/CMakeLists.txt` (install rule, like `dump_text.py`), `docs/modding/files-and-textures.md`, `docs/modding/guide/en/files.md`, `docs/modding/guide/ru/files.md` ("Archives" sections), `docs/modding/api-changelog.md` is not touched (no API change); Create `docs/modding/archives.md` (reference: path form, rules, limits, growth, tool).

- [ ] `tests/test_arc_tool.py` first: pack of a directory then unpack gives the same files; unpack of a synthetic archive lists names; `pack` output parses with the same layout rules (0x20 alignment, tables first). FAIL, implement `tools/arc_tool.py`, PASS.
- [ ] Failing live check first: scenario `arc_member` installs mod `arc_member` (one file `files/Data/layout/game_us.arc!/lyt_data/timg/2SHT_B.tpl` = the original member bytes + `UCARCMARKER\0` + zero padding to the next 0x20, produced from the owner's install at run time by a helper in the scenario; skipped with a message when `UC_GAME_DIR` is unset), reaches the train chapter, and requires the marker in guest memory (MEM2 window scan like `text_replace`); a control run without the mod (`arc_member_plain`) must not find it. Expect FAIL before Task 4's integration is deployed in `dist`, PASS after `build.bat --no-install`.
- [ ] Docs: `archives.md` (what a member path is, load-order rules, `arc:` messages, the growth limit with the M6 numbers, `arc_tool.py`), "Archives" sections of the guides in both languages (replace "planned but not available" with the real rules and a worked example), link from `files-and-textures.md`; `docs/modding` link test passes.
- [ ] Full `ctest`, full `build.bat` deploy, one Opus review of the whole milestone, fix findings, commit "Archive member replacement for mods (milestone 3)".

# Steam Deck Launcher Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Steam Deck launcher (`uc_launcher` for Linux) that configures and starts the game, unpacks disc images, and updates itself, plus a release scheme in which each platform's launcher reacts only to updates of its own archive.

**Architecture:** The release selection becomes platform-aware and portable (version comes from the archive name, one `releases/latest` request). The Windows-only update stack (`updater/`: WinHTTP, Win32 handles, transaction) gets a POSIX twin with the same headers (libcurl, POSIX files, rename-based transaction). The Deck launcher is a separate program on the shared libraries (`settings_registry`, `graphite_style`, `localization`, `binding_editor`) with its own `main`, file browser and image unpack.

**Tech Stack:** C++20, SDL3, Dear ImGui, miniz, nlohmann_json, libcurl (Linux), Python (release scripts), CMake; Linux cross build `scripts/linux/build.ps1`, Deck over `ssh deck`.

**Spec:** `docs/superpowers/specs/2026-10-31-steamdeck-launcher-design.md` (read it first; the plan only adds decisions).

## Global Constraints

- Archive names `UC-Windows-<ver>.zip`, `UC-SteamDeck-<ver>.zip`; both attached to every release; release is always Latest; one request `GET https://api.github.com/repos/solleo95/reuc-recomp/releases/latest`.
- Download URL must equal `https://github.com/solleo95/reuc-recomp/releases/download/<tag>/<name>`; HTTPS only, redirects only to `github.com`, `release-assets.githubusercontent.com`, `objects.githubusercontent.com`, at most 5; SHA-256 (GitHub `digest`) checked before unpacking; metadata limit 1 MiB.
- Installed version = `release.json` next to the launcher; no valid `release.json` disables updating.
- Protected from updates (both platforms): `config.json`, `config.json.*`, `cpu-profile.json*`, `saves/`, `mods/`, `textures/`, `game/`, `debug/`, `logs/`, `steam-env.txt`, `next-run.sh`, `launcher-state.json`, `data/pipeline_keys.bin`, `data/pipeline_cache.bin`, `data/pipeline_cache.tmp`, `.uc-update/`, `release.json`.
- Deck tabs: Game, Graphics, Controls (no Controllers tab, no Multiplayer, no CPU calibration, no DLSS). Language selector in the sidebar. 13 languages; new strings go into `launcher/translations.inc` for all of them.
- Deck window: fullscreen 1280x800 normally; every test or snapshot run uses a windowed 1280x720 window. `uc` is only run through the `uctest` harness.
- Source edits only through Edit/Write (no heredoc/sed/python patches of source). Commits and pushes only when the owner asks: no commit steps in this plan. The Deck must be left clean (`python scripts/linux/deck.py clean`).
- Build and tests on the PC: `build.bat --no-install`; Linux: `scripts/linux/build.ps1`; Linux tests run on the Deck: `python scripts/linux/deck.py run build-linux-znver2/tests/linux/<name>`.

## Review Focus

- Release JSON where the own archive is missing, duplicated, not `uploaded`, has no digest, or has a different version in name and `release.json` -> a clear error, never a wrong update (Task 1, 5).
- Power loss or kill in the middle of an update -> next start restores the old files and does not touch `game/`, saves, config (Task 6).
- Game folder or image path with spaces, Cyrillic, on an SD card (exFAT/ext4), read-only mount -> browse, unpack and launch work or show a readable error (Tasks 11, 12).
- No network, GitHub rate limit or a hung connection at start -> the launcher still opens and Play works within the timeouts (Tasks 7, 8).
- Disk full during unpack or update -> no half-written `game/`, old install intact (Tasks 6, 12).
- Launcher started from Steam with another working directory -> it finds its own files (Task 9).

---

### Task 1: Platform-aware release selection

**Files:**
- Modify: `updater/release.h`, `updater/release.cpp`, `updater/download.h`, `updater/download.cpp` (only `check_release`), `launcher/update.cpp` (call site)
- Modify: `tests/test_update_release.cpp`, `tests/test_update_download.cpp`, `tests/fixtures/update/release-v1.json`

**Interfaces:**
- Produces: `enum class Platform { windows, steamdeck };` `std::string_view archive_prefix(Platform);` (`"UC-Windows-"`, `"UC-SteamDeck-"`) in `release.h`.
- Produces: `Result<std::optional<Release>> select_release(std::string_view json, Version installed, Platform platform = Platform::windows);` — `Release::version` is the version in the archive name, not the tag.
- Produces: `Result<std::optional<Release>> check_release(Transport&, Version installed, std::stop_token stop = {}, Platform platform = Platform::windows);`

- [ ] **Step 1: Write failing tests** in `tests/test_update_release.cpp` (keep its helper style): `select_uses_asset_version_not_tag` (tag `0.0.9`, assets `UC-Windows-0.0.5.zip` + `UC-SteamDeck-0.0.9.zip`, installed 0.0.5: Windows -> none, Deck installed 0.0.8 -> Release 0.0.9), `select_equal_or_lower_is_none`, `select_missing_own_archive_is_invalid_release`, `select_duplicate_own_archive_is_invalid_release`, `select_requires_uploaded_size_and_digest`, `select_url_must_use_release_tag_and_asset_name`, `select_ignores_draft_and_prerelease`, `select_ignores_other_platform_asset_when_own_present`.
- [ ] **Step 2: Run** `build.bat --no-install` (or `cmake --build build --target test_update_release && ctest --test-dir build -R test_update_release`). Expected: compile error / FAIL.
- [ ] **Step 3: Implement** in `release.cpp`: find the single asset named `archive_prefix(platform) + X.Y.Z + ".zip"` (parse `X.Y.Z` with `parse_version`, no `v`), compare with `installed` first (equal/lower -> `std::optional<Release>{}`), then run the existing asset checks (state `uploaded`, size in range, `digest` `sha256:` + 64 hex, URL check against `tag_name` string, notes from `body`). `tag_name` is only used for the URL and need not parse as a version. Pass `platform` through `check_release`; `launcher/update.cpp` keeps the default (Windows).
- [ ] **Step 4: Run** the same command. Expected: `test_update_release`, `test_update_download`, `test_update_controller`, `test_update_end_to_end.py` PASS (update fixtures whose tag/asset versions no longer matter).

### Task 2: Portable path keys, platform manifest, protected names

**Files:**
- Modify: `updater/release.h`, `updater/release.cpp`; Test: `tests/test_update_release.cpp`

**Interfaces:**
- Produces: `Result<Manifest> parse_manifest(std::string_view json, Platform platform = Platform::windows);` — Windows requires `UC.EXE`, `UC_LAUNCHER.EXE`, `UC_UPDATER.EXE` and helper `UC_UPDATER.EXE` (unchanged); `steamdeck` requires `UC` and `UC_LAUNCHER`, and `helper_files` must be an empty array.
- `path_key` compiles on Linux (`#ifdef _WIN32` around `MultiByteToWideChar`/`LCMapStringEx`; elsewhere decode UTF-8 to `std::wstring` code points, reject invalid UTF-8, upper-case ASCII only; keep the same forbidden-character and component rules). Add `// shortcut: Linux case folding is ASCII-only, manifests are produced by our own scripts; upgrade if third-party manifests appear`.
- `validate_managed_path` protects the full list from Global Constraints (add `STEAM-ENV.TXT`, `NEXT-RUN.SH`, `LAUNCHER-STATE.JSON`).

- [ ] **Step 1: Write failing tests**: `manifest_steamdeck_requires_uc_and_launcher`, `manifest_steamdeck_rejects_helpers`, `manifest_windows_unchanged`, `managed_path_rejects_launcher_state_and_steam_env`, `path_key_rejects_invalid_utf8`, `path_key_equal_for_case_variants_ascii`.
- [ ] **Step 2-4:** run -> FAIL; implement; run `ctest --test-dir build -R "update"` -> PASS (all existing update tests still green).

### Task 3: Deck release builder and release checklist

**Files:**
- Create: `scripts/build_release_deck.py`; Modify: `scripts/build_release.py` (parametrize `write_manifest` required set; print the checklist); Test: `tests/test_build_release_deck.py`

**Interfaces:**
- Produces: `build_release_deck.build_deck_release(repo: Path, version: str, stage: Path) -> Path` — takes an installed Deck tree (`cmake --install build-linux-znver2 --prefix <stage>`), writes `release.json` (format 1, `helper_files: []`), checks the required set `{"uc","uc_launcher","assets/launcher/graphite-train.png","assets/launcher/graphite-brand.png","tools/wit/wit"}` and the protected-name rule, forbids user data, and writes `<repo>/UC-SteamDeck-<version>.zip` atomically (final name only on success). Executables (`uc`, `uc_launcher`, `tools/wit/wit`) get Unix mode 0o755 in `ZipInfo.external_attr`, others 0o644.
- Produces: `checklist(version: str) -> str` shared by both scripts: both archives attached, names `UC-Windows-<ver>.zip` / `UC-SteamDeck-<ver>.zip`, published as Latest, all assets uploaded.

- [ ] **Step 1: Write failing tests** (style of `tests/test_build_release.py`, fake stage dirs): `test_deck_zip_has_manifest_and_modes`, `test_deck_rejects_missing_required_file`, `test_deck_rejects_user_data`, `test_deck_failure_leaves_no_final_zip`, `test_checklist_mentions_latest_and_both_archives`.
- [ ] **Step 2:** `python -m pytest tests/test_build_release_deck.py tests/test_build_release.py -q` -> FAIL.
- [ ] **Step 3: Implement** reusing `files_in`, `sha256`, `write_manifest`, `package_release` from `build_release.py` (add an optional `modes` argument).
- [ ] **Step 4:** same command -> PASS.

### Task 4: POSIX update primitives (sha256, files) and shared download code

**Files:**
- Create: `updater/sha256.h`, `updater/sha256.cpp` (portable), `updater/posix/files_posix.h`, `updater/posix/files_posix.cpp`, `updater/download_common.cpp`
- Modify: `updater/download.cpp` (move `allowed_download_url`, `ascii_url`, `status_error`, `check_release` into `download_common.cpp`; WinHTTP code stays), `updater/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/linux/CMakeLists.txt`
- Test: `tests/test_update_sha256.cpp`, `tests/test_update_files_posix.cpp`

**Interfaces:**
- Produces: `class Sha256 { public: void update(std::span<const unsigned char>); std::string finish_hex(); };` and `std::string sha256_hex(std::string_view)`.
- Produces (namespace `uc::update`, POSIX only): `Error file_error(std::string action, int code = errno); Result<std::string> read_text(const fs::path&, uint64_t limit); Result<std::string> sha256_file(const fs::path&); Status remove_owned_tree(const fs::path&); Status replace_file(const fs::path& from, const fs::path& to); /* rename(2), then fsync of the parent */ Status fsync_file(const fs::path&); fs::path utf8_path(std::string_view);` — all open files with `O_NOFOLLOW`; `remove_owned_tree` never follows symlinks.
- `updater/CMakeLists.txt`: keep every existing Windows target unchanged under `if(WIN32)`; under `else()` define `uc_update_core` (`release.cpp`, `sha256.cpp`, `download_common.cpp`, posix files; links `nlohmann_json`, `uc_miniz`) with the same target name so launcher code links either way.

- [ ] **Step 1: Write failing tests**: `test_update_sha256` (vectors `""` = `e3b0c442...b855`, `"abc"` = `ba7816bf...15ad`, one-million `a` = `cdc76e5c...c7c0`, incremental == one-shot; runs on the PC); `test_update_files_posix` (Linux/Deck only: symlink as final component is refused, `remove_owned_tree` does not delete a symlink target, `replace_file` replaces atomically, `read_text` honours the limit).
- [ ] **Step 2:** run -> FAIL (`ctest -R test_update_sha256`; Deck: `deck.py run .../test_update_files_posix`).
- [ ] **Step 3: Implement**; verify the Windows build is unchanged (`build.bat --no-install` green).
- [ ] **Step 4:** PASS on both.

### Task 5: POSIX package preparation

**Files:**
- Create: `updater/posix/package_posix.cpp`; Modify: `updater/CMakeLists.txt`, `tests/linux/CMakeLists.txt`; Test: `tests/test_update_package_posix.cpp`, fixture generator inside the test (writes zips with miniz)

**Interfaces:**
- Implements the existing `updater/package.h` for POSIX with `Manifest` parsed using `Platform::steamdeck`: `prepare_package(zip, release, staging, stop)`, `verify_package`, `validate_target_path`, `sha256_file`. Zip entry rules as in `package.cpp`: no absolute paths, `..`, symlink entries (Unix mode `0120000`), duplicates, file/dir collisions, protected paths, size and entry-count limits; the manifest in the zip must have `version == release.version` and list exactly the zip's files with matching sizes and SHA-256; no extra files. After extraction set `0755` on `uc`, `uc_launcher`, `tools/wit/wit` regardless of stored attributes.

- [ ] **Step 1: Write failing tests**: `good_package_extracts_and_sets_exec_bits`, `rejects_traversal`, `rejects_symlink_entry`, `rejects_extra_file`, `rejects_missing_file`, `rejects_hash_mismatch`, `rejects_version_mismatch`, `rejects_protected_path`, `cancel_stops_extraction`.
- [ ] **Step 2:** `deck.py run .../test_update_package_posix` -> FAIL. **Step 3:** implement (port the checks of `updater/package.cpp`; replace Windows handles with `files_posix`). **Step 4:** PASS on the Deck.

### Task 6: POSIX install transaction with recovery

**Files:**
- Create: `updater/posix/transaction_posix.cpp`; Modify: `updater/CMakeLists.txt`, `tests/linux/CMakeLists.txt`; Test: `tests/test_update_transaction_posix.cpp`

**Interfaces:**
- Implements `updater/transaction.h` for POSIX: `plan_install`, `apply_install`, `finish_install`, `recover_install`, `pending_install` with the same semantics as the Windows version: backups under `<install>/.uc-update/<id>/backup` on the same volume, durable journal written before any change (each step recorded before and after), files replaced with `rename`, files removed only if the old manifest listed them, unknown user files kept, a new managed path occupied by an unmanaged file stops the plan. Check free space before applying.
- Fault injection: reuse the hook pattern of `updater/transaction_faults.cpp` (`UC_UPDATE_FAULT_AT=<step>` or the existing mechanism), one fault per step.

- [ ] **Step 1: Write failing tests** on a fake install in a temp dir: `update_replaces_removes_adds`, `user_data_untouched`, `unmanaged_conflict_stops_before_changes`, `fault_at_every_step_rolls_back_or_recovers` (loop over all steps; afterwards tree equals the original), `recover_after_simulated_kill` (journal left, `pending_install` true, `recover_install` restores), `recover_twice_is_safe`, `no_space_reports_before_changes`.
- [ ] **Step 2-4:** run on the Deck -> FAIL; implement; PASS.

### Task 7: libcurl transport and POSIX download

**Files:**
- Create: `updater/posix/download_curl.cpp`; Modify: `updater/download.h` (declare `struct CurlTransport final : Transport` under `#ifndef _WIN32`), `updater/CMakeLists.txt` (`uc_update_download` on Linux links `CURL::libcurl`; find it in the cross sysroot), `tests/linux/CMakeLists.txt`; Test: `tests/test_update_download_posix.cpp`

**Interfaces:**
- `CurlTransport::get_metadata(stop)` and `download(release, sink, stop)` with the limits of Global Constraints; implement `download_release(...)` (declared in `download.h`) for POSIX: write `<dest>.part` with `O_EXCL|O_NOFOLLOW`, check size, SHA-256, `replace_file` to the final name, delete the part on failure. libcurl options: `CURLOPT_PROTOCOLS_STR "https"`, `CURLOPT_REDIR_PROTOCOLS_STR "https"`, `CURLOPT_FOLLOWLOCATION 0` (redirects followed by hand through `allowed_download_url(url, redirect=true)`, max 5), no cookies, connect timeout 5 s, low-speed abort, total deadlines 30 s (metadata) and 1800 s (archive), stop token checked in the write/progress callbacks, User-Agent `UC-Launcher-Update/1`, `Accept` headers as in the WinHTTP version.
- [ ] **Step 1: Write failing tests** with a fake `Transport` (no network): `download_release_checks_size_and_hash`, `download_release_rejects_untrusted_url`, `download_cancel_removes_part_file`, `download_existing_destination_is_busy`, plus `allowed_download_url` cases on the PC in `test_update_download.cpp`.
- [ ] **Step 2-4:** Deck run FAIL -> implement -> PASS. Network smoke (read-only, once): a tiny `--check` mode in the test calls `check_release(CurlTransport, Version{0,0,0}, {}, Platform::steamdeck)` against GitHub and prints the result; expect either a release or `invalid_release` (no Deck archive yet), never a crash.

### Task 8: Portable `UpdateController` and update window on Linux

**Files:**
- Modify: `launcher/update.h` (guard `update_process.h`, `Helper` and the helper member with `#ifdef _WIN32`), `launcher/update.cpp` (split: Windows-only `prepare`/`discard`/ctor/dtor stay; state machine `start/accept/cancel/postpone/poll/busy/blocks_game_launch/close_requested/handoff/launch` moves to `launcher/update_common.cpp`), `launcher/CMakeLists.txt` (`uc_launcher_update` and `uc_update_ui` on Linux), `launcher/update_ui.cpp` (no Windows headers)
- Create: `launcher/update_posix.cpp`; Test: `tests/test_update_controller_posix.cpp`

**Interfaces:**
- POSIX `UpdateController(fs::path root, std::unique_ptr<update::Transport> = CurlTransport, Platform = steamdeck)`; same public API as Windows. `prepare` on POSIX: download -> `prepare_package` -> `plan_install` -> `apply_install` -> `finish_install` in the worker thread, then state `ready_to_close`; the launcher's main loop re-executes itself (`execv` of `/proc/self/exe`) when `close_requested()`.
- Produces: `bool recover_pending_update(const fs::path& install)` in `launcher/update_posix.cpp` — scans `<install>/.uc-update/*`, runs `recover_install` for pending ones; called before anything else at launcher start.

- [ ] **Step 1: Write failing tests** with a fake transport and a temp install: `check_finds_newer_deck_archive`, `equal_version_disables`, `accept_installs_and_reports_ready_to_close`, `failure_keeps_old_install_and_reports_error`, `cancel_cleans_staging`, `recover_pending_update_restores_old_files`, `network_error_does_not_block_play` (fake transport returns a network error / HTTP 403: state `disabled`, `blocks_game_launch()` false, no modal).
- [ ] **Step 2-4:** Deck run FAIL -> implement -> PASS; Windows `test_update_controller`, `test_update_ui` still PASS (`build.bat --no-install`).

### Task 9: Deck launcher program skeleton, artwork, snapshot mode

**Files:**
- Create: `launcher/deck/main_deck.cpp`, `launcher/deck/paths_posix.cpp` (`executable_dir()` via `/proc/self/exe`, `chdir` to it), `launcher/graphite_posix.cpp` (`load_banner` for PNG through vendored `stb_image.h` fetched by `FetchContent` with a pinned URL and SHA-256 like the other deps, a licence file in `licenses/stb`)
- Modify: `launcher/graphite.cpp` / `launcher/CMakeLists.txt` (if `combo`, `slider_int`, `tooltip`, `draw_umbrella`, `draw_navigation_icon` live only in the Windows-only `graphite.cpp`, move them to a portable file and keep WIC/dialog functions Windows-only), `launcher/CMakeLists.txt` (`add_executable(uc_launcher ...)` for non-WIN32, assets copied to `assets/launcher`)

**Interfaces:**
- `uc_launcher [--snapshot <dir>] [--window WxH]`: normal run opens fullscreen at the display size; `--snapshot` opens windowed (default 1280x720), draws every tab for a few frames, writes `<dir>/<tab>.bmp` (use `SDL_RenderReadPixels`, as `Ui::capture` does in `launcher/main.cpp`), exits 0.
- Layout and look as the Windows launcher: sidebar (`graphite-brand.png` regions 0,0-342,446 and 0,446-304,502), hero `graphite-train.png` ("cover" fit), graphite background, accent, three tab buttons (Game, Graphics, Controls), language combo at the bottom. Before `SDL_Init`: `unsetenv("SDL_GAMECONTROLLER_IGNORE_DEVICES")`, hints `SDL_HINT_JOYSTICK_HIDAPI_STEAMDECK=1`, `SDL_HINT_JOYSTICK_HIDAPI=1`. Input: ImGui keyboard+gamepad navigation, SDL touch as mouse, right trackpad hard press as click (port the logic of `DeckMenu::event` in `game/deck_menu.cpp`, extract to a shared function if it separates cleanly).
- Reads/writes `config.json` next to the executable with `uc::load_config` / `uc::options_from_config` / the same save path as Windows (`launcher/main.cpp` `save` lambda; move to a shared helper if it is not Windows-specific).

- [ ] **Step 1: Write failing test** `tests/test_launcher_deck_snapshot.py`-style script run on the Deck (`scripts/linux/deck.py` helper `run`): launch with `--snapshot`, expect three BMPs of the requested size, non-uniform (std-dev above a threshold), exit 0; run it once with the working directory set to `/` to prove the launcher finds its own files. Also a ctest `test_launcher_locale` check that every new key exists in all 13 languages (existing test enforces this once keys are added).
- [ ] **Step 2:** build Linux (`scripts/linux/build.ps1`), push, run -> FAIL. **Step 3:** implement. **Step 4:** run on the Deck, open the BMPs (Read tool) and check layout; clean the Deck.

### Task 10: Graphics and Controls tabs from the registry

**Files:**
- Create: `launcher/deck/settings_pages.h`, `launcher/deck/settings_pages.cpp`; Modify: `launcher/settings_registry.cpp` (only if a needed row lacks the `deck_launcher` bit), `tests/test_settings_registry.cpp`

**Interfaces:**
- Produces: `std::vector<TabPages> deck_launcher_tabs();` where Game = `Page::game`, Graphics = `graphics`+`effects`, Controls = `controls`+`gamepad`+`buttons`+`reticle` (only pages `settings::pages_for(settings::deck_launcher)` returns); `void draw_settings_page(uc::Options&, const uc::settings::Env&, uc::settings::Page, DrawContext&)` draws toggles/choices/sliders/colours from `items_for(deck_launcher, page)` and the bindings editor (`uc_binding_editor`) for `Page::buttons`. Check `game/deck_menu.cpp` (`deck_rows`, buttons page) first and extract shared drawing instead of duplicating when it separates cleanly.
- `Env`: `vendor_upscalers=false`, monitor size from SDL, `features_known` from `query_features` (Task 13).

- [ ] **Step 1: Write failing tests** in `test_settings_registry.cpp`: `deck_launcher_has_no_multiplayer_calibration_dlss_items`, `deck_launcher_controls_has_gyro_items`, `deck_launcher_tabs_cover_all_deck_launcher_pages`.
- [ ] **Step 2-4:** run -> FAIL -> implement -> `ctest -R settings_registry` PASS; Deck snapshot shows the tabs; changing a value in a unit-level `draw` harness is not needed, the `step/flip` functions are already tested.

### Task 11: File browser (model and screen)

**Files:**
- Create: `launcher/deck/file_browser.h`, `launcher/deck/file_browser.cpp`, `launcher/deck/file_browser_ui.cpp`; Test: `tests/test_file_browser.cpp` (host and Deck)

**Interfaces:**
- `enum class PickMode { folder, image };` `struct BrowserEntry { std::string name; bool directory; };`
- `class FileBrowser { public: FileBrowser(std::filesystem::path start, PickMode, std::vector<std::filesystem::path> shortcuts); const std::filesystem::path& current() const; const std::vector<BrowserEntry>& entries() const; bool enter(size_t index); bool up(); bool go(const std::filesystem::path&); std::optional<std::filesystem::path> choice(size_t index) const; /* folder mode: current folder when index == npos; image mode: the file at index when its extension is in the Wit list of choose_image_file */ const std::string& error() const; };` Entries: folders first, then files (image mode: only image files), sorted case-insensitively, hidden names hidden; unreadable folder -> `error()` set and `current()` unchanged.
- `std::vector<std::filesystem::path> default_shortcuts()` (home, every `/run/media/*/*` mount, `/run/media/mmcblk0p1` style cards, last used folder from `launcher-state.json`); `load_last_folder / store_last_folder` for that file (atomic write).
- UI: list with the focused row highlighted, up button, shortcuts row, "Select this folder" button in folder mode, gamepad (D-pad, A enter/select, B up/back) and touch.

- [ ] **Step 1: Write failing tests**: `lists_folders_first_sorted`, `image_mode_filters_extensions`, `enter_and_up_stay_inside_root_of_filesystem`, `unreadable_folder_sets_error_keeps_current`, `folder_choice_is_current`, `last_folder_roundtrip_survives_missing_file`, `non_ascii_and_space_names` (Cyrillic folder).
- [ ] **Step 2-4:** `ctest -R test_file_browser` FAIL -> implement -> PASS; Deck snapshot of the browser screen (`--snapshot` also renders `browser.bmp`).

### Task 12: Disc image unpack on Linux with Wit

**Files:**
- Create: `launcher/image_unpack_posix.cpp`; Modify: `launcher/image_unpack.h` (portable private part: `#ifdef _WIN32 int wit(const std::wstring&, ...) #else int wit(const std::vector<std::string>&, std::string& output, bool track_progress) #endif`, process handle as `long`/`void*`), `launcher/CMakeLists.txt` (Linux branch: fetch `https://wit.wiimm.de/download/wit-v3.05a-r8638-x86_64.tar.gz` with a pinned SHA-256 and a `build/deps` mirror check like the Windows one; install `tools/wit/wit` and `licenses/wit/*`), `tests/linux/CMakeLists.txt`; Test: `tests/test_image_unpack_posix.cpp` with a fake `wit` shell script

**Interfaces:**
- Same class and semantics as Windows `ImageUnpack`: `ID6` must be `RBUE08` (else `wrong_image`), image not inside `<root>/game` (`image_in_game_folder`), free space on the volume of `<root>` at least `required_bytes` counting the old `game/` as free (`no_space`), `extract <image> <root>/game --progress --pmode name`, `main.dol` must exist afterwards, progress through `parse_percent`, cancel kills the process group and removes the partial folder, old `game/` is replaced only after a successful identification and a failed or cancelled run leaves no `game/` behind. Wit runs with `LC_ALL=C.UTF-8`, `posix_spawn`, own process group, stdout piped.
- Wit path default `<root>/tools/wit/wit`.

- [ ] **Step 1: Write failing tests** (Deck/Linux, fake wit script prints `RBUE08` for `ID6`, creates `main.dol` and prints `17%` lines for `extract`): `unpack_success_replaces_game`, `wrong_id_keeps_old_game`, `image_inside_game_folder_refused`, `no_space_before_any_change`, `cancel_leaves_no_game_folder`, `wit_missing_reports_wit_missing`, `path_with_spaces_and_cyrillic`.
- [ ] **Step 2-4:** Deck run FAIL -> implement -> PASS. Windows `test` for `image_unpack` still PASS.

### Task 13: Game tab, Play and update wiring

**Files:**
- Create: `launcher/deck/game_tab.cpp`, `launcher/deck/exec_game.cpp`; Modify: `launcher/deck/main_deck.cpp`, `launcher/translations.inc`

**Interfaces:**
- Game tab: game path row with "Folder" and "Disc image" buttons opening the browser (Task 11), status line (found / not found / image selected, same logic and texts as Windows `game_found`), unpack progress with cancel (Task 12), toggles from the registry, Play button (disabled while unpacking, while an update is being prepared or `blocks_game_launch()`).
- `[[noreturn]] void exec_game(const std::filesystem::path& install, const std::vector<std::string>& extra)` in `exec_game.cpp`: `chdir(install)`, environment as `scripts/linux/uc-deck.sh` (unset `SDL_GAMECONTROLLER_IGNORE_DEVICES`, `SDL_JOYSTICK_HIDAPI_STEAMDECK=1`, `SDL_JOYSTICK_HIDAPI=1`), `execv("./uc", {"uc","--window-mode","borderless","--skip-intro", extra...})`; if `execv` fails the launcher shows the error and stays open. `uc-deck.sh` stays for the harness and terminal.
- Play: save options (`config.json`), run `query_features` once per start in a background thread for `Env`, then `exec_game`.
- Start-up order in `main_deck.cpp`: `chdir`, `recover_pending_update`, read `release.json`, `UpdateController::start`, window; update modal via `draw_update_modal`; `close_requested()` -> `execv` self.
- New strings: all keys the screens use, translated into the 13 languages; `test_launcher_locale` passes.

- [ ] **Step 1: Write failing tests**: `exec_game` argument/env builder as a pure function `build_game_command(install, extra) -> struct { std::vector<std::string> argv; std::vector<std::pair<std::string,std::optional<std::string>>> env; }` tested on the PC and the Deck: `game_command_matches_uc_deck_sh`, `extra_args_appended`; `game_found` helper tests with temp folders.
- [ ] **Step 2-4:** run -> FAIL -> implement -> PASS; Deck snapshot shows the Game tab in all three states (found, missing, image selected).

### Task 14: Packaging, install rules, entry point

**Files:**
- Modify: `CMakeLists.txt` / `game/CMakeLists.txt` / `launcher/CMakeLists.txt` (Linux `install()`: `uc`, `uc_launcher`, `data/` (as `scripts/linux/deck.py stage` copies it), `assets/launcher/graphite-*.png`, `tools/wit`, `licenses/`, `THIRD_PARTY_NOTICES.md`), `scripts/linux/deck.py` (`stage` also stages `uc_launcher`, assets and `tools/wit`), `scripts/linux/build.ps1` (print how to run `build_release_deck.py`)
- Create: `docs/steamdeck-install.md` (unzip, add `uc_launcher` as a non-Steam game, first run, where the game folder goes, updating, release checklist link)

**Interfaces:** `cmake --install build-linux-znver2 --prefix <stage>` produces the tree `build_deck_release` expects (Task 3).

- [ ] **Step 1: Write failing test**: extend `tests/test_build_release_deck.py` with `test_install_tree_from_cmake_has_required_files` (skipped when `build-linux-znver2` is absent).
- [ ] **Step 2-4:** run -> FAIL -> implement -> PASS with a real `cmake --install` into a temp folder.

### Task 15: End-to-end check on the Deck, full build, deploy

- [ ] **Step 1:** Linux build, `python scripts/linux/deck.py stage` + staged `uc_launcher`; on the Deck run `uc_launcher --snapshot` (windowed 1280x720), read all BMPs.
- [ ] **Step 2: Update dry run:** build `UC-SteamDeck-0.0.1.zip` and `-0.0.2.zip` with `build_deck_release`; run `test_update_controller_posix` with them as fixtures (install 0.0.1 -> 0.0.2, then a forced failure -> rollback) on the Deck.
- [ ] **Step 3:** Owner acceptance list (give to the owner, mark unverified items in the closing message): Game Mode start from Steam, D-pad/stick/touch/trackpad navigation, folder pick on SD card, image unpack, settings change persists, Play starts the game, offered update installs and relaunches.
- [ ] **Step 4:** `python scripts/linux/deck.py clean --all` if test leftovers exist; `build.bat` (full, deploys to the game folder per the owner's rule); `ctest --test-dir build --output-on-failure` green.
- [ ] **Step 5:** Final review on Opus (`model: "opus"`), then report: what was verified on the PC, on the Deck, and what only the owner can check.

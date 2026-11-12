# Game Language Implementation Plan

> **For agentic workers:** Follow the active preset's execution path and approved task briefs. Use subagent-driven-development when delegating. Preserve design/plan approval; no redundant execution-method choice. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Use launcher language for available game text/fonts with English fallback; force English in netplay and deterministic/demo modes.

**Architecture:** Reuse shared config resolution and native guest resource selection for ordinary play. Prove DOL mappings and actual asset opens first. A single tested startup policy selects English for state-equality modes without changing full-strength desync checks. User explicitly deferred independent network localization.

**Tech Stack:** Existing C++ runtime/HLE, CMake/Windows dual-CPU build, Python uctest harness.

**Spec:** `docs/superpowers/specs/2026-11-12-game-language-design.md` (approved conversational design, including the user's network-language correction).

## Global Constraints
- No separate game-language option; resolve `launcher.language` and `system` using existing code.
- Do not rewrite the disc, change region/application identity, alter real saves, weaken hashes, commit, publish or deploy.
- Deterministic offline runs, demo recording/playback, and netplay force English game resources. Launcher/port UI language remains local.
- Preserve language-specific mods and existing overlay ordering.
- Commands run Idle; live runs use existing harness, isolated config/saves, 1280x720 nonactivating windows, unlimited FPS.
- External read-only disc scope: `S:\Games\Umbrella Chronicles\game\DATA`.

## Review Focus
- Incomplete language packs: missing fonts/layouts must choose consistent English rather than mixed or missing assets.
- Regional English suffixes: native US English and patched English resources remain unchanged.
- Localized allocations: differing language resource bytes/sizes must not contaminate shared simulation or be hidden by checksum omissions.
- Mod ordering: selected-language text mods must read the correct base and retain precedence.
- Rollback/replay: local presentation must survive checkpoint restore while deterministic demos remain canonical.

## Task 1: Establish language contracts
**Files:** read-only source and external DATA; no changes.
**Interfaces:** verify SCGetLanguage at `0x802AEF48`, selector `0x80242984`, required assets, `game_checkpoint_hash` and existing two-game checks.
- [x] Inspect DOL language jump table, suffix/font tables and actual disc inventory.
- [x] Trace checkpoint/hash and local rendering paths; identify whether native language changes can be isolated.
  - Native per-peer asset loading is not state-equal: texts and localized ARCs/fonts have differing sizes and bytes in hashed RAM. The user subsequently approved forced English netplay; no language negotiation or rendering subsystem is needed.
  - A rescue criterion (3) read-only probe examined the unknown full-string renderer hook lifecycle. It made no edits and was interrupted after the scope revision; preliminary evidence is sufficient to defer this route, not claim it implemented.
  - Parent baseline: `pwsh -NoProfile -File scripts/ctest-hidden.ps1 -j 30`, exit 0, 255/255 passed. Parent xref independently confirmed the selector's equal -> US index2 branch and actual `mes_sp` existence.
- [x] Record evidence and choose the smallest viable implementation; never treat different asset hashes as harmless without a presentation boundary. Evidence: `docs/superpowers/plans/2026-11-12-game-language-task-1-results.md`; network scope revised explicitly by user to English.

## Task 2: Native language selection and real integration probe
**Files:** new `runtime/game_language.{h,cpp}`, `runtime/hle/game_language.cpp`, `runtime/CMakeLists.txt`, `project/hle_map.txt`, `game/uc_main.cpp`, new `tests/test_game_language.cpp`, `tests/CMakeLists.txt`, new `scripts/uctest/language.py`.
**Interfaces:** `GameLanguage select_game_language(std::string_view, const DvdFs&)` with explicit guest index/suffix/font/availability result; `void game_language_init(uint32_t)` before guests start; `hle_GameLanguageSelect` at `0x80242984` stores selected index at object+4. Ruling from DOL evidence: SCGetLanguage alone is insufficient on US discs, where French/Spanish regional branches still select US text. Override the resource selector, not disc identity or global console locale.
- [x] Write failing mapping/availability tests with fixture files and literal expected asset paths; test missing message, each font directory, required localized layouts, unsupported locale and English fallback.
- [x] Implement minimum selection/HLE and use the already resolved launcher language before guest startup. Force English for deterministic/det-out/demo/netplay modes through a tested helper; keep port UI language local.
  - Worker RED: missing implementation link failure. Worker GREEN: focused target/test and startup object compile. Parent independently executed `build/tests/test_game_language.exe` from build, exit0, all13 behavior checks passed. This initial focused checkpoint was later superseded by the completed integration evidence below.
- [x] Add harness test using isolated config and normal Game launch: Spanish actual message/S font opens, English/unsupported fallback, deterministic Spanish-config English opens. Capture Spanish accented text.
- [x] Run focused tests, `build.bat --no-install`, and real language harness. Record red/green evidence.

## Task 3: Forced-English network integration
**Files:** `scripts/uctest/language.py`; existing `scripts/uctest/netplay.py` or `netplay_history.py` may be reused without changes. No checksum/protocol or renderer rewrite.
**Interfaces:** existing `Game` isolated config launch and canonical `game_checkpoint_hash`/rollback behavior.
- [x] Test two real peers with English/Spanish launcher configs through title, menus and gameplay. Assert both load native English messages/fonts, original confirmed hashes match, and controls work.
- [x] Exercise existing rollback/history checks or report explicitly if not run. Any failure must be diagnosed before acceptance.
- [x] Keep independent network localization deferred per the user's scope revision; no forced host-language negotiation.

## Parent verification checkpoint
- Initial full CTest: 255/255 passed before feature work.
- Runtime language test: parent independently ran from build, exit0, 13 checks passed.
- Harness unit tests: parent independently ran `python -I tests/test_uctest_language.py <repo>`, exit0, 10 checks passed; synthetic fixtures are not live proof.
- Scope deviations ruled: build cache C/CXX flags were unexpectedly empty; worker restored standard Windows Release defaults through normal CMake configure only. This is local generated build state, not a committed build-policy change. Parent independently observed active compiler/build children at Idle.
- Initial Task3 worker failed during its running build. Its job `pwsh-65f92de458f64282` is not accessible from the parent, but parent inspected the surviving process chain and logs; no duplicate build was started. Recovery worker owns the existing build now.
- Build log shows AVX2 compilation/link complete, then 257 tests passed after one automatic retry. Initial `test_update_end_to_end::test_success_through_junction` failed in cleanup `TerminateProcess` assertion (`build/tests-junit.xml:1071-1080`); retry passed7.55s. Record this flake, not a clean first-pass suite.
- Recovered build log reached stage6 staging and stage7 `Skipping copy (--no-install)`; both CPU variants staged. Original top-job exit is unavailable after worker failure, so log/process proof is recorded instead of fabricating an exit code.
- Parent independently reran full hidden CTest with `pwsh -NoProfile -File scripts/ctest-hidden.ps1 -j 30`, exit0, 257/257 passed firsttry,31.85s. This includes the updater test that initially flaked.
- Parent confirmed generated caller invokes `hle_GameLanguageSelect` and project/hle.cfg contains the binding. This initial checkpoint was later superseded by the final review and live proof below.

## Final verification and review checkpoint
- Parent independently reran actual Spanish gameplay: `python -m uctest.language --case es`, exit0; report `build/autotest/language-1794489104814253100/report.json`. Parent title image inspection confirms `PULSA CUALQUIER BOTÓN` and correct Ó.
- Final full CTest after harness repairs: exit8,256/257pass; `test_sched.cpp:49` failed `wake_one(0x1000)==mid`. Earlier complete257run passed. Isolated `test_sched --repeat until-fail:20` then passed20/20,exit0,12.17s. Source investigation identifies a pre-existing10ms wall-clock readiness assumption (no barrier, host threads start during sleep window). Actual failing-run stall is not measured. Scheduler/clock/test source unchanged; no unrelated repair.
- Required final review returned CHANGES_REQUIRED with one medium finding: complete-pack checks omit Atract/HomeButton. Fix wave1 added both Atract aspect variants, native HomeButton mapping and banner variant, with missing/empty/directory fixtures for every language. Parent inspected current two changed source/test files and independently ran14checks exit0. Repair worker fullbuild exit0,257/257pass firsttry, both CPU variants staged. Evidence `.superpowers/sdd/2026-11-12-game-language/repair-report.md` and `build/build-repair.out.log`.
- Parent post-repair full hidden CTest exit0,257/257firsttry30.27s; independent Spanishgameplay exit0 at `build/autotest/language-1794490534912610700/report.json`.
- Scoped final re-review APPROVE, no findings; original dependency-check blocker ADDRESSED. Unchanged startup/HLE/mods/replay/netplay review reused. Non-blocking exclusions: no Linux/Deck/compat live, no DE/FR/IT/JA visual runs, no full1200retrace det-out or in-mission lag spike. Native Atract indirect/banner caller unpinned but tables/strings/inventory and actualen/esreads cover this bounded contract.

## Task 4: Acceptance and docs
**Files:** docs/launcher-graphite.md, relevant English/Russian text-modding docs, docs/spikes/steamdeck-windows-validation-backlog.md; task code fixes only through workers.
- [x] Update docs only to behavior actually proved, including restart and demo/network limits.
- [x] Parent inspects current affected/connected code and scoped diff, independently reruns decisive checks and full CTest (hidden desktop).
- [x] Required final review: guest resource selection crosses startup/mods/replay/netplay; inspect uncovered allocation and rollback risk. Review pending is not full acceptance.
- [x] Record exact status and remaining gaps without installing to the owner's game.

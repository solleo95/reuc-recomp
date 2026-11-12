# Final review fix wave — complete language-pack dependencies

## Finding and objective
Final review CHANGES_REQUIRED, one medium blocker: runtime/game_language.cpp complete() omits localized Atract and HomeButton archives. Fresh real Spanish runs read Data/layout/atract_4_3_sp.arc and Data/HomeButton2/homeBtn_SPA.arc; deleting either from an otherwise complete pack still returns installed=true. Fix complete-pack fallback, not resource aliasing. Full review inputs: docs/superpowers/plans/2026-11-12-game-language-review-input.md. Re-review will be scoped to this repair.

## Allowed files
Modify runtime/game_language.cpp and tests/test_game_language.cpp only. Update .superpowers/sdd/2026-11-12-game-language/progress.md and repair-report.md. Generated/build/dist outputs via normal build/tests allowed. No other source/doc/installed-file changes, no commit, no subagents. Read-only external disc path S:\Games\Umbrella Chronicles\game\DATA.

## Interface and mapping
Keep existing public API and startup policy. Verified mappings: Atract uses `Data/layout/atract_{4_3|16_9}_{jp|us|ge|fr|sp|it}.arc` for ja/en/de/fr/es/it respectively (DOL formatter/suffix strings, real en/es opens; indirect caller not pinned). HomeButton table803F5388 consumed by selector800A762C: ja `Data/HomeButton2/homeBtn.arc`, en `homeBtn_ENG.arc`, de `homeBtn_GER.arc`, fr `homeBtn_FRA.arc`, es `homeBtn_SPA.arc`, it `homeBtn_ITA.arc`. Same guest index; no localized fallback in native selector. Banner table803F9450: JP `Data/sysres/savebanner_jp.arc`, all others `savebanner_us.arc` (caller not pinned, table proof). Check these localized dependencies plus both aspect variants. Common shared HomeButton files do not distinguish language packs: leave global disc corruption outside this localized-pack check; do not invent optional/fallback semantics. Existing title_us is shared outside Japanese. Keep check before overlays and existing fallback semantics.

## Checks
Test-first: extend full_disc fixture with independently literal required files, then for each newly required file remove it, make empty, replace by directory, reopening DvdFs each time. Expect English fallback; observe RED on old complete(), then implement minimal table/check and GREEN. Ensure Japanese special font0-only behavior and English/deterministic defaults unchanged. Verify resolved absolute fixture delete paths, run tests from build.

Run focused test, then build.bat --no-install at Idle with max(1,logicalCPU-2) jobs and capture its actual exit code. No other build is running. Parent will independently run decisive tests and Spanish harness; do not duplicate live runs unless requested. Full project CTest through standard build must report failures, not hide them. Known unrelated test_sched wake-order wall-clock flake: latest parallel suite256/257, isolated20/20pass, prior suite257/257; scheduler untouched, no unrelated repair authorized. Updater cleanup also flaked once then passed. Record actual results.

## Evidence and deliverable
This is final fix wave1/3 maximum. Record exact RED/GREEN commands/results, actual files, final build exit and output paths in repair-report.md and ledger. Return concise DONE/DONE_WITH_CONCERNS/BLOCKED. No broad cleanup or new translation subsystem.

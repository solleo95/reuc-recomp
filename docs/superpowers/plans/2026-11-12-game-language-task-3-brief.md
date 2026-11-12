# Task 3 — Build and real game language acceptance

## Objective
Verify current language selection through actual builds and the existing game harness. Approved scope: ordinary game uses launcher language when the disc contains its required pack; missing languages use English; netplay, deterministic/det-out and demos always use English. Local port UI remains local. Read spec `docs/superpowers/specs/2026-11-12-game-language-design.md` and current Task 2 report before work.

## Allowed files
Create `scripts/uctest/language.py`; modify only if needed its test/wiring `tests/test_uctest_language.py`, `tests/CMakeLists.txt`. Maintain `.superpowers/sdd/2026-11-12-game-language/progress.md` and `task-3-report.md`. Normal generated/build/dist outputs allowed through existing build/test commands. Do not edit runtime/game sources; report defects to parent. Read-only external disc path: `S:\Games\Umbrella Chronicles\game\DATA`. Never write installed game or saves/config. No commits or subagents.

## Interfaces
Use existing `uctest.client.Game` and `steps`, with preseeded `<run>/config.json` setting `launcher.language`. Existing Game passes --config, windowed1280x720, --fps-limit0, copied saves, no system input, nonactivating visible window. Use --log-dvd and inspect actual open logs, not only startup selection diagnostics. Reuse netplay.coop_menu and rollback_log or existing netplay_history patterns; no full performance benchmarking suite required.

## Checks
First unit-test any nontrivial new harness logic against controlled real inputs (log parser/config prep), observe RED then minimal GREEN. Avoid mock-only assertions and source-string tests. Run at Idle priority, use max(1,logicalCPU-2) parallel build jobs. Confirm no other build owns same trees before running `build.bat --no-install` once; do not install/deploy. Build script stages AVX2 and compat variants and tests.

Actual harness cases:
- Spanish config: reach title and gameplay; prove mes_sp.txtres, S font and localized layout opens. Capture a visibly Spanish accented string and provide file path. Do not assume message bytes alone prove rendered accents.
- English and unsupported launcher language (e.g. ru): actual mes_us/E opens. Missing/partial pack availability is covered by unit fixtures; do not rename installed files to simulate it.
- Spanish config deterministic/det-out, demo record and demo replay modes: verify English resource opens. Reuse existing deterministic harness support; avoid mixing incompatible flags. If any mode not run, mark NOT RUN.
- Pair with different launcher configs (en/es): prove both actual English message/font opens, reach gameplay via coop_menu, validate common confirmed hashes equal and controls accepted. Exercise a bounded delayed-input rollback/recovery if feasible, reusing existing relay/history checks; mark gaps.
- All processes exit normally and configs/saves remain isolated. Inspect captures with native image reader.

## Deliverable
Return DONE/DONE_WITH_CONCERNS/BLOCKED, precise files changed, build/test commands with exit codes, run directories and brief results, mode cases NOT RUN, and failures verbatim. Full evidence in task-3-report.md; update progress. Do not weaken tests or hash checking for green results. If step budget approaches40, stop at checkpoint with remaining cases; no blind continue.

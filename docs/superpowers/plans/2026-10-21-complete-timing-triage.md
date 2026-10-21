# Complete timing triage implementation plan

> **For agentic workers:** Use systematic debugging for analysis and executing-plans for each confirmed correction. Preserve other workers' changes.

**Goal:** Review all 3,513 recorded timing candidates, exclude precisely covered fixes, and correct every confirmed frame-dependent gameplay update found by that review.

**Architecture:** Maintain a store-address ledger tied to generation `8f5c8a9f29ed8c00`. Trace units, producers, callers and event/loop cadence before changing gameplay. Apply narrow pacing hooks with retained original bodies where the existing architecture supports them.

**Tech Stack:** Translated PPC C++, native C++ pacing, Python audit, CTest and offline guest-function probes.

**Spec:** User's 2026-10-21 request to examine every remaining candidate while they test the installed build; audit scope and limitations in `docs/testing/timing-audit.md`.

## Constraints

- Preserve current work and installed build while the user plays. No commits or pushes.
- No blind global timestep multiplication or blanket 30 Hz gating.
- Keep event counts, reference counts, vector math and already-timed paths intact.
- Prefer offline tests; any necessary live game uses the existing harness and copied saves.
- An unresolved classification is outstanding work, never proof of safety.

## Review focus

- Saved-register dt carried across calls: trace producer before declaring missing dt.
- Event damage and state-entry setup: establish invocation cadence before scaling.
- Variable interpolation/damping: preserve nominal-frame behavior and document integration limits.
- Pause, mode changes and timestep extremes: regression tests for each correction.
- Hook ABI and scope: preserve original callers, state and retained prologues.

## Work

- [x] Inventory every finding by containing function and store address, with no omissions or duplicate ownership (`docs/testing/timing-triage.json`, exactly 3,513 unique store PCs).
- [x] Review core addresses below `80100000` (1,027 findings / 230 functions).
- [x] Review game addresses `80100000..8025FFFF` (1,075 findings / 427 functions).
- [x] Review SDK addresses from `80260000` (1,411 findings / 519 functions).
- [x] Merge conclusions into a complete ledger; revisit unresolved producers/callers rather than discarding candidates.
- [x] Apply confirmed corrections with bindings and offline regression coverage; red-before-green checks recorded where performed, fixture and runtime limits documented.
- [ ] Run full build/tests, regenerate the audit, review final changes and report remaining validation limits.

Primary code targets are `game/game_pacing.cpp`, `game/game_pacing.h`, `project/hle_map.txt`, `project/uc.cfg` and `tests/test_game_pacing.cpp`. Change other subsystems only when evidence locates the root cause there. Per-region investigation ledgers live in `build/timing_audit/triage-*.json`; the durable consolidated ledger belongs under `docs/testing/`.

## Current checkpoint

**Update, 2026-10-21 evening:** the union is fully classified: 3,165 not frame-based,
359 already timed, 54 fixed, 0 unresolved (SoundWaitStep fixed 8017AC88/8017AF70).
CrossfadeStep (80165610) fixed the monkey AI/movement problem; it is a cross-call
double time factor, outside the store-PC registry. Build 135/135 on
generation_2a48cf24324130e2, installed and SHA-256 verified. Details:
`docs/testing/timing-handoff.md`. The text below is the earlier checkpoint.

The 3,513-store baseline is closed: 3,111 not frame-based, 359 already timed,
43 covered by fixes. The expanded durable union has 3,578 stores: 3,163 not
frame-based, 359 already timed, 52 fixed and **four unresolved**.

Full-build checkpoint: 135/135 passed (14.36 s), before the latest SoundRetryStep
correction. Final baseline review found that missed retry counter; the fix and
focused tests are complete. Scanner tests now pass 16/16 after addic/addic_ support.
A new full build/regeneration and the four extended investigations remain pending.
The installed game is unchanged.

User requested preservation before usage limits: full/fixed JSON and Markdown
exports plus actionable continuation are saved under docs/testing/. Start with
[timing-handoff.md](../../testing/timing-handoff.md). That file records unresolved
addresses, exact verification limits, source/dist/install differences and next steps.

## Subsequent build/install checkpoint

2026-10-21: current source including SoundRetryStep passed the full build, 135/135
tests in 14.58 seconds. Generation bb9b4e9eb8c69408 installed to the game folder;
all 102 manifest files match dist by SHA-256. See docs/testing/timing-latest-deployment.json.
The four extended candidates and re-scan of the new generation remain outstanding;
no live game run was performed. Earlier source/dist/install warnings above are historical.

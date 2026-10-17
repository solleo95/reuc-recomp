# Launcher CPU calibration prototype

User-authorized design: calibrate in the background on first launcher startup,
cache the result, keep settings interactive, and defer an early Play request
until calibration completes. No commits or installation.
Profile lives beside uc_launcher.exe in the game folder (user clarification).
Calibration creates no subprocess/window. Game and harness launches suppress
console creation; SDL game windows remain visible.

Scope: Windows CPU topology, one logical processor per available physical core,
bounded single-thread memory/arithmetic probe, machine/version-bound cache,
launcher startup/Play/close lifecycle, existing guest affinity argument, tests.
Use Windows scheduling when calibration fails or topology is unsupported.
The prototype supports one processor group, matching the game's existing CPU
argument; multiple groups retain Windows scheduling. Synthetic ranking must
be validated against fight3 and is not a promise of the best CPU under all loads.

1. Add topology and score/cache tests; demonstrate failures, implement helpers.
   Resolve SMT siblings from actual topology instead of CPU xor 1.
2. Implement cancellable background calibration and atomic cached result.
   Use three alternating rounds over physical cores; target a few seconds.
   Serialize concurrent launchers with a file lock; no shared mutable results.
3. Connect startup and queued Play, localized waiting state, and launch argument.
   Verify cold/warm startup, close, corruption, and early Play using isolated tests.
4. Build and run tests, launcher smoke, then adjacent fight3 A/B: Windows default
   versus selected CPU, one run each, exact guest work, wall time and peak FPS.
   Record the selection, calibration duration, and limitations. Review final diff.

Execution: inline, existing main checkout preserves accepted uncommitted work.
No new rendering/collision optimization. Calibration tests run before game bench.

Ledger:
- Baseline working changes inspected; launcher uses launch_game and has a native
  ImGui smoke harness. CPU pinning currently assumes adjacent SMT siblings.
- CPU selection and argument tests failed before implementation, then passed.
  Full initial suite: 107/107; added lifecycle checks also pass. Cold native
  launcher smoke: queued Play, CPU 28, 2362.1 ms; warm smoke: CPU 28, 0.4 ms.
- Adjacent fight3, one run/configuration, same executable and exact draw trace:
  Windows 25.026 s / peak 136.0 FPS; CPU 28 23.920 s / peak 142.9 FPS.
  Time -4.4%, peak +5.1%; one-pair 5% noise floor makes acceleration inconclusive.
  Evidence: build/autotest/cpu-calibration-fight3-result.json.
- Final console/path changes: 107/107 CTest, 10/10 benchmark unit tests;
  launcher native smoke reports console=0 in its launched child; real game
  train_gameplay harness passes in 4.0 s. Main game binary is unchanged by
  these launcher/harness-only refinements; no additional performance runs.
- Final review found queued Play was cleared during binding/path editing.
  Native smoke reproduced the failure, then launch gating was corrected to
  preserve the request through transient editing. No other review findings.
- Follow-up CPU 26/28 adjacent fight3 (one each, identical executable SHA and
  exact draw trace): CPU 26 22.57960 s / peak 148.97125; CPU 28 23.30136 s /
  peak 144.95477. CPU 28 +3.2% time, -2.7% peak; within the 5% single-pair
  noise floor. Saved probe had picked CPU 28 on only a 0.53% median advantage.
- Selection v2 treats probe results within 5% of the fastest median as tied,
  using SchedulingClass as a heuristic tie-breaker. Clear measurement leads
  still win; missing/equal hints retain the fastest median. No CPU IDs are
  hardcoded. Profile version and topology fingerprint invalidate old caches.
  Regression reproduced with recorded CPU 26/28 samples before the change.
  Fresh native launcher calibration selected CPU 26 in 2336.1 ms and smoke passed.
  Comparison artifacts: build/autotest/cpu-26-vs-28-result.json and archived
  build/game/uc_cpu_26_28_compared.exe. No extra fight3 runs after selector changes.
- First full test run had an unrelated test_save_hle:22 failure; isolated rerun
  passed. That fixture uses a PID-only temporary folder without cleanup, so the
  final suite is run with a fresh TEMP directory to avoid stale saved files.
